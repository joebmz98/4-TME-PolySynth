/*
  4-TME PolySynth prototype v11.5 — 4 voices, 16 encoders, full per-voice panel, MIDI in
  Daisy Seed / DaisyDuino (Arduino IDE)

  Per voice: PolyBLEP saw -> MoogLadder LPF -> ADSR VCA -> pan/level
             triangle LFO -> pitch and cutoff
             ADSR -> amp, pitch (amount) and cutoff (amount)
  Voice buttons choose which voice the encoders edit (LED shows it).
  MIDI notes (any channel) cycle round-robin: 1 -> 2 -> 3 -> 4 -> 1 ...

  ---------------------------------------------------------------- ENCODERS
  Every encoder uses two neighbouring channels: A on the even channel, B on the odd.

   MUX 1 (SIG -> D15)                       MUX 2 (SIG -> D16)
   C0/C1   tune          ±12 st              C0/C1   cutoff CV amount  (not yet)
   C2/C3   wave scrub    tri>saw>sq>PWM      C2/C3   attack     1 ms .. 10 s
   C4/C5   pitch env amt ±24 st              C4/C5   decay      1 ms .. 10 s
   C6/C7   pitch LFO amt 0 .. 12 st          C6/C7   sustain    0 .. 100 %
   C8/C9   cutoff        20 Hz .. 18 kHz     C8/C9   release    1 ms .. 10 s
   C10/C11 resonance     0 .. 0.9            C10/C11 LFO rate   0.05 .. 25 Hz
   C12/C13 cutoff env    ±5 oct              C12/C13 pan        L .. R
   C14/C15 cutoff LFO    0 .. 4 oct          C14/C15 level      0 .. 100 %
  Spinning fast moves in bigger steps.

  Wave scrub (150 clicks end to end):
     0 triangle -> 50 sawtooth -> 100 square (50 %) -> 150 narrow pulse (5 %)
  Uses DaisySP's VariableShapeOscillator (ported from Mutable Instruments
  Plaits): one band-limited oscillator that morphs continuously
  triangle -> saw -> square, with pulse width once it reaches the square.

  ---------------------------------------------------------------- WIRING
    Both muxes share the select lines (S0 to S0, S1 to S1 ...):
      S0 D0   S1 D1   S2 D2   S3 D3
    Mux 1 SIG  D15 (physical pin 22)
    Mux 2 SIG  D16 (physical pin 23)
    Both muxes: EN -> GND, VCC -> 3V3, GND -> DGND
    Encoder middle pin -> GND (all 16)
    10k pull-ups from every used mux channel to 3V3 recommended

    Buttons 1-4  D4, D5, D6, D7          to GND, internal pull-ups
    LEDs 1-4     D17, D18, D19, D20      pin -> 1k -> LED -> GND
    MIDI in      6N138 output -> D14 (physical pin 15)

  ---------------------------------------------------------------- AUDIO TESTS
  BOOT_BEEP 1     : 0.5 s 440 Hz sine straight to the outputs at power-up,
                    bypassing the voices. No beep = codec / output wiring / block size.
  AUDIO_TEST_MODE : 0 normal
                    1 each voice = raw saw x envelope (filter bypassed)
                    2 a constant 220 Hz saw on both outs (ignores MIDI)
  If BOOT_BEEP works but normal mode is silent, try modes 1 and 2 and tell me
  which ones make sound.

  ---------------------------------------------------------------- DEBUG
  DEBUG_SERIAL 1 prints every edit, and every 2 s a status line:
    CPU avg / max   audio callback time as % of the block (cycle counter)
    loops/s         how often loop() runs (MIDI + encoders are read there)
    note-ons        MIDI notes the sketch has actually received
  (Tools > USB support: CDC, 115200 baud). Leave at 0 for normal use.

  If CPU max is near or over 100 %: set Tools > Optimize to "Fastest (-O3)"
  (the Arduino default is "Smallest", much slower for DSP), and/or set
  FILTER_TYPE 1 for the lighter Svf filter.
*/

#include "DaisyDuino.h"
#include <MIDI.h>   // FortySevenEffects MIDI Library (same as WEASEL)

// ------------------------------------------------------------------ config
#define DEBUG_SERIAL        0
#define BOOT_BEEP           0          // 1 = 0.5 s test tone at power-up
#define AUDIO_TEST_MODE     0
#define FILTER_TYPE         0          // 0 = MoogLadder (24 dB, heavier), 1 = Svf (12 dB, much lighter)

#define NUM_VOICES          4
#define MIDI_CHANNEL        0          // 0 = omni (any channel), or 1-16
#define BEND_RANGE_ST       2.0f

#define OSC_LEVEL           0.4f
#define MIX_GAIN            0.5f       // output scaling for the 4-voice sum
#define CUTOFF_MIN_HZ       20.0f
#define CUTOFF_MAX_HZ       18000.0f
#define SMOOTH_MS           10.0f

// MIDI reliability: keep each audio callback short so the UART interrupt gets in
#define AUDIO_BLOCK_SIZE    16         // samples per callback (333 us at 48 kHz)
#define CONTROL_RATE        8          // pitch / cutoff / LFO update every N samples

#define ENC_STEPS_PER_DETENT 4         // try 2 if every other click does nothing
#define ENC_REVERSE          0         // 1 = flip direction of all encoders
#define ENC_SETTLE_US        10        // mux settle time before reading (longer = less noise)
#define ENC_FILTER           1         // 1 = a pin state must read the same twice before it counts

#define BEND_ENABLE          1         // 0 = ignore MIDI pitch bend (the OP-XY streams it constantly)

#define DEBOUNCE_MS          20
#define LED_FLASH_MS         60        // how long a voice LED blinks when that voice is triggered
#define LED_FLASH_ENABLE     1         // 0 = no trigger flashes (test: do the false encoder edits stop?)
#define ENC_BLANK_US         2000      // skip encoder reads for this long after any LED pin changes

const int MUX_S[4]  = { D0, D1, D2, D3 };
const int MUX1_SIG  = D15;
const int MUX2_SIG  = D16;
const int BTN_PINS[NUM_VOICES] = { D4,  D5,  D6,  D7  };
const int LED_PINS[NUM_VOICES] = { D17, D18, D19, D20 };

#define MIDI_RX_PIN 14   // Seed D14 / physical pin 15 (USART1 RX), as on WEASEL
MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);

// ------------------------------------------------------------------ parameters
// One parameter per encoder, same order as the encoders (mux 1 C0/C1 first).
enum Param {
  P_TUNE = 0, P_WAVE, P_PITCH_ENV, P_PITCH_LFO,
  P_CUTOFF, P_RES, P_FLT_ENV, P_FLT_LFO,
  P_FLT_CV, P_ATTACK, P_DECAY, P_SUSTAIN,
  P_RELEASE, P_LFO_RATE, P_PAN, P_LEVEL,
  P_COUNT
};
#define NUM_ENC P_COUNT

// Each parameter is a whole number 0..PARAM_MAX; one encoder click = 1.
// PARAM_MAX 0 = encoder read but not used yet.
const int16_t PARAM_MAX[P_COUNT] = {
  240, 150,  96, 100,     // tune 0.1 st steps, wave scrub, pitch env 0.5 st steps, pitch LFO
   98, 100, 100, 100,     // cutoff 0.1 oct steps, res, cutoff env 0.1 oct steps, cutoff LFO
    0, 100, 100, 100,     // CV amt (later), attack, decay, sustain
  100, 100, 100, 100      // release, LFO rate, pan, level
};
const int16_t PARAM_DEFAULT[P_COUNT] = {
  120,  50,  48,   0,     // tune 0, sawtooth, pitch env 0, no pitch LFO
   56,  33,  50,   0,     // cutoff ~1 kHz, res 0.3, cutoff env 0, no cutoff LFO
    0,  17,  62,  70,     // -, attack 5 ms, decay 300 ms, sustain 70 %
   62,  66,  50,  80      // release 300 ms, LFO ~3 Hz, pan centre, level 80 %
};
// Per-encoder direction: 1 = flip that encoder (on top of ENC_REVERSE for all)
const bool PARAM_FLIP[P_COUNT] = {
  0, 0, 0, 0,     // tune, wave, pitch env, pitch LFO
  0, 1, 0, 0,     // cutoff, RESONANCE, cutoff env, cutoff LFO
  0, 1, 1, 1,     // cutoff CV, ATTACK, DECAY, SUSTAIN
  0, 1, 1, 0      // release, LFO RATE, PAN, level
};

// Per-encoder direction flip (1 = reversed), same order as the encoders.
// Set to match the wiring so turning clockwise always increases the value.
const bool ENC_INVERT[P_COUNT] = {
  0, 0, 0, 0,     // tune, wave, pitch env, pitch LFO
  0, 1, 0, 0,     // cutoff, res, cutoff env, cutoff LFO
  0, 1, 1, 1,     // cutoff CV, attack, decay, sustain
  1, 1, 0, 0      // release, LFO rate, pan, level
};

const char *PARAM_NAME[P_COUNT] = {
  "tune", "wave", "pitch env", "pitch lfo", "cutoff", "res", "cutoff env", "cutoff lfo",
  "cutoff cv", "attack", "decay", "sustain", "release", "lfo rate", "pan", "level"
};

// Types used in function signatures go before any function: the Arduino IDE
// puts its auto-generated prototypes above the first function in the file.
struct MuxEncoder {
  uint8_t  prev;        // last accepted A/B state
  uint8_t  pending;     // candidate new state, waiting to be confirmed
  int8_t   acc;
  uint32_t lastClick;
};

// Per-voice settings in real units. Written by loop() (encoders), read by audio.
struct VoiceSettings {
  volatile float tune;        // semitones
  volatile float wave;        // 0 tri, 1 saw, 2 square, 3 narrowest pulse
  volatile float pitchEnv;    // semitones at full envelope
  volatile float pitchLfo;    // semitones at full LFO swing
  volatile float cutoffOct;   // octaves above CUTOFF_MIN_HZ
  volatile float res;         // 0..0.9
  volatile float fltEnv;      // octaves at full envelope
  volatile float fltLfo;      // octaves at full LFO swing
  volatile float attack, decay, sustain, release;   // seconds / level
  volatile float lfoRate;     // Hz
  volatile float gainL, gainR;                      // pan x level
};

// ------------------------------------------------------------------ helpers
static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline float timeFromNorm(float n) { return 0.001f * powf(10000.0f, n); }   // 1 ms .. 10 s
static inline float noteToHz(float note)  { return 440.0f * exp2f((note - 69.0f) / 12.0f); }

const float CUTOFF_MAX_OCT = log2f(CUTOFF_MAX_HZ / CUTOFF_MIN_HZ);

DaisyHardware hw;
float sampleRate = 48000.0f;
float smoothK    = 0.002f;     // per-sample smoothing
float smoothKc   = 0.016f;     // same smoothing time, applied every CONTROL_RATE samples

// audio callback timing with the CPU cycle counter (works inside interrupts)
volatile uint32_t cbMaxCycles = 0, cbSumCycles = 0, cbCount = 0;
volatile uint32_t noteOnCount = 0;
uint32_t midiMsgCount = 0, midiBendCount = 0, midiClockCount = 0, midiNoteCount = 0, midiOtherCount = 0;

// read and count every MIDI message (callbacks still do the actual work)
void pollMidi() {
  while (MIDI.read()) {
    midiMsgCount++;
    switch (MIDI.getType()) {
      case midi::NoteOn:
      case midi::NoteOff:    midiNoteCount++;  break;
      case midi::PitchBend:  midiBendCount++;  break;
      case midi::Clock:      midiClockCount++; break;
      default:               midiOtherCount++; break;
    }
  }
}
volatile uint32_t nanCount = 0; // output samples that came out as NaN / inf

Oscillator testOsc;             // boot beep + test mode 2
volatile uint32_t beepLeft = 0; // samples of boot beep still to play

// ------------------------------------------------------------------ voice
struct Voice {
  VariableShapeOscillator osc;            // tri -> saw -> square -> PWM morph
  Oscillator lfo;
#if FILTER_TYPE == 1
  Svf        filt;
#else
  MoogLadder filt;
#endif
  Adsr       env;
  VoiceSettings set;
  int16_t    param[P_COUNT];   // encoder positions (loop side)

  // note state, written by loop() (MIDI), read by the audio callback
  volatile int      note      = 60;
  volatile int      heldNote  = -1;    // -1 = key released
  volatile bool     gate      = false;
  volatile uint32_t trigCount = 0;

  // audio-thread state
  uint32_t trigSeen = 0;
  float aApplied = -1, dApplied = -1, sApplied = -1, rApplied = -1, lfoApplied = -1;
  float tuneC, pEnvC, pLfoC, cutC, resC, fEnvC, fLfoC;   // smoothed (control rate)
  float waveC;                                            // smoothed wave position
  float oscGain = 1.0f, oscDc = 0.0f;                     // level match + DC removal for the morph
  float gLC, gRC;                                         // smoothed (audio rate)
  float lfoVal = 0.0f;
  bool  g = false;
  float baseNote = 60.0f;

  void init(float sr) {
    osc.Init(sr);
    osc.SetSync(false);
    osc.SetFreq(220.0f);
    osc.SetSyncFreq(220.0f);    // with sync off, this is the frequency you hear
    osc.SetWaveshape(0.5f);     // saw
    osc.SetPW(0.5f);

    lfo.Init(sr / CONTROL_RATE);              // LFO only runs at the control rate
    lfo.SetWaveform(Oscillator::WAVE_TRI);
    lfo.SetAmp(1.0f);

    filt.Init(sr);
#if FILTER_TYPE == 1
    filt.SetDrive(0.0f);
#endif
    env.Init(sr);
  }

  // snap smoothed values to their targets (used at boot)
  void settle() {
    tuneC = set.tune;   pEnvC = set.pitchEnv; pLfoC = set.pitchLfo;
    waveC = set.wave;   updateWave();
    cutC  = set.cutoffOct; resC = set.res; fEnvC = set.fltEnv; fLfoC = set.fltLfo;
    gLC   = set.gainL;  gRC = set.gainR;
  }

  void beginBlock(float bend) {
    // only touch the ADSR / LFO when their settings changed
    float a = set.attack, d = set.decay, s = set.sustain, r = set.release, lr = set.lfoRate;
    if (a != aApplied) { env.SetTime(ADSR_SEG_ATTACK,  a); aApplied = a; }
    if (d != dApplied) { env.SetTime(ADSR_SEG_DECAY,   d); dApplied = d; }
    if (s != sApplied) { env.SetSustainLevel(s);           sApplied = s; }
    if (r != rApplied) { env.SetTime(ADSR_SEG_RELEASE, r); rApplied = r; }
    if (lr != lfoApplied) { lfo.SetFreq(lr);               lfoApplied = lr; }

    g = gate;
    if (trigCount != trigSeen) {          // new note on this voice
      trigSeen = trigCount;
      if (g) env.Retrigger(false);        // restart attack from the current level
    }
    baseNote = note + bend;
  }

  // wave position (0..3) -> VariableShapeOscillator shape and pulse width
  //   waveshape 0 = triangle, 0.5 = saw, 1 = square; pw only matters at the square
  //   (at 0.5 it also keeps the triangle symmetrical)
  // The raw morph isn't level-matched (square is ~5 dB louder than saw) and
  // narrow pulses carry a big DC offset, so estimate the RMS / DC of the
  // current shape and correct both. Target RMS = the saw's (~0.58).
  void updateWave() {
    float w = clampf(waveC, 0.0f, 3.0f);
    float rms;
    if (w <= 2.0f) {                      // triangle -> saw -> square
      osc.SetWaveshape(w * 0.5f);
      osc.SetPW(0.5f);
      oscDc = 0.0f;
      rms = (w <= 1.0f) ? 0.58f - 0.68f * w * (1.0f - w)      // tri .58, mid .41, saw .57
                        : 0.57f + 0.43f * (w - 1.0f);         // saw .57 -> square 1.0
    } else {                              // square, pulse width 50 % -> 5 %
      float pw = 0.5f - (w - 2.0f) * 0.45f;
      osc.SetWaveshape(1.0f);
      osc.SetPW(pw);
      oscDc = 1.0f - 2.0f * pw;           // pulse spends (1 - pw) of the cycle at +1
      rms = 2.0f * sqrtf(pw * (1.0f - pw));
    }
    oscGain = clampf(0.58f / rms, 0.5f, 1.5f);
  }

  inline float oscOut() { return (osc.Process() - oscDc) * oscGain * OSC_LEVEL; }

  // returns mono voice output; L/R gains are applied by the caller
  float process(bool ctl) {
    float e = env.Process(g);

    if (ctl) {
      tuneC += smoothKc * (set.tune      - tuneC);
      pEnvC += smoothKc * (set.pitchEnv  - pEnvC);
      pLfoC += smoothKc * (set.pitchLfo  - pLfoC);
      cutC  += smoothKc * (set.cutoffOct - cutC);
      resC  += smoothKc * (set.res       - resC);
      fEnvC += smoothKc * (set.fltEnv    - fEnvC);
      fLfoC += smoothKc * (set.fltLfo    - fLfoC);
      waveC += smoothKc * (set.wave      - waveC);

      lfoVal = lfo.Process();             // -1 .. +1

      float semis = baseNote + tuneC + pEnvC * e + pLfoC * lfoVal;
      float hz = clampf(noteToHz(semis), 1.0f, sampleRate * 0.45f);
      osc.SetFreq(hz);
      osc.SetSyncFreq(hz);
      updateWave();                       // after the frequency: SetPW limits depend on it

      float fcOct = clampf(cutC + fEnvC * e + fLfoC * lfoVal, 0.0f, CUTOFF_MAX_OCT);
      filt.SetFreq(CUTOFF_MIN_HZ * exp2f(fcOct));
      filt.SetRes(resC);
    }

    gLC += smoothK * (set.gainL - gLC);
    gRC += smoothK * (set.gainR - gRC);

#if AUDIO_TEST_MODE == 1
    return oscOut() * e;             // filter bypassed
#elif FILTER_TYPE == 1
    filt.Process(oscOut());
    return filt.Low() * e;
#else
    return filt.Process(oscOut()) * e;
#endif
  }
};

Voice voices[NUM_VOICES];
volatile float bendSt = 0.0f;

// encoder position -> real units for one parameter of one voice (loop side)
void applyParam(int v, int id) {
  Voice &vc = voices[v];
  VoiceSettings &s = vc.set;
  int16_t p = vc.param[id];
  float n = PARAM_MAX[id] ? (float)p / PARAM_MAX[id] : 0.0f;

  switch (id) {
    case P_TUNE:      s.tune      = (p - 120) * 0.1f; break;
    case P_WAVE:      s.wave      = p / 50.0f; break;                // 0..3
    case P_PITCH_ENV: s.pitchEnv  = (p - 48) * 0.5f; break;
    case P_PITCH_LFO: s.pitchLfo  = n * n * 12.0f; break;          // fine control at low amounts
    case P_CUTOFF:    s.cutoffOct = p * 0.1f; break;
    case P_RES:       s.res       = n * 0.9f; break;
    case P_FLT_ENV:   s.fltEnv    = (p - 50) * 0.1f; break;
    case P_FLT_LFO:   s.fltLfo    = n * n * 4.0f; break;
    case P_ATTACK:    s.attack    = timeFromNorm(n); break;
    case P_DECAY:     s.decay     = timeFromNorm(n); break;
    case P_SUSTAIN:   s.sustain   = n; break;
    case P_RELEASE:   s.release   = timeFromNorm(n); break;
    case P_LFO_RATE:  s.lfoRate   = 0.05f * exp2f(n * 9.0f); break; // 0.05 .. 25.6 Hz
    case P_PAN:
    case P_LEVEL: {                                                  // equal-power pan x level
      float pan   = (float)vc.param[P_PAN] / PARAM_MAX[P_PAN];
      float level = (float)vc.param[P_LEVEL] / PARAM_MAX[P_LEVEL];
      float a = pan * 0.5f * PI_F;
      s.gainL = cosf(a) * level;
      s.gainR = sinf(a) * level;
      break;
    }
    default: break;                                                  // cutoff CV: later
  }
}

#if DEBUG_SERIAL
void printParam(int v, int id) {
  const VoiceSettings &s = voices[v].set;
  Serial.print("v"); Serial.print(v + 1); Serial.print(" ");
  Serial.print(PARAM_NAME[id]); Serial.print(": ");
  switch (id) {
    case P_TUNE:      Serial.print(s.tune, 1);      Serial.println(" st");  break;
    case P_WAVE: {
      float w = s.wave;
      if      (w <= 0.0f) Serial.println("triangle");
      else if (w <  1.0f) { Serial.print("tri/saw "); Serial.print((int)(w * 100)); Serial.println("% saw"); }
      else if (w == 1.0f) Serial.println("sawtooth");
      else if (w <  2.0f) { Serial.print("saw/square "); Serial.print((int)((w - 1.0f) * 100)); Serial.println("% square"); }
      else { Serial.print("pulse width "); Serial.print(50.0f - (w - 2.0f) * 45.0f, 1); Serial.println("%"); }
      break;
    }
    case P_PITCH_ENV: Serial.print(s.pitchEnv, 1);  Serial.println(" st");  break;
    case P_PITCH_LFO: Serial.print(s.pitchLfo, 2);  Serial.println(" st");  break;
    case P_CUTOFF:    Serial.print(CUTOFF_MIN_HZ * exp2f(s.cutoffOct), 0); Serial.println(" Hz"); break;
    case P_RES:       Serial.println(s.res, 2); break;
    case P_FLT_ENV:   Serial.print(s.fltEnv, 1);    Serial.println(" oct"); break;
    case P_FLT_LFO:   Serial.print(s.fltLfo, 2);    Serial.println(" oct"); break;
    case P_ATTACK:    Serial.print(s.attack, 3);    Serial.println(" s");   break;
    case P_DECAY:     Serial.print(s.decay, 3);     Serial.println(" s");   break;
    case P_SUSTAIN:   Serial.println(s.sustain, 2); break;
    case P_RELEASE:   Serial.print(s.release, 3);   Serial.println(" s");   break;
    case P_LFO_RATE:  Serial.print(s.lfoRate, 2);   Serial.println(" Hz");  break;
    default:          Serial.println(voices[v].param[id]); break;
  }
}
#endif

// ------------------------------------------------------------------ audio
void AudioCallback(float **in, float **out, size_t size) {
  uint32_t c0 = DWT->CYCCNT;

  // boot beep / test mode 2: bypass the voices completely
  if (beepLeft > 0 || AUDIO_TEST_MODE == 2) {
    for (size_t i = 0; i < size; i++) {
      float s = testOsc.Process();
      out[0][i] = s;
      out[1][i] = s;
    }
    if (beepLeft > 0) beepLeft = beepLeft > size ? beepLeft - size : 0;
    uint32_t dc = DWT->CYCCNT - c0;
    if (dc > cbMaxCycles) cbMaxCycles = dc;
    cbSumCycles += dc; cbCount++;
    return;
  }

  float bend = bendSt;
  for (int v = 0; v < NUM_VOICES; v++) voices[v].beginBlock(bend);

  for (size_t i = 0; i < size; i++) {
    bool ctl = (i % CONTROL_RATE) == 0;
    float l = 0.0f, r = 0.0f;
    for (int v = 0; v < NUM_VOICES; v++) {
      float s = voices[v].process(ctl);
      l += s * voices[v].gLC;
      r += s * voices[v].gRC;
    }
    l *= MIX_GAIN;
    r *= MIX_GAIN;
    if (!(fabsf(l) < 100.0f) || !(fabsf(r) < 100.0f)) { nanCount++; l = 0.0f; r = 0.0f; }   // catches NaN and inf
    out[0][i] = l;
    out[1][i] = r;
  }

  uint32_t dc = DWT->CYCCNT - c0;
  if (dc > cbMaxCycles) cbMaxCycles = dc;
  cbSumCycles += dc; cbCount++;
}

// ------------------------------------------------------------------ voice select buttons + LEDs
int      selVoice = 0;                // the voice the encoders edit
bool     btnStable[NUM_VOICES];
bool     btnLast[NUM_VOICES];
uint32_t btnChanged[NUM_VOICES];

// LED = selected voice (steady on), and blinks when its voice is triggered.
// The selected voice's LED blinks OFF instead, so a trigger is visible on it too.
uint32_t ledFlashStart[NUM_VOICES];
volatile uint32_t lastLedChangeUs = 0;    // encoder reads pause briefly after an LED switches
bool     ledFlashing[NUM_VOICES];
int8_t   ledState[NUM_VOICES] = { -1, -1, -1, -1 };   // -1 = unknown, forces the first write

void flashVoiceLed(int v) {
#if LED_FLASH_ENABLE
  ledFlashStart[v] = millis();
  ledFlashing[v]   = true;
#endif
}

void updateLeds() {
  uint32_t now = millis();
  for (int i = 0; i < NUM_VOICES; i++) {
    if (ledFlashing[i] && now - ledFlashStart[i] >= LED_FLASH_MS) ledFlashing[i] = false;
    bool on = (i == selVoice);
    if (ledFlashing[i]) on = !on;
    if (ledState[i] != (int8_t)on) {      // only touch the pin when it changes
      digitalWrite(LED_PINS[i], on ? HIGH : LOW);
      ledState[i] = on;
      lastLedChangeUs = micros();
    }
  }
}

void selectVoice(int v) {
  selVoice = v;
  updateLeds();
#if DEBUG_SERIAL
  Serial.print("editing voice "); Serial.println(v + 1);
#endif
}

void initVoiceSelect() {
  for (int i = 0; i < NUM_VOICES; i++) {
    pinMode(BTN_PINS[i], INPUT_PULLUP);
    pinMode(LED_PINS[i], OUTPUT);
    digitalWrite(LED_PINS[i], LOW);
    btnStable[i] = HIGH; btnLast[i] = HIGH; btnChanged[i] = 0;
    ledFlashing[i] = false; ledFlashStart[i] = 0;
  }
  for (int i = 0; i < NUM_VOICES; i++) {      // LED check: 1 -> 4 in turn
    digitalWrite(LED_PINS[i], HIGH); delay(120);
    digitalWrite(LED_PINS[i], LOW);
  }
  selectVoice(0);                             // voice 1 = D17 on at boot
}

void scanButtons() {
  uint32_t now = millis();
  for (int i = 0; i < NUM_VOICES; i++) {
    bool raw = digitalRead(BTN_PINS[i]);
    if (raw != btnLast[i]) { btnLast[i] = raw; btnChanged[i] = now; }
    if (raw != btnStable[i] && now - btnChanged[i] >= DEBOUNCE_MS) {
      btnStable[i] = raw;
      if (raw == LOW) selectVoice(i);
    }
  }
}

// ------------------------------------------------------------------ encoders (2 muxes, 16 encoders)
MuxEncoder enc[NUM_ENC];
const int8_t QDEC[16] = { 0, -1, 1, 0,  1, 0, 0, -1,  -1, 0, 0, 1,  0, 1, -1, 0 };

// read all 16 channels of both muxes in one sweep (the select lines are shared)
void readMuxes(uint16_t &bits1, uint16_t &bits2) {
  bits1 = 0; bits2 = 0;
  for (int ch = 0; ch < 16; ch++) {
    for (int b = 0; b < 4; b++) digitalWrite(MUX_S[b], (ch >> b) & 1);
    delayMicroseconds(ENC_SETTLE_US);
    if (digitalRead(MUX1_SIG)) bits1 |= (1u << ch);
    if (digitalRead(MUX2_SIG)) bits2 |= (1u << ch);
  }
}

// encoder i: mux 1 for 0-7, mux 2 for 8-15; A on channel 2k, B on 2k+1
static inline uint8_t encoderPins(int i, uint16_t bits1, uint16_t bits2) {
  uint16_t bits = (i < 8) ? bits1 : bits2;
  int k = i % 8;
  uint8_t a = (bits >> (2 * k)) & 1;
  uint8_t b = (bits >> (2 * k + 1)) & 1;
  return ENC_REVERSE ? (b << 1 | a) : (a << 1 | b);
}

// returns 0, or a signed step size (bigger when spun fast)
int decodeEncoder(int i, uint8_t s) {
  MuxEncoder &e = enc[i];
  if (s == e.prev) { e.pending = s; return 0; }
#if ENC_FILTER
  // noise filter: a new A/B state only counts once it reads the same on two
  // scans in a row. A single glitchy read from a mux channel is ignored.
  if (s != e.pending) { e.pending = s; return 0; }
#endif
  e.acc += QDEC[(e.prev << 2) | s];
  e.prev = s;

  int click = 0;
  if      (e.acc >=  ENC_STEPS_PER_DETENT) { click =  1; e.acc -= ENC_STEPS_PER_DETENT; }
  else if (e.acc <= -ENC_STEPS_PER_DETENT) { click = -1; e.acc += ENC_STEPS_PER_DETENT; }
  if (!click) return 0;

  uint32_t now = millis();
  uint32_t dt  = now - e.lastClick;
  e.lastClick  = now;
  return click * (dt < 25 ? 5 : (dt < 60 ? 2 : 1));
}

void scanEncoders() {
  // An LED switching can disturb the encoder lines for a moment (shared ground,
  // or wires next to each other: LED pins 24-27 sit beside the mux SIG pins 22-23).
  // Skip reads just after any LED change; a real turn holds each state for ms, so
  // nothing is missed.
  if (micros() - lastLedChangeUs < ENC_BLANK_US) return;

  uint16_t bits1, bits2;
  readMuxes(bits1, bits2);

  for (int i = 0; i < NUM_ENC; i++) {
    int d = decodeEncoder(i, encoderPins(i, bits1, bits2));
    if (!d || PARAM_MAX[i] == 0) continue;          // no movement, or not implemented yet
    if (ENC_INVERT[i]) d = -d;
    if (PARAM_FLIP[i]) d = -d;

    Voice &vc = voices[selVoice];
    int p = vc.param[i] + d;
    if (p < 0) p = 0;
    if (p > PARAM_MAX[i]) p = PARAM_MAX[i];
    if (p == vc.param[i]) continue;
    vc.param[i] = p;
    applyParam(selVoice, i);
#if DEBUG_SERIAL
    printParam(selVoice, i);
#endif
  }
}

// ------------------------------------------------------------------ MIDI in (round-robin)
int nextVoice = 0;   // the voice the next note will play on

void updateNoteLed() {
  static int8_t lastState = -1;
  bool any = false;
  for (int v = 0; v < NUM_VOICES; v++) if (voices[v].heldNote >= 0) any = true;
  if (lastState != (int8_t)any) {
    digitalWrite(LED_BUILTIN, any ? HIGH : LOW);
    lastState = any;
    lastLedChangeUs = micros();
  }
}

void noteOn(uint8_t n) {
  noteOnCount++;
  int v = nextVoice;
  nextVoice = (nextVoice + 1) % NUM_VOICES;

  Voice &vc = voices[v];
  vc.note     = n;
  vc.heldNote = n;
  vc.trigCount++;          // retriggers the envelope even if this voice is still sounding
  vc.gate     = true;
  updateNoteLed();
  flashVoiceLed(v);
#if DEBUG_SERIAL
  Serial.print("note on "); Serial.print(n); Serial.print(" -> voice "); Serial.println(v + 1);
#endif
}

void noteOff(uint8_t n) {
  for (int v = 0; v < NUM_VOICES; v++) {
    if (voices[v].heldNote == n) {
      voices[v].heldNote = -1;
      voices[v].gate     = false;
    }
  }
  updateNoteLed();
}

void allNotesOff() {
  for (int v = 0; v < NUM_VOICES; v++) { voices[v].heldNote = -1; voices[v].gate = false; }
  updateNoteLed();
}

// MIDI library callbacks (called from MIDI.read() in loop)
void handleNoteOn(byte channel, byte note, byte velocity)  { noteOn(note); }   // vel 0 arrives as note-off
void handleNoteOff(byte channel, byte note, byte velocity) { noteOff(note); }
void handlePitchBend(byte channel, int bend) {
#if BEND_ENABLE
  bendSt = bend / 8192.0f * BEND_RANGE_ST;
#endif
}
void handleControlChange(byte channel, byte cc, byte value) {
  if (cc == 120 || cc == 123) allNotesOff();
}

// ------------------------------------------------------------------ setup / loop
void setup() {
#if DEBUG_SERIAL
  Serial.begin(115200);
#endif

  // enable the Cortex-M7 cycle counter for CPU load measurement
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->LAR = 0xC5ACCE55;            // unlock (needed on M7)
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  // same order as WEASEL: init, sample rate, block size
  hw = DAISY.init(DAISY_SEED, AUDIO_SR_48K);
  sampleRate = DAISY.get_samplerate();
  DAISY.SetAudioBlockSize(AUDIO_BLOCK_SIZE);

  testOsc.Init(sampleRate);
#if AUDIO_TEST_MODE == 2
  testOsc.SetWaveform(Oscillator::WAVE_POLYBLEP_SAW);
  testOsc.SetFreq(220.0f);
#else
  testOsc.SetWaveform(Oscillator::WAVE_SIN);
  testOsc.SetFreq(440.0f);
#endif
  testOsc.SetAmp(0.3f);
#if BOOT_BEEP
  beepLeft = (uint32_t)(sampleRate * 0.5f);
#endif
  smoothK  = 1.0f - expf(-1.0f / (SMOOTH_MS * 0.001f * sampleRate));
  smoothKc = 1.0f - expf(-(float)CONTROL_RATE / (SMOOTH_MS * 0.001f * sampleRate));

  // muxes
  for (int b = 0; b < 4; b++) { pinMode(MUX_S[b], OUTPUT); digitalWrite(MUX_S[b], LOW); }
  pinMode(MUX1_SIG, INPUT_PULLUP);
  pinMode(MUX2_SIG, INPUT_PULLUP);
  uint16_t bits1, bits2;
  readMuxes(bits1, bits2);
  for (int i = 0; i < NUM_ENC; i++) {
    enc[i].prev = encoderPins(i, bits1, bits2);
    enc[i].pending = enc[i].prev;
    enc[i].acc = 0;
    enc[i].lastClick = 0;
  }

  // voices + defaults (all voices start panned centre)
  for (int v = 0; v < NUM_VOICES; v++) {
    voices[v].init(sampleRate);
    for (int id = 0; id < P_COUNT; id++) voices[v].param[id] = PARAM_DEFAULT[id];
    for (int id = 0; id < P_COUNT; id++) applyParam(v, id);
    voices[v].settle();
  }

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  initVoiceSelect();

  DAISY.begin(AudioCallback);

  // MIDI: started after the audio, in the same order as WEASEL
  Serial1.setRx(MIDI_RX_PIN);
  Serial1.begin(31250);
  Serial1.flush();

  MIDI.setHandleNoteOn(handleNoteOn);
  MIDI.setHandleNoteOff(handleNoteOff);
  MIDI.setHandlePitchBend(handlePitchBend);
  MIDI.setHandleControlChange(handleControlChange);
  MIDI.begin(MIDI_CHANNEL == 0 ? MIDI_CHANNEL_OMNI : MIDI_CHANNEL);
  MIDI.turnThruOff();
}

void loop() {
  // read MIDI between every other job so the UART never sits unread for long
  pollMidi();
  scanButtons();
  pollMidi();
  scanEncoders();
  updateLeds();

#if DEBUG_SERIAL
  static uint32_t lastPrint = 0, loopCount = 0;
  loopCount++;
  if (millis() - lastPrint > 2000) {
    float elapsed = (millis() - lastPrint) * 0.001f;
    lastPrint = millis();

    float blockCycles = (float)SystemCoreClock * AUDIO_BLOCK_SIZE / sampleRate;
    uint32_t n = cbCount ? cbCount : 1;
    float avgPct = 100.0f * ((float)cbSumCycles / n) / blockCycles;
    float maxPct = 100.0f * (float)cbMaxCycles / blockCycles;

    Serial.print("CPU avg "); Serial.print(avgPct, 1);
    Serial.print("% max "); Serial.print(maxPct, 1);
    Serial.print("% | loops/s "); Serial.print((uint32_t)(loopCount / elapsed));
    Serial.print(" | note-ons "); Serial.print(noteOnCount);
    Serial.print(" | MIDI msgs "); Serial.print(midiMsgCount);
    Serial.print(" (notes "); Serial.print(midiNoteCount);
    Serial.print(" bend "); Serial.print(midiBendCount);
    Serial.print(" clock "); Serial.print(midiClockCount);
    Serial.print(" other "); Serial.print(midiOtherCount); Serial.print(")");
    Serial.print(" | NaN "); Serial.print(nanCount);
    Serial.print(" | clock MHz "); Serial.println(SystemCoreClock / 1000000);

    cbMaxCycles = 0; cbSumCycles = 0; cbCount = 0; loopCount = 0;
  }
#endif
}