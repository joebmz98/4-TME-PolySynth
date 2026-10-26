/*
  SawFilterTest v5 — saw -> filter -> AR envelope, 5 encoders, MIDI in
  Daisy Seed / DaisyDuino (Arduino IDE)

  - One PolyBLEP sawtooth -> MoogLadder low-pass -> AR amp envelope -> both outs
  - Mono, played from MIDI in: last-note priority (hold a note, play another,
    let go and it returns to the held one), pitch bend ±2 semitones
  - Seed's onboard LED lights while a note is held (quick MIDI check)
  - 5 rotary encoders on one mux:

      Encoder   A pin   B pin   Controls
      1         C0      C1      tune      ±12 semitones, 0.1 st per click
      2         C2      C3      cutoff    20 Hz .. 18 kHz, 0.1 oct per click
      3         C4      C5      attack    1 ms .. 10 s
      4         C6      C7      release   1 ms .. 10 s
      5         C8      C9      filter env amount  -5 .. +5 octaves, 0.1 oct per click
                                (envelope sweeps the cutoff up, or down when negative)
    Spinning fast moves in bigger steps.

  ---------------------------------------------------------------- WIRING
    Mux 1 S0..S3   D0, D1, D2, D3
    Mux 1 SIG      D15
    Mux 1 EN       GND
    Mux 1 VCC/GND  3V3 / DGND
    Encoder A/B    -> mux channels in the table above
    Encoder middle -> GND (all five)
    10k pull-ups from C0..C9 to 3V3 recommended

    MIDI in (6N138)
      TRS tip  -> 220R -> 6N138 pin 2      (Type A; swap tip/ring for Type B)
      TRS ring -> 6N138 pin 3
      1N4148 across pins 2-3, cathode on pin 2
      pin 8 -> 3V3, pin 5 -> GND, 100nF between pins 8 and 5
      pin 6 -> 1k pull-up to 3V3, and pin 6 -> Seed D14 / physical pin 15 (MIDI RX)
      pin 7 -> 10k -> GND (speeds up turn-off at 3.3V)

  ---------------------------------------------------------------- DEBUG
  Set DEBUG_SERIAL 1 to print values over USB serial
  (Tools > USB support: CDC, 115200 baud).
*/

#include "DaisyDuino.h"
#include <MIDI.h>   // FortySevenEffects MIDI Library (same as WEASEL)

// ------------------------------------------------------------------ config
#define DEBUG_SERIAL        0

#define MIDI_CHANNEL        0          // 0 = omni (any channel), or 1-16
#define BEND_RANGE_ST       2.0f

#define OSC_LEVEL           0.4f
#define RESONANCE           0.3f
#define CUTOFF_MIN_HZ       20.0f
#define CUTOFF_MAX_HZ       18000.0f
#define SMOOTH_MS           10.0f

// start values
#define TUNE_START_ST       0.0f
#define CUTOFF_START_HZ     1000.0f
#define ATTACK_START_S      0.005f
#define RELEASE_START_S     0.3f
#define FLT_ENV_START_OCT   0.0f       // no envelope on the filter until you turn it

// amount per encoder click
#define TUNE_PER_CLICK      0.1f       // semitones
#define CUTOFF_PER_CLICK    0.1f       // octaves
#define TIME_PER_CLICK      0.01f      // of the 1 ms .. 10 s curve (100 clicks end to end)
#define FLT_ENV_PER_CLICK   0.1f       // octaves
#define FLT_ENV_RANGE_OCT   5.0f       // ± this many octaves

#define ENC_STEPS_PER_DETENT 4         // try 2 if every other click does nothing
#define ENC_REVERSE          0         // 1 = flip direction of all encoders
#define ENC_SETTLE_US        3

const int MUX_S[4] = { D0, D1, D2, D3 };
const int MUX_SIG  = D15;
// MIDI in on the board's built-in Serial1 (USART1), RX remapped to Arduino pin 14
// = Seed D14 (physical pin 15). This is the setup proven on the WEASEL board.
#define MIDI_RX_PIN 14
MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);

// Defined before any function: the Arduino IDE puts auto-generated prototypes
// above the first function, so types used in signatures must come first.
struct MuxEncoder {
  uint8_t  chA, chB;
  uint8_t  prev;
  int8_t   acc;
  uint32_t lastClick;
};

// ------------------------------------------------------------------ audio objects
DaisyHardware hw;
Oscillator    osc;
MoogLadder    filt;
Adsr          env;

float sampleRate = 48000.0f;
float smoothK    = 0.002f;

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

// 0..1 <-> 1 ms .. 10 s
static inline float timeFromNorm(float n) { return 0.001f * powf(10000.0f, n); }
static inline float normFromTime(float t) { return log10f(t / 0.001f) / 4.0f; }
static inline float noteToHz(float note)  { return 440.0f * exp2f((note - 69.0f) / 12.0f); }

const float CUTOFF_MAX_OCT = log2f(CUTOFF_MAX_HZ / CUTOFF_MIN_HZ);

// control targets, written by loop(), read by the audio callback
volatile float tuneTarget    = TUNE_START_ST;
volatile float cutoffTarget  = log2f(CUTOFF_START_HZ / CUTOFF_MIN_HZ);   // octaves above 20 Hz
volatile float attackNorm    = normFromTime(ATTACK_START_S);
volatile float releaseNorm   = normFromTime(RELEASE_START_S);
volatile float fltEnvTarget  = FLT_ENV_START_OCT;                      // octaves at full envelope

// smoothed values (audio thread only)
float tuneCur, cutoffCur, fltEnvCur, attackApplied = -1.0f, releaseApplied = -1.0f;

// note state, written by loop() (MIDI), read by the audio callback
volatile int      curNote   = 60;
volatile bool     gate      = false;
volatile uint32_t trigCount = 0;      // bumps on every new note so the envelope retriggers
volatile float    bendSt    = 0.0f;
uint32_t trigSeen = 0;

// ------------------------------------------------------------------ audio
void AudioCallback(float **in, float **out, size_t size) {
  // envelope times: only touch the Adsr when an encoder changed them
  float a = attackNorm, r = releaseNorm;
  if (a != attackApplied)  { env.SetTime(ADSR_SEG_ATTACK,  timeFromNorm(a)); attackApplied  = a; }
  if (r != releaseApplied) { env.SetTime(ADSR_SEG_RELEASE, timeFromNorm(r)); releaseApplied = r; }

  // new note while one is already sounding: restart the attack from the current level
  bool g = gate;
  if (trigCount != trigSeen) {
    trigSeen = trigCount;
    if (g) env.Retrigger(false);
  }

  float tuneT = tuneTarget, cutT = cutoffTarget, envAmtT = fltEnvTarget;
  float baseNote = curNote + bendSt;

  for (size_t i = 0; i < size; i++) {
    tuneCur   += smoothK * (tuneT - tuneCur);
    cutoffCur += smoothK * (cutT  - cutoffCur);
    fltEnvCur += smoothK * (envAmtT - fltEnvCur);

    float e = env.Process(g);

    // cutoff in octaves: base + envelope * amount, kept inside 20 Hz .. 18 kHz
    float fcOct = clampf(cutoffCur + fltEnvCur * e, 0.0f, CUTOFF_MAX_OCT);

    osc.SetFreq(noteToHz(baseNote + tuneCur));
    filt.SetFreq(CUTOFF_MIN_HZ * exp2f(fcOct));

    float s = filt.Process(osc.Process()) * e;
    out[0][i] = s;
    out[1][i] = s;
  }
}

// ------------------------------------------------------------------ encoders
MuxEncoder enc[5] = {
  { 0, 1, 0, 0, 0 },   // tune
  { 2, 3, 0, 0, 0 },   // cutoff
  { 4, 5, 0, 0, 0 },   // attack
  { 6, 7, 0, 0, 0 },   // release
  { 8, 9, 0, 0, 0 },   // filter env amount
};
enum { ENC_TUNE = 0, ENC_CUTOFF, ENC_ATTACK, ENC_RELEASE, ENC_FLT_ENV, NUM_ENC };

const int8_t QDEC[16] = { 0, -1, 1, 0,  1, 0, 0, -1,  -1, 0, 0, 1,  0, 1, -1, 0 };

static inline int readMuxPin(int ch) {
  for (int b = 0; b < 4; b++) digitalWrite(MUX_S[b], (ch >> b) & 1);
  delayMicroseconds(ENC_SETTLE_US);
  return digitalRead(MUX_SIG);
}

static inline uint8_t readEncoderPins(const MuxEncoder &e) {
  uint8_t a = readMuxPin(e.chA);
  uint8_t b = readMuxPin(e.chB);
  return ENC_REVERSE ? (b << 1 | a) : (a << 1 | b);
}

// returns 0, or a signed step size (bigger when spun fast)
int pollEncoder(MuxEncoder &e) {
  uint8_t s = readEncoderPins(e);
  if (s == e.prev) return 0;
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
  int d;

  if ((d = pollEncoder(enc[ENC_TUNE]))) {
    tuneTarget = clampf(tuneTarget + d * TUNE_PER_CLICK, -12.0f, 12.0f);
#if DEBUG_SERIAL
    Serial.print("tune st: "); Serial.println(tuneTarget, 1);
#endif
  }
  if ((d = pollEncoder(enc[ENC_CUTOFF]))) {
    cutoffTarget = clampf(cutoffTarget + d * CUTOFF_PER_CLICK, 0.0f, CUTOFF_MAX_OCT);
#if DEBUG_SERIAL
    Serial.print("cutoff Hz: "); Serial.println(CUTOFF_MIN_HZ * exp2f(cutoffTarget), 0);
#endif
  }
  if ((d = pollEncoder(enc[ENC_ATTACK]))) {
    attackNorm = clampf(attackNorm + d * TIME_PER_CLICK, 0.0f, 1.0f);
#if DEBUG_SERIAL
    Serial.print("attack s: "); Serial.println(timeFromNorm(attackNorm), 3);
#endif
  }
  if ((d = pollEncoder(enc[ENC_RELEASE]))) {
    releaseNorm = clampf(releaseNorm + d * TIME_PER_CLICK, 0.0f, 1.0f);
#if DEBUG_SERIAL
    Serial.print("release s: "); Serial.println(timeFromNorm(releaseNorm), 3);
#endif
  }
  if ((d = pollEncoder(enc[ENC_FLT_ENV]))) {
    fltEnvTarget = clampf(fltEnvTarget + d * FLT_ENV_PER_CLICK, -FLT_ENV_RANGE_OCT, FLT_ENV_RANGE_OCT);
#if DEBUG_SERIAL
    Serial.print("filter env oct: "); Serial.println(fltEnvTarget, 1);
#endif
  }
}

// ------------------------------------------------------------------ MIDI in (mono, last-note priority)
#define STACK_SIZE 16
uint8_t noteStack[STACK_SIZE];
int     stackLen = 0;

void noteOn(uint8_t n) {
  // remove if already held, then push on top
  for (int i = 0; i < stackLen; i++)
    if (noteStack[i] == n) { for (int j = i; j < stackLen - 1; j++) noteStack[j] = noteStack[j + 1]; stackLen--; break; }
  if (stackLen == STACK_SIZE) { for (int j = 0; j < stackLen - 1; j++) noteStack[j] = noteStack[j + 1]; stackLen--; }
  noteStack[stackLen++] = n;

  curNote = n;
  trigCount++;
  gate = true;
  digitalWrite(LED_BUILTIN, HIGH);
}

void noteOff(uint8_t n) {
  for (int i = 0; i < stackLen; i++)
    if (noteStack[i] == n) { for (int j = i; j < stackLen - 1; j++) noteStack[j] = noteStack[j + 1]; stackLen--; break; }
  if (stackLen > 0) {
    curNote = noteStack[stackLen - 1];   // fall back to the previous held note (legato)
  } else {
    gate = false;
    digitalWrite(LED_BUILTIN, LOW);
  }
}

void allNotesOff() {
  stackLen = 0;
  gate = false;
  digitalWrite(LED_BUILTIN, LOW);
}

// MIDI library callbacks (called from MIDI.read() in loop)
void handleNoteOn(byte channel, byte note, byte velocity) {
  noteOn(note);     // velocity 0 is already turned into note-off by the library
#if DEBUG_SERIAL
  Serial.print("note on  "); Serial.print(note); Serial.print(" ch "); Serial.println(channel);
#endif
}

void handleNoteOff(byte channel, byte note, byte velocity) {
  noteOff(note);
#if DEBUG_SERIAL
  Serial.print("note off "); Serial.println(note);
#endif
}

void handlePitchBend(byte channel, int bend) {
  bendSt = bend / 8192.0f * BEND_RANGE_ST;   // bend is -8192 .. +8191
}

void handleControlChange(byte channel, byte cc, byte value) {
  if (cc == 120 || cc == 123) allNotesOff();
}

// ------------------------------------------------------------------ setup / loop
void setup() {
#if DEBUG_SERIAL
  Serial.begin(115200);
#endif

  hw = DAISY.init(DAISY_SEED, AUDIO_SR_48K);
  sampleRate = DAISY.get_samplerate();
  smoothK = 1.0f - expf(-1.0f / (SMOOTH_MS * 0.001f * sampleRate));

  for (int b = 0; b < 4; b++) { pinMode(MUX_S[b], OUTPUT); digitalWrite(MUX_S[b], LOW); }
  pinMode(MUX_SIG, INPUT_PULLUP);
  for (int i = 0; i < NUM_ENC; i++) enc[i].prev = readEncoderPins(enc[i]);

  osc.Init(sampleRate);
  osc.SetWaveform(Oscillator::WAVE_POLYBLEP_SAW);
  osc.SetAmp(OSC_LEVEL);

  filt.Init(sampleRate);
  filt.SetRes(RESONANCE);

  // AR envelope = ADSR with full sustain and no decay
  env.Init(sampleRate);
  env.SetTime(ADSR_SEG_DECAY, 0.001f);
  env.SetSustainLevel(1.0f);

  tuneCur   = tuneTarget;
  cutoffCur = cutoffTarget;
  fltEnvCur = fltEnvTarget;

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

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
  while (MIDI.read()) { }   // callbacks above do the work
  scanEncoders();
}