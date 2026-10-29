/*
  4-TME PolySynth prototype v15.6 — 4 voices, 16 encoders, OLED menu, shift / voice groups, MIDI in
  Daisy Seed / DaisyDuino (Arduino IDE)

  Per voice: VariableShape osc (tri>saw>sq>PWM) -> MoogLadder LPF -> ADSR VCA -> pan/level
             triangle LFO -> pitch and cutoff,  ADSR -> amp, pitch and cutoff
  MIDI notes cycle round-robin over the voices: 1 -> 2 -> 3 -> 4 -> 1 ...

  ---------------------------------------------------------------- PANEL
  Voice buttons      press = choose the voice the encoders edit (its LED is lit)
  SHIFT (hold)       encoders edit every voice in the shift group (default: all 4),
                     each voice moving by the same amount from where it is
  SHIFT + voice btns choose the shift group while SHIFT is held:
                     the first press starts a new group with just that voice,
                     further presses add / remove voices. The group is kept for
                     next time. While SHIFT is held the LEDs show the group.
  Menu encoder       press = open the menu, turn = scroll, press = select / confirm
  BACK button        cancel / go up one level
  BACK (hold) + voice button, on the main screen: mute / unmute that voice.
                     A muted voice fades out in ~10 ms (no click) and stays in the
                     round-robin, so the note pattern doesn't shift; it's just silent.
                     While BACK is held the label reads MUTE and the LEDs show the
                     voices that are playing. Otherwise a muted voice's LED doesn't
                     flash and its planet is drawn hollow. Mutes are remembered at
                     power-up (last state), not stored in presets.
  SHIFT only acts as the "edit several voices" modifier on the main screen.
  In the menus it is a plain modifier (used for naming presets).
  Voice LEDs (PWM)   follow each voice's amp envelope, DCLK style:
                       lit voice (selected, or in the SHIFT group while SHIFT is
                       held on the main screen): steady at the LEDS level, rises
                       by that level's flash amount with the envelope, falls back
                       unlit voice: dark, rises to steady + flash with the
                       envelope, falls back to dark
                     (an LED pin without PWM blinks briefly on each trigger instead)

  MAIN SCREEN
    At rest: the selected voice (V2, or S 13 while SHIFT is held) and generative
    pixel art: each voice is a "planet" drifting over a starfield.
      shape     wave scrub: triangle > circle (saw) > square > narrowing bar (PWM)
      size      level, pulsing with the voice's amp envelope on every note
      fill      cutoff (dithered: darker = filter more closed)
      spikes    resonance; they stretch with the cutoff envelope amount on notes
      spin      LFO rate          wobble   pitch LFO amount
      position  pan (sideways), tune (up / down)
      stars     drift faster with the voices' average LFO rate
      marker    under the selected voice / the SHIFT group
    Turning an encoder shows that parameter until it's been still for
    PARAM_HOLD_MS, then the art comes back.

  BOOT ANIMATION (DCLK's sequence, about 2 s):
    1. ".axs instruments" fades in (display contrast ramp)
    2. count-in: four bars drop in, one per voice, each voice LED flashing with it
    3. "4-TME" wipes in above the bars, all four LEDs flash, then the main screen

  MENU (styled on DCLK: Picopixel font, ">" cursor, two rows per page)
    PRESETS   "INIT" at the top, then 32 slots, Model:Cycles style. Empty slots
              show "1. - - -".
              INIT -> CONFIRM resets every voice to the INIT sound (PARAM_DEFAULT):
                filter fully open, resonance 0, no LFO / envelope modulation,
                LFO 2 Hz, attack 1 ms (shortest), decay 48 ms, sustain 50 %,
                release 48 ms (closest steps to 50 ms), saw, level 80 %, pan centre.
              It's also the sound a unit with blank flash powers up with.
              Press a slot:  empty -> SAVE
                             used  -> LOAD / SAVE (overwrite) / CLEAR
              SAVE asks for a name (up to 5 characters) first:
                turn          = move between characters 1-5
                SHIFT + turn  = change the character: A-Z, 0-9, dash, space
                press         = save,  BACK = cancel
    SETTINGS
      MIDI      OMNI or channel 1-16 (applied on confirm)
      LEDS      LO / MID / HI, DCLK's levels (steady + trigger-flash PWM out of 255).
                Previewed live while editing, restored if you press BACK.
                LED_PWM 1 (default): hardware PWM on the LED pins (analogWrite)
                LED_PWM 0: plain on/off.   LED_PWM 2: ledDriverWrite() hook (PCA9685)
      VALUES    ON: turning an encoder shows that parameter's name and value
                OFF: the main screen keeps showing the art while you edit
      DISPLAY   screen timeout OFF / 1 / 5 / 10 / 30 min. After the timeout the
                panel switches off; any control wakes it (the menu encoder's
                first touch only wakes, voice controls wake AND act). Incoming
                MIDI notes wake it too and keep it awake while you play (pitch
                bend / clock don't, so a bend stream can't keep it on forever).

  STORAGE: external W25Q64 SPI flash (8 MB), same chip and command set as the
  MC8P MkII. Settings (MIDI IN, LEDS, DISPLAY) are saved half a second after
  you confirm a menu change and loaded at power-up. Erase / program runs as a
  background job polled from loop(), so MIDI, encoders and the display never
  wait on the flash (audio is unaffected either way: it runs in its interrupt).
  Flash layout (4 KB sectors):
    sector 0          global settings
    sector 1 + n      preset n (n = 0 .. NUM_PRESETS-1): name + all 16 params x 4 voices
    sectors 128-143   last state: the sound as it was when you switched off
  LAST STATE: STATE_SAVE_DELAY_MS after the last encoder change or preset load,
  every parameter of every voice (+ the selected voice) is saved, and it's
  restored at power-up. Saves rotate through 16 sectors (newest = highest
  sequence number), so the flash wears 16x slower and a power cut mid-save
  just falls back to the previous state. Changes made in the last couple of
  seconds before switching off may not be saved yet.
  The preset list (used / name) is read once at boot so the menu scrolls instantly.
  No chip found at boot -> the splash shows "NO FLASH" and nothing is saved.

  ---------------------------------------------------------------- ENCODERS
  Every mux encoder uses two neighbouring channels: A on the even, B on the odd.

   MUX 1 (SIG -> D15)                       MUX 2 (SIG -> D16)
   C0/C1   tune          ±12 st              C0/C1   cutoff CV amount  (not yet)
   C2/C3   wave scrub    tri>saw>sq>PWM      C2/C3   attack     1 ms .. 10 s
   C4/C5   pitch env amt ±24 st              C4/C5   decay      1 ms .. 10 s
   C6/C7   pitch LFO amt 0 .. 12 st          C6/C7   sustain    0 .. 100 %
   C8/C9   cutoff        20 Hz .. 18 kHz     C8/C9   release    1 ms .. 10 s
   C10/C11 resonance     0 .. 0.9            C10/C11 LFO rate   0.05 .. 25 Hz
   C12/C13 cutoff env    ±5 oct              C12/C13 pan        L .. R
   C14/C15 cutoff LFO    0 .. 4 oct          C14/C15 level      0 .. 100 %

  ---------------------------------------------------------------- WIRING
    Mux S0..S3     D0, D1, D2, D3      (shared by both muxes)
    Mux 1 SIG      D15 (physical pin 22)
    Mux 2 SIG      D16 (physical pin 23)
    Buttons 1-4    D4, D5, D6, D7      to GND, internal pull-ups
    LEDs 1-4       D17, D18, D19, D28  pin -> 1k -> LED -> GND
                   (LED 4 moved from D20 to D28 / physical pin 35: D20 = PC1 has no
                   timer, so it can't do PWM. Left on D20 it still works, on/off only.)
    OLED SSD1306   128x32, SCL D11 (pin 12), SDA D12 (pin 13), VCC 3V3, GND; address 0x3C
    Menu encoder   A D8 (pin 9), B D9 (pin 10), common -> GND
    Menu button    D10 (pin 11) -> GND (the encoder's push switch)
    SHIFT button   D21 (pin 28) -> GND
    BACK button    D22 (pin 29) -> GND
    W25Q64 flash   FLASH_SPI_SOFT 1 (breadboard, free pins, software SPI):
                     CS D23 (pin 30), CLK D24 (pin 31), DO/MISO D25 (pin 32),
                     DI/MOSI D26 (pin 33)
                   FLASH_SPI_SOFT 0 (PCB, hardware SPI1, the Seed's SPI pins):
                     CS D7, CLK D8, DO/MISO D9, DI/MOSI D10  (these clash with
                     voice button 4 and the menu encoder on the prototype)
                   VCC 3V3, GND; WP and HOLD to 3V3 (most modules do this already)
    MIDI in        6N138 output -> D30 (physical pin 37, USART1 RX alternate pin).
                   D14 is no longer used: its pin (PB7) doubles as I2C1 SDA.

  Libraries: FortySevenEffects MIDI, Adafruit GFX, Adafruit SSD1306 (+ BusIO)

  ---------------------------------------------------------------- DEBUG
  DEBUG_SERIAL 1 prints every edit + a status line every 2 s
  (Tools > USB support: CDC, 115200 baud). Leave at 0 for normal use.
  Tools > Optimize: "Fastest (-O3)".
*/

#include "DaisyDuino.h"
#include <MIDI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/Picopixel.h>
#include <SPI.h>

// ------------------------------------------------------------------ config
#define DEBUG_SERIAL        0
#define FILTER_TYPE         0          // 0 = MoogLadder (24 dB), 1 = Svf (12 dB, lighter)

#define NUM_VOICES          4
#define BEND_RANGE_ST       2.0f
#define BEND_ENABLE         1          // 0 = ignore MIDI pitch bend

#define OSC_LEVEL           0.4f
#define MIX_GAIN            0.5f
#define CUTOFF_MIN_HZ       20.0f
#define CUTOFF_MAX_HZ       18000.0f
#define SMOOTH_MS           10.0f

#define AUDIO_BLOCK_SIZE    16         // short callbacks keep the MIDI UART serviced
#define CONTROL_RATE        8          // pitch / cutoff / LFO update every N samples

#define ENC_STEPS_PER_DETENT 4         // try 2 if every other click does nothing
#define ENC_REVERSE          0         // 1 = flip direction of ALL mux encoders
#define ENC_SETTLE_US        10
#define ENC_FILTER           1         // a pin state must read the same twice before it counts
#define ENC_BLANK_US         2000      // skip encoder reads this long after an LED switches

#define MENU_ENC_STEPS       4         // menu encoder steps per detent (try 2 if needed)
#define MENU_ENC_REVERSE     0         // 0 = clockwise moves down lists / raises values (set 1 if your wiring is the other way)

#define DEBOUNCE_MS          20
#define LED_FLASH_MS         60        // trigger blink length for LEDs without PWM
#define LED_UPDATE_MS        4         // PWM LEDs follow the envelopes at 250 Hz
#define LED_PWM              1         // 0 = on/off, 1 = PWM on the LED pins, 2 = ledDriverWrite() (PCA9685, later)
#define LED_PWM_HZ           1000      // PWM frequency (same as DCLK)

#define OLED_ADDR            0x3C
#define OLED_ROTATION        0         // 0 = normal, 2 = rotated 180 degrees
#define OLED_W               128
#define OLED_H               32        // 128x32 panel (set 64 for a 128x64 one)
#define OLED_I2C_HZ          400000    // most SSD1306 modules also run at 800000-1000000 (faster frames)
#define PARAM_HOLD_MS        1500      // parameter readout stays this long after the last change
#define ART_FRAME_MS         50        // 20 fps for the generative art

// ---------- pins
const int MUX_S[4]  = { D0, D1, D2, D3 };
const int MUX1_SIG  = D15;
const int MUX2_SIG  = D16;
const int BTN_PINS[NUM_VOICES] = { D4,  D5,  D6,  D7  };
const int LED_PINS[NUM_VOICES] = { D17, D18, D19, D28 };   // all four PWM-capable (D20 isn't)
const int PIN_MENU_A   = D8;
const int PIN_MENU_B   = D9;
const int PIN_MENU_BTN = D10;
const int PIN_SHIFT    = D21;
const int PIN_BACK     = D22;

// ---------- W25Q64 SPI flash
#define FLASH_SPI_SOFT   1             // 1 = software SPI on free pins (prototype), 0 = hardware SPI1
#if FLASH_SPI_SOFT
const int FLASH_CS   = D23;
const int FLASH_SCK  = D24;
const int FLASH_MISO = D25;
const int FLASH_MOSI = D26;
#else
const int FLASH_CS   = D7;
const int FLASH_SCK  = D8;
const int FLASH_MISO = D9;
const int FLASH_MOSI = D10;
#define FLASH_SPI_HZ 20000000          // same as the MC8P
#endif
#define NUM_PRESETS        32
#define SETTINGS_SAVE_DELAY_MS 500     // save this long after the last confirmed change
#define STATE_SAVE_DELAY_MS    2000    // save the sound this long after the last edit
#define STATE_BASE_SECTOR      128     // 0x080000: well clear of settings and presets
#define STATE_SECTORS          16      // ring size (wear levelling)
const int PIN_OLED_SCL = D11;
const int PIN_OLED_SDA = D12;
#define MIDI_RX_PIN D30          // PB15, USART1 RX (alternate)

MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);

// ------------------------------------------------------------------ parameters
enum Param {
  P_TUNE = 0, P_WAVE, P_PITCH_ENV, P_PITCH_LFO,
  P_CUTOFF, P_RES, P_FLT_ENV, P_FLT_LFO,
  P_FLT_CV, P_ATTACK, P_DECAY, P_SUSTAIN,
  P_RELEASE, P_LFO_RATE, P_PAN, P_LEVEL,
  P_COUNT
};
#define NUM_ENC P_COUNT

// whole-number positions 0..PARAM_MAX; one click = 1 (0 = not implemented yet)
const int16_t PARAM_MAX[P_COUNT] = {
  240, 150,  96, 100,
   98, 100, 100, 100,
    0, 100, 100, 100,
  100, 100, 100, 100
};
// INIT sound (PRESETS > INIT, and power-up with blank flash)
const int16_t PARAM_DEFAULT[P_COUNT] = {
  120,  50,  48,   0,     // tune 0, saw, pitch env 0, no pitch LFO
   98,   0,  50,   0,     // cutoff fully open (17.8 kHz), res 0, cutoff env 0, no cutoff LFO
    0,   0,  42,  50,     // -, attack 1 ms (shortest), decay 48 ms, sustain 50 %
   42,  59,  50,  80      // release 48 ms, LFO 1.98 Hz, pan centre, level 80 %
};
// Per-encoder direction flip (1 = reversed) so clockwise always increases.
const bool ENC_INVERT[P_COUNT] = {
  0, 0, 0, 0,     // tune, wave, pitch env, pitch LFO
  0, 0, 0, 0,     // cutoff, res, cutoff env, cutoff LFO
  0, 1, 1, 1,     // cutoff CV, attack, decay, sustain
  1, 1, 1, 1      // release, LFO rate, pan, level
};
const char *PARAM_LABEL[P_COUNT] = {
  "TUNE", "WAVE", "PITCH ENV", "PITCH LFO", "CUTOFF", "RESO", "CUTOFF ENV", "CUTOFF LFO",
  "CUTOFF CV", "ATTACK", "DECAY", "SUSTAIN", "RELEASE", "LFO RATE", "PAN", "LEVEL"
};

// ---------- menu / settings
enum UiScreen : uint8_t {
  UI_MAIN,
  UI_TOP,                                    // PRESETS / SETTINGS
  UI_PRESETS, UI_PRESET_ACTION, UI_NAME,     // slot list, LOAD/SAVE/CLEAR, name entry
  UI_INIT,                                   // INIT confirm
  UI_SETTINGS, UI_EDIT_MIDI, UI_EDIT_LEDS, UI_EDIT_VALUES, UI_EDIT_DISPLAY
};
enum TopItem : uint8_t { TI_PRESETS, TI_SETTINGS, TI_COUNT };
const char *TOP_LABEL[TI_COUNT] = { "PRESETS", "SETTINGS" };
enum MenuItem : uint8_t { MI_MIDI, MI_LEDS, MI_VALUES, MI_DISPLAY, MI_COUNT };
const char *MENU_LABEL[MI_COUNT] = { "MIDI", "LEDS", "VALUES", "DISPLAY" };
enum PresetAction : uint8_t { PA_LOAD, PA_SAVE, PA_CLEAR };
const char *ACTION_LABEL[] = { "LOAD", "SAVE", "CLEAR" };

enum PresetOp : uint8_t { OP_NONE, OP_SAVE, OP_LOAD, OP_CLEAR };   // queued flash jobs

#define NAME_LEN 5
const char NAME_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789- ";   // A-Z, 0-9, dash, space
#define N_NAME_CHARS (sizeof(NAME_CHARS) - 1)

// LED brightness levels, same values as DCLK (PWM out of 255):
//   base  = steady brightness of a lit LED
//   flash = extra brightness added while a trigger flash is showing
struct LedLevel { uint8_t base; uint8_t flash; const char *name; };
const LedLevel LED_LEVELS[] = {
  {  5,  25, "LO"  },   //  2 % steady, 10 % flash
  { 25,  80, "MID" },   // 10 % steady, 31 % flash
  { 80, 150, "HI"  },   // 31 % steady, 59 % flash
};
#define N_LED_LEVELS 3
const uint8_t SAVER_MIN[] = { 0, 1, 5, 10, 30 };            // DCLK timeouts (minutes, 0 = off)
#define N_SAVER 5

// Types used in function signatures go before any function: the Arduino IDE
// puts its auto-generated prototypes above the first function in the file.
struct Debounce { bool stable, last; uint32_t changed; };   // button debounce state

struct MuxEncoder {
  uint8_t  prev;
  uint8_t  pending;
  int8_t   acc;
  uint32_t lastClick;
};

// What goes into flash. Magic + version catch blank or old data; checksum
// catches a half-written sector (e.g. power cut mid-save).
#define FLASH_MAGIC   0x34544D45UL     // "4TME"
#define FLASH_VERSION 1                // settings layout
#define PRESET_VERSION 2               // preset layout (2 = with name)
struct StoredSettings {
  uint32_t magic;
  uint8_t  version;
  uint8_t  midiChannel, ledLevel, saverIdx;
  uint8_t  hideValues;                 // VALUES setting, stored inverted so older saves (0) read as ON
  uint8_t  reserved[7];                // room for more settings without a version bump
  uint32_t checksum;
};
#define STATE_VERSION 1
struct StoredState {                   // last sound, for power-up
  uint32_t magic;
  uint8_t  version;
  uint8_t  selVoice;
  uint8_t  muteMask;                   // bit per voice (older saves: 0 = none muted)
  uint8_t  reserved[1];
  uint32_t seq;                        // increases with every save; the highest valid one wins
  int16_t  param[4][16];
  uint32_t checksum;
};
struct StoredPreset {
  uint32_t magic;
  uint8_t  version;
  char     name[5];                    // NAME_LEN characters, space padded, no terminator
  uint8_t  reserved[2];
  int16_t  param[4][16];               // [voice][param]
  uint32_t checksum;
};

static_assert(NUM_VOICES == 4 && P_COUNT == 16, "StoredPreset layout assumes 4 voices x 16 params");
static_assert(sizeof(StoredPreset) <= 256, "a preset must fit the flash write buffer");
static_assert(sizeof(StoredState) <= 256, "the state must fit the flash write buffer");

struct VoiceSettings {
  volatile float tune, wave, pitchEnv, pitchLfo;
  volatile float cutoffOct, res, fltEnv, fltLfo;
  volatile float attack, decay, sustain, release;
  volatile float lfoRate;
  volatile float gainL, gainR;
};

// ------------------------------------------------------------------ helpers
static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline float timeFromNorm(float n) { return 0.001f * powf(10000.0f, n); }   // 1 ms .. 10 s
static inline float noteToHz(float note)  { return 440.0f * exp2f((note - 69.0f) / 12.0f); }

const float CUTOFF_MAX_OCT = log2f(CUTOFF_MAX_HZ / CUTOFF_MIN_HZ);

DaisyHardware hw;
float sampleRate = 48000.0f;
float smoothK    = 0.002f;
float smoothKc   = 0.016f;

volatile uint32_t cbMaxCycles = 0, cbSumCycles = 0, cbCount = 0;
volatile uint32_t noteOnCount = 0;
volatile uint32_t nanCount    = 0;

// settings (RAM only for now, see header)
uint8_t midiChannel = 0;     // 0 = OMNI, 1-16
uint8_t ledLevel    = 1;     // MID
uint8_t ledLevelOrig = 1;    // restored if the LEDS edit is cancelled
uint8_t saverIdx    = 2;     // 5 min
uint8_t showValues  = 1;     // VALUES: 1 = show the parameter readout while editing

// ------------------------------------------------------------------ voice
struct Voice {
  VariableShapeOscillator osc;
  Oscillator lfo;
#if FILTER_TYPE == 1
  Svf        filt;
#else
  MoogLadder filt;
#endif
  Adsr       env;
  VoiceSettings set;
  int16_t    param[P_COUNT];

  volatile int      note      = 60;
  volatile int      heldNote  = -1;
  volatile bool     gate      = false;
  volatile uint32_t trigCount = 0;

  uint32_t trigSeen = 0;
  float aApplied = -1, dApplied = -1, sApplied = -1, rApplied = -1, lfoApplied = -1;
  float tuneC, pEnvC, pLfoC, cutC, resC, fEnvC, fLfoC, waveC;
  float oscGain = 1.0f, oscDc = 0.0f;
  float gLC, gRC;
  float lfoVal = 0.0f;
  volatile float envOut = 0.0f;           // current amp envelope 0..1 (for the LEDs)
  volatile bool  muted  = false;          // BACK + voice button
  float muteC = 1.0f;                     // smoothed mute gain (fades in / out, no click)
  bool  g = false;
  float baseNote = 60.0f;

  void init(float sr) {
    osc.Init(sr);
    osc.SetSync(false);
    osc.SetFreq(220.0f);
    osc.SetSyncFreq(220.0f);    // with sync off, this is the frequency you hear
    osc.SetWaveshape(0.5f);
    osc.SetPW(0.5f);
    lfo.Init(sr / CONTROL_RATE);
    lfo.SetWaveform(Oscillator::WAVE_TRI);
    lfo.SetAmp(1.0f);
    filt.Init(sr);
#if FILTER_TYPE == 1
    filt.SetDrive(0.0f);
#endif
    env.Init(sr);
  }

  void settle() {
    tuneC = set.tune; pEnvC = set.pitchEnv; pLfoC = set.pitchLfo;
    waveC = set.wave; updateWave();
    cutC = set.cutoffOct; resC = set.res; fEnvC = set.fltEnv; fLfoC = set.fltLfo;
    gLC = set.gainL; gRC = set.gainR;
  }

  void beginBlock(float bend) {
    float a = set.attack, d = set.decay, s = set.sustain, r = set.release, lr = set.lfoRate;
    if (a != aApplied) { env.SetTime(ADSR_SEG_ATTACK,  a); aApplied = a; }
    if (d != dApplied) { env.SetTime(ADSR_SEG_DECAY,   d); dApplied = d; }
    if (s != sApplied) { env.SetSustainLevel(s);           sApplied = s; }
    if (r != rApplied) { env.SetTime(ADSR_SEG_RELEASE, r); rApplied = r; }
    if (lr != lfoApplied) { lfo.SetFreq(lr);               lfoApplied = lr; }

    g = gate;
    if (trigCount != trigSeen) {
      trigSeen = trigCount;
      if (g) env.Retrigger(false);
    }
    baseNote = note + bend;
  }

  // wave 0..3 -> shape + pulse width, with level matching and DC removal
  void updateWave() {
    float w = clampf(waveC, 0.0f, 3.0f);
    float rms;
    if (w <= 2.0f) {
      osc.SetWaveshape(w * 0.5f);
      osc.SetPW(0.5f);
      oscDc = 0.0f;
      rms = (w <= 1.0f) ? 0.58f - 0.68f * w * (1.0f - w) : 0.57f + 0.43f * (w - 1.0f);
    } else {
      float pw = 0.5f - (w - 2.0f) * 0.45f;
      osc.SetWaveshape(1.0f);
      osc.SetPW(pw);
      oscDc = 1.0f - 2.0f * pw;
      rms = 2.0f * sqrtf(pw * (1.0f - pw));
    }
    oscGain = clampf(0.58f / rms, 0.5f, 1.5f);
  }

  inline float oscOut() { return (osc.Process() - oscDc) * oscGain * OSC_LEVEL; }

  float process(bool ctl) {
    float e = env.Process(g);
    envOut = e;                           // read by the LEDs
    if (ctl) {
      tuneC += smoothKc * (set.tune      - tuneC);
      pEnvC += smoothKc * (set.pitchEnv  - pEnvC);
      pLfoC += smoothKc * (set.pitchLfo  - pLfoC);
      cutC  += smoothKc * (set.cutoffOct - cutC);
      resC  += smoothKc * (set.res       - resC);
      fEnvC += smoothKc * (set.fltEnv    - fEnvC);
      fLfoC += smoothKc * (set.fltLfo    - fLfoC);
      waveC += smoothKc * (set.wave      - waveC);

      lfoVal = lfo.Process();
      float semis = baseNote + tuneC + pEnvC * e + pLfoC * lfoVal;
      float hz = clampf(noteToHz(semis), 1.0f, sampleRate * 0.45f);
      osc.SetFreq(hz);
      osc.SetSyncFreq(hz);
      updateWave();

      float fcOct = clampf(cutC + fEnvC * e + fLfoC * lfoVal, 0.0f, CUTOFF_MAX_OCT);
      filt.SetFreq(CUTOFF_MIN_HZ * exp2f(fcOct));
      filt.SetRes(resC);
    }
    gLC += smoothK * (set.gainL - gLC);
    gRC += smoothK * (set.gainR - gRC);
    muteC += smoothK * ((muted ? 0.0f : 1.0f) - muteC);
#if FILTER_TYPE == 1
    filt.Process(oscOut());
    return filt.Low() * e * muteC;
#else
    return filt.Process(oscOut()) * e * muteC;
#endif
  }
};

Voice voices[NUM_VOICES];
volatile float bendSt = 0.0f;

void applyParam(int v, int id) {
  Voice &vc = voices[v];
  VoiceSettings &s = vc.set;
  int16_t p = vc.param[id];
  float n = PARAM_MAX[id] ? (float)p / PARAM_MAX[id] : 0.0f;
  switch (id) {
    case P_TUNE:      s.tune      = (p - 120) * 0.1f; break;
    case P_WAVE:      s.wave      = p / 50.0f; break;
    case P_PITCH_ENV: s.pitchEnv  = (p - 48) * 0.5f; break;
    case P_PITCH_LFO: s.pitchLfo  = n * n * 12.0f; break;
    case P_CUTOFF:    s.cutoffOct = p * 0.1f; break;
    case P_RES:       s.res       = n * 0.9f; break;
    case P_FLT_ENV:   s.fltEnv    = (p - 50) * 0.1f; break;
    case P_FLT_LFO:   s.fltLfo    = n * n * 4.0f; break;
    case P_ATTACK:    s.attack    = timeFromNorm(n); break;
    case P_DECAY:     s.decay     = timeFromNorm(n); break;
    case P_SUSTAIN:   s.sustain   = n; break;
    case P_RELEASE:   s.release   = timeFromNorm(n); break;
    case P_LFO_RATE:  s.lfoRate   = 0.05f * exp2f(n * 9.0f); break;
    case P_PAN:
    case P_LEVEL: {
      float pan   = (float)vc.param[P_PAN] / PARAM_MAX[P_PAN];
      float level = (float)vc.param[P_LEVEL] / PARAM_MAX[P_LEVEL];
      float a = pan * 0.5f * PI_F;
      s.gainL = cosf(a) * level;
      s.gainR = sinf(a) * level;
      break;
    }
    default: break;
  }
}

// ------------------------------------------------------------------ value text (no printf float)
// "+1.2", "0.25" etc. without relying on printf %f (not enabled in newlib-nano)
void fmtFixed(char *buf, size_t n, float v, int decimals, bool showPlus) {
  int32_t scale = 1;
  for (int i = 0; i < decimals; i++) scale *= 10;
  int32_t x = (int32_t)lroundf(v * scale);
  const char *sign = x < 0 ? "-" : (showPlus && x > 0 ? "+" : "");
  if (x < 0) x = -x;
  if (decimals == 0) snprintf(buf, n, "%s%ld", sign, (long)x);
  else snprintf(buf, n, "%s%ld.%0*ld", sign, (long)(x / scale), decimals, (long)(x % scale));
}

void fmtTime(char *buf, size_t n, float sec) {
  if (sec < 1.0f) { fmtFixed(buf, n, sec * 1000.0f, 0, false); strncat(buf, " MS", n - strlen(buf) - 1); }
  else            { fmtFixed(buf, n, sec, 2, false);           strncat(buf, " S",  n - strlen(buf) - 1); }
}

void formatParam(int v, int id, char *buf, size_t n) {
  const VoiceSettings &s = voices[v].set;
  int16_t p = voices[v].param[id];
  char t[16];
  switch (id) {
    case P_TUNE:      fmtFixed(t, sizeof(t), s.tune, 1, true);     snprintf(buf, n, "%s ST", t); break;
    case P_WAVE:
      if      (p == 0)   snprintf(buf, n, "TRI");
      else if (p < 50)   snprintf(buf, n, "TRI>SAW %d", p * 2);
      else if (p == 50)  snprintf(buf, n, "SAW");
      else if (p < 100)  snprintf(buf, n, "SAW>SQR %d", (p - 50) * 2);
      else if (p == 100) snprintf(buf, n, "SQUARE");
      else { fmtFixed(t, sizeof(t), 50.0f - (s.wave - 2.0f) * 45.0f, 0, false); snprintf(buf, n, "PW %s%%", t); }
      break;
    case P_PITCH_ENV: fmtFixed(t, sizeof(t), s.pitchEnv, 1, true); snprintf(buf, n, "%s ST", t); break;
    case P_PITCH_LFO: fmtFixed(t, sizeof(t), s.pitchLfo, 2, false); snprintf(buf, n, "%s ST", t); break;
    case P_CUTOFF: {
      float hz = CUTOFF_MIN_HZ * exp2f(s.cutoffOct);
      if (hz < 1000.0f) { fmtFixed(t, sizeof(t), hz, 0, false);          snprintf(buf, n, "%s HZ", t); }
      else              { fmtFixed(t, sizeof(t), hz / 1000.0f, 1, false); snprintf(buf, n, "%s KHZ", t); }
      break;
    }
    case P_RES:       snprintf(buf, n, "%d%%", p); break;
    case P_FLT_ENV:   fmtFixed(t, sizeof(t), s.fltEnv, 1, true);   snprintf(buf, n, "%s OCT", t); break;
    case P_FLT_LFO:   fmtFixed(t, sizeof(t), s.fltLfo, 2, false);  snprintf(buf, n, "%s OCT", t); break;
    case P_ATTACK:    fmtTime(buf, n, s.attack);  break;
    case P_DECAY:     fmtTime(buf, n, s.decay);   break;
    case P_SUSTAIN:   snprintf(buf, n, "%d%%", p); break;
    case P_RELEASE:   fmtTime(buf, n, s.release); break;
    case P_LFO_RATE:  fmtFixed(t, sizeof(t), s.lfoRate, s.lfoRate < 10.0f ? 2 : 1, false); snprintf(buf, n, "%s HZ", t); break;
    case P_PAN:
      if (p == 50)     snprintf(buf, n, "CENTRE");
      else if (p < 50) snprintf(buf, n, "L %d", (50 - p) * 2);
      else             snprintf(buf, n, "R %d", (p - 50) * 2);
      break;
    case P_LEVEL:     snprintf(buf, n, "%d%%", p); break;
    default:          snprintf(buf, n, "-"); break;
  }
}

// ------------------------------------------------------------------ audio
void AudioCallback(float **in, float **out, size_t size) {
  uint32_t c0 = DWT->CYCCNT;
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
    l *= MIX_GAIN; r *= MIX_GAIN;
    if (!(fabsf(l) < 100.0f) || !(fabsf(r) < 100.0f)) { nanCount++; l = 0.0f; r = 0.0f; }
    out[0][i] = l;
    out[1][i] = r;
  }
  uint32_t dc = DWT->CYCCNT - c0;
  if (dc > cbMaxCycles) cbMaxCycles = dc;
  cbSumCycles += dc; cbCount++;
}

// ------------------------------------------------------------------ W25Q64 SPI flash
// Command set and page / sector handling as in the MC8P MkII driver.
#define W25Q_WRITE_ENABLE  0x06
#define W25Q_READ_DATA     0x03
#define W25Q_PAGE_PROGRAM  0x02
#define W25Q_SECTOR_ERASE  0x20
#define W25Q_READ_STATUS1  0x05
#define W25Q_READ_ID       0x9F
#define W25Q_RELEASE_PD    0xAB

bool flashOk = false;

static inline void flashSelect()   {
#if !FLASH_SPI_SOFT
  SPI.beginTransaction(SPISettings(FLASH_SPI_HZ, MSBFIRST, SPI_MODE0));
#endif
  digitalWrite(FLASH_CS, LOW);
}
static inline void flashDeselect() {
  digitalWrite(FLASH_CS, HIGH);
#if !FLASH_SPI_SOFT
  SPI.endTransaction();
#endif
}

uint8_t spiXfer(uint8_t out) {
#if FLASH_SPI_SOFT
  uint8_t in = 0;                      // SPI mode 0, MSB first
  for (int b = 7; b >= 0; b--) {
    digitalWrite(FLASH_MOSI, (out >> b) & 1);
    digitalWrite(FLASH_SCK, HIGH);
    in = (in << 1) | (digitalRead(FLASH_MISO) & 1);
    digitalWrite(FLASH_SCK, LOW);
  }
  return in;
#else
  return SPI.transfer(out);
#endif
}

static inline void spiAddr(uint32_t a) { spiXfer(a >> 16); spiXfer(a >> 8); spiXfer(a); }

void flashCommand(uint8_t cmd) { flashSelect(); spiXfer(cmd); flashDeselect(); }

uint8_t flashStatus() {
  flashSelect(); spiXfer(W25Q_READ_STATUS1); uint8_t st = spiXfer(0); flashDeselect();
  return st;
}
bool flashBusy() { return flashStatus() & 0x01; }

void flashWaitBlocking() { while (flashBusy()) { } }   // boot only

uint32_t flashReadId() {
  flashSelect(); spiXfer(W25Q_READ_ID);
  uint32_t id = (uint32_t)spiXfer(0) << 16; id |= (uint32_t)spiXfer(0) << 8; id |= spiXfer(0);
  flashDeselect();
  return id;
}

void flashRead(uint32_t addr, uint8_t *dst, uint32_t len) {
  flashSelect(); spiXfer(W25Q_READ_DATA); spiAddr(addr);
  for (uint32_t i = 0; i < len; i++) dst[i] = spiXfer(0);
  flashDeselect();
}

uint32_t checksum32(const uint8_t *p, size_t len) {     // FNV-1a
  uint32_t h = 2166136261UL;
  for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 16777619UL; }
  return h;
}

void flashInit() {
  pinMode(FLASH_CS, OUTPUT);
  digitalWrite(FLASH_CS, HIGH);
#if FLASH_SPI_SOFT
  pinMode(FLASH_SCK, OUTPUT);  digitalWrite(FLASH_SCK, LOW);
  pinMode(FLASH_MOSI, OUTPUT); digitalWrite(FLASH_MOSI, LOW);
  pinMode(FLASH_MISO, INPUT_PULLUP);
#else
  SPI.setMOSI(FLASH_MOSI);
  SPI.setMISO(FLASH_MISO);
  SPI.setSCLK(FLASH_SCK);
  SPI.begin();
#endif
  flashCommand(W25Q_RELEASE_PD);       // in case the chip was left powered down
  delayMicroseconds(50);
  uint32_t id = flashReadId();
  // Winbond = 0xEF; anything else is fine too as long as it isn't a dead bus
  flashOk = (id != 0x000000 && id != 0xFFFFFF);
#if DEBUG_SERIAL
  Serial.print("SPI flash ID 0x"); Serial.print(id, HEX);
  Serial.println(flashOk ? " OK" : " NOT FOUND - check wiring");
#endif
}

// ---- background write job: erase one 4 KB sector, then program it page by page.
// flashService() is called every pass of loop() and only ever sends a few bytes
// or one 256-byte page (~0.3 ms hardware SPI, ~2 ms software SPI) before returning.
enum FlashJobState : uint8_t { FJ_IDLE, FJ_ERASING, FJ_PROGRAM, FJ_PROGRAMMING };
FlashJobState fjState = FJ_IDLE;
uint32_t fjAddr = 0;                   // sector start
uint16_t fjLen = 0, fjDone = 0;
uint8_t  fjBuf[256];                   // data being written (settings or one preset)

bool flashJobBusy() { return fjState != FJ_IDLE; }

bool flashStartWrite(uint32_t sectorAddr, const void *data, uint16_t len) {
  if (!flashOk || flashJobBusy() || len > sizeof(fjBuf)) return false;
  memcpy(fjBuf, data, len);
  fjAddr = sectorAddr; fjLen = len; fjDone = 0;
  flashCommand(W25Q_WRITE_ENABLE);
  flashSelect(); spiXfer(W25Q_SECTOR_ERASE); spiAddr(fjAddr); flashDeselect();
  fjState = FJ_ERASING;                // typ. 45 ms, max 400 ms: polled, not waited on
  return true;
}

void flashService() {
  switch (fjState) {
    case FJ_IDLE: return;
    case FJ_ERASING:
    case FJ_PROGRAMMING:
      if (flashBusy()) return;
      fjState = (fjDone >= fjLen) ? FJ_IDLE : FJ_PROGRAM;
      return;
    case FJ_PROGRAM: {                 // one page (or what's left of the data)
      uint16_t n = fjLen - fjDone;
      uint32_t a = fjAddr + fjDone;
      uint16_t room = 256 - (a & 0xFF);
      if (n > room) n = room;
      flashCommand(W25Q_WRITE_ENABLE);
      flashSelect(); spiXfer(W25Q_PAGE_PROGRAM); spiAddr(a);
      for (uint16_t i = 0; i < n; i++) spiXfer(fjBuf[fjDone + i]);
      flashDeselect();
      fjDone += n;
      fjState = FJ_PROGRAMMING;        // typ. 0.7 ms per page
      return;
    }
  }
}

// ---- settings
const uint32_t SETTINGS_ADDR = 0x000000;
bool     settingsSavePending = false;
uint32_t settingsChangedMs   = 0;

void requestSettingsSave() { settingsSavePending = true; settingsChangedMs = millis(); }

void loadSettings() {
  if (!flashOk) return;
  StoredSettings st;
  flashRead(SETTINGS_ADDR, (uint8_t *)&st, sizeof(st));
  if (st.magic != FLASH_MAGIC || st.version != FLASH_VERSION) return;      // blank / old: keep defaults
  if (st.checksum != checksum32((const uint8_t *)&st, offsetof(StoredSettings, checksum))) return;
  if (st.midiChannel <= 16)          midiChannel = st.midiChannel;
  if (st.ledLevel < N_LED_LEVELS)    ledLevel    = st.ledLevel;
  if (st.saverIdx < N_SAVER)         saverIdx    = st.saverIdx;
  showValues = st.hideValues ? 0 : 1;
}

void serviceSettingsSave() {
  if (!settingsSavePending || millis() - settingsChangedMs < SETTINGS_SAVE_DELAY_MS) return;
  if (flashJobBusy()) return;          // e.g. a preset is still being written: try next pass
  StoredSettings st;
  memset(&st, 0, sizeof(st));
  st.magic = FLASH_MAGIC; st.version = FLASH_VERSION;
  st.midiChannel = midiChannel; st.ledLevel = ledLevel; st.saverIdx = saverIdx;
  st.hideValues = showValues ? 0 : 1;
  st.checksum = checksum32((const uint8_t *)&st, offsetof(StoredSettings, checksum));
  if (flashStartWrite(SETTINGS_ADDR, &st, sizeof(st)) || !flashOk) settingsSavePending = false;
}

// ---- presets: name + every parameter of all 4 voices
uint32_t presetAddr(uint8_t slot) { return 0x1000UL * (1 + slot); }

// directory, read once at boot and kept up to date by save / clear
bool presetUsed[NUM_PRESETS];
char presetName[NUM_PRESETS][NAME_LEN + 1];

bool readPreset(uint8_t slot, StoredPreset &pr) {
  if (!flashOk || slot >= NUM_PRESETS) return false;
  flashRead(presetAddr(slot), (uint8_t *)&pr, sizeof(pr));
  if (pr.magic != FLASH_MAGIC || pr.version != PRESET_VERSION) return false;
  return pr.checksum == checksum32((const uint8_t *)&pr, offsetof(StoredPreset, checksum));
}

void scanPresets() {                   // boot only: ~9 KB read
  StoredPreset pr;
  for (int i = 0; i < NUM_PRESETS; i++) {
    presetUsed[i] = readPreset(i, pr);
    if (presetUsed[i]) { memcpy(presetName[i], pr.name, NAME_LEN); presetName[i][NAME_LEN] = 0; }
    else               presetName[i][0] = 0;
  }
}

bool startPresetSave(uint8_t slot, const char *name) {
  StoredPreset pr;
  memset(&pr, 0, sizeof(pr));
  pr.magic = FLASH_MAGIC; pr.version = PRESET_VERSION;
  memcpy(pr.name, name, NAME_LEN);
  for (int v = 0; v < NUM_VOICES; v++)
    for (int id = 0; id < P_COUNT; id++) pr.param[v][id] = voices[v].param[id];
  pr.checksum = checksum32((const uint8_t *)&pr, offsetof(StoredPreset, checksum));
  if (!flashStartWrite(presetAddr(slot), &pr, sizeof(pr))) return false;
  presetUsed[slot] = true;
  memcpy(presetName[slot], name, NAME_LEN); presetName[slot][NAME_LEN] = 0;
  return true;
}

bool startPresetClear(uint8_t slot) {  // erase only: a blank sector reads as an empty slot
  if (!flashStartWrite(presetAddr(slot), fjBuf, 0)) return false;
  presetUsed[slot] = false; presetName[slot][0] = 0;
  return true;
}

// Blocking read (~0.1 ms hardware SPI, ~1 ms software SPI). False if the slot is empty.
bool loadPreset(uint8_t slot) {
  StoredPreset pr;
  if (flashJobBusy() || !readPreset(slot, pr)) return false;
  for (int v = 0; v < NUM_VOICES; v++)
    for (int id = 0; id < P_COUNT; id++) {
      int16_t p = pr.param[v][id];
      if (p < 0) p = 0;
      if (p > PARAM_MAX[id]) p = PARAM_MAX[id];
      voices[v].param[id] = p;
      applyParam(v, id);               // smoothing in the voices glides to the new values
    }
  return true;
}

// The flash does one job at a time (e.g. a settings save may still be running),
// so menu actions are queued and started from loop() as soon as it's free.
PresetOp pendingOp   = OP_NONE;
uint8_t  pendingSlot = 0;
char     pendingName[NAME_LEN + 1];

void queuePresetOp(PresetOp op, uint8_t slot, const char *name) {
  pendingOp = op; pendingSlot = slot;
  if (name) { memcpy(pendingName, name, NAME_LEN); pendingName[NAME_LEN] = 0; }
}

void showToast(const char *l1, const char *l2);   // defined with the UI below
void markStateDirty();                             // defined with the last-state code below

void presetSlotText(uint8_t slot, char *buf, size_t n);

// PRESETS > INIT: every voice back to the INIT sound
void initSound() {
  for (int v = 0; v < NUM_VOICES; v++)
    for (int id = 0; id < P_COUNT; id++) {
      voices[v].param[id] = PARAM_DEFAULT[id];
      applyParam(v, id);                 // the voices' smoothing glides to it, no clicks
    }
  markStateDirty();                      // and it's what you'll get at the next power-up
}

void servicePresetOps() {
  if (pendingOp == OP_NONE || flashJobBusy()) return;
  char t[16];
  presetSlotText(pendingSlot, t, sizeof(t));
  switch (pendingOp) {
    case OP_SAVE:
      if (!startPresetSave(pendingSlot, pendingName)) return;
      presetSlotText(pendingSlot, t, sizeof(t));
      showToast("SAVED", t);
      break;
    case OP_LOAD:
      if (loadPreset(pendingSlot)) { showToast("LOADED", t); markStateDirty(); }
      else                           showToast("LOAD FAILED", t);
      break;
    case OP_CLEAR:
      if (!startPresetClear(pendingSlot)) return;
      showToast("CLEARED", t);
      break;
    default: break;
  }
  pendingOp = OP_NONE;
}

// ------------------------------------------------------------------ UI state
UiScreen uiScreen  = UI_MAIN;
uint8_t  topSel    = 0;          // PRESETS / SETTINGS
uint8_t  menuSel   = 0;          // SETTINGS list
uint8_t  listSel   = 0;          // PRESETS list row: 0 = INIT, 1.. = slot 1..
uint8_t  presetSel = 0;          // preset slot being acted on (0-based)
uint8_t  actionSel = 0;          // index into the slot's action list
char     nameBuf[NAME_LEN + 1];  // name being entered
uint8_t  nameCursor = 0;

char     toast1[16], toast2[24]; // short message over the screen (SAVED / LOADED ...)
uint32_t toastUntil = 0;
uint8_t  editIdx   = 0;          // value being edited (index into its option list)
bool     uiDirty   = true;

int      selVoice   = 0;         // voice edited without SHIFT
bool     shiftHeld  = false;
uint8_t  shiftGroup = 0x0F;      // voices edited with SHIFT (bit per voice)
bool     groupStartedThisHold = false;

// SHIFT edits several voices only on the main screen; in menus it's a plain modifier
#define GROUP_MODE() (shiftHeld && uiScreen == UI_MAIN)
bool backHeld = false;           // BACK button currently down
// BACK held on the main screen: voice buttons mute / unmute
#define MUTE_MODE()  (backHeld && uiScreen == UI_MAIN)

int      lastParam  = -1;        // last edited parameter (-1 = none yet)
uint32_t lastParamMs = 0;        // when it was last changed
int      lastParamVoice = 0;

uint32_t lastActivity = 0;
bool     screenAsleep = false;

void markActivity() { lastActivity = millis(); }

// Returns true if the screen was asleep (that input should then only wake it)
bool wakeScreen() {
  markActivity();
  if (screenAsleep) {
    screenAsleep = false;
    display.ssd1306_command(SSD1306_DISPLAYON);
    uiDirty = true;
    return true;
  }
  return false;
}

// ------------------------------------------------------------------ last state (power-up = as you left it)
bool     stateDirty   = false;
uint32_t stateEditMs  = 0;
uint32_t stateSeq     = 0;             // sequence number of the newest saved state
uint8_t  stateNext    = 0;             // ring slot the next save goes to

void markStateDirty() { stateDirty = true; stateEditMs = millis(); }

uint32_t stateAddr(uint8_t i) { return 0x1000UL * (STATE_BASE_SECTOR + i); }

bool readState(uint8_t i, StoredState &st) {
  flashRead(stateAddr(i), (uint8_t *)&st, sizeof(st));
  if (st.magic != FLASH_MAGIC || st.version != STATE_VERSION) return false;
  return st.checksum == checksum32((const uint8_t *)&st, offsetof(StoredState, checksum));
}

// boot: find the newest valid state in the ring and apply it to the voices
void loadState() {
  if (!flashOk) return;
  StoredState st, best;
  int bestIdx = -1;
  for (uint8_t i = 0; i < STATE_SECTORS; i++)
    if (readState(i, st) && (bestIdx < 0 || (int32_t)(st.seq - best.seq) > 0)) { best = st; bestIdx = i; }
  if (bestIdx < 0) return;                       // nothing saved yet: keep the defaults
  stateSeq  = best.seq;
  stateNext = (bestIdx + 1) % STATE_SECTORS;
  for (int v = 0; v < NUM_VOICES; v++) {
    for (int id = 0; id < P_COUNT; id++) {
      int16_t p = best.param[v][id];
      if (p < 0) p = 0;
      if (p > PARAM_MAX[id]) p = PARAM_MAX[id];
      voices[v].param[id] = p;
    }
    for (int id = 0; id < P_COUNT; id++) applyParam(v, id);
    voices[v].settle();                          // start there, no glide at power-up
  }
  if (best.selVoice < NUM_VOICES) selVoice = best.selVoice;
  for (int v = 0; v < NUM_VOICES; v++) {
    voices[v].muted = (best.muteMask >> v) & 1;
    voices[v].muteC = voices[v].muted ? 0.0f : 1.0f;
  }
}

void serviceStateSave() {
  if (!stateDirty || !flashOk || millis() - stateEditMs < STATE_SAVE_DELAY_MS) return;
  if (flashJobBusy() || pendingOp != OP_NONE || settingsSavePending) return;   // let those go first
  StoredState st;
  memset(&st, 0, sizeof(st));
  st.magic = FLASH_MAGIC; st.version = STATE_VERSION;
  st.selVoice = selVoice;
  for (int v = 0; v < NUM_VOICES; v++) if (voices[v].muted) st.muteMask |= 1 << v;
  st.seq = stateSeq + 1;
  for (int v = 0; v < NUM_VOICES; v++)
    for (int id = 0; id < P_COUNT; id++) st.param[v][id] = voices[v].param[id];
  st.checksum = checksum32((const uint8_t *)&st, offsetof(StoredState, checksum));
  if (!flashStartWrite(stateAddr(stateNext), &st, sizeof(st))) return;
  stateSeq = st.seq;
  stateNext = (stateNext + 1) % STATE_SECTORS;
  stateDirty = false;
}

// ------------------------------------------------------------------ LEDs
// Normal: the selected voice is lit. SHIFT held: the voices in the shift group
// are lit. A trigger flashes a voice's LED.
//
// Each LED gets a brightness 0..255 from the LEDS level:
//   lit = base, lit + flashing = base + flash, unlit + flashing = flash, else 0
// LED_PWM 0 (now): plain on/off pins. A flash on a lit LED shows as a short
//   blink OFF, since an on/off pin can't get brighter.
// LED_PWM 1 (with the LED driver): brightness goes to ledDriverWrite().
uint32_t ledFlashStart[NUM_VOICES];
bool     ledFlashing[NUM_VOICES];
int16_t  ledOut[NUM_VOICES] = { -1, -1, -1, -1 };   // last value written, -1 = force a write
volatile uint32_t lastLedChangeUs = 0;

void flashVoiceLed(int v) { ledFlashStart[v] = millis(); ledFlashing[v] = true; }

bool ledHasPwm[NUM_VOICES];          // set in setup(): does this pin have a timer channel?

// PWM brightness for one voice LED, following that voice's envelope (0..1):
//   lit   : base + flash x env        (rests at base, peaks at base + flash)
//   unlit : (base + flash) x env      (rests dark,   peaks at base + flash)
uint8_t ledBrightness(bool lit, float env) {
  const LedLevel &L = LED_LEVELS[ledLevel];
  env = clampf(env, 0.0f, 1.0f);
  float b = lit ? L.base + L.flash * env : (L.base + L.flash) * env;
  return (uint8_t)clampf(b + 0.5f, 0.0f, 255.0f);
}

// Hook for the PCA9685 (or any PWM driver): channel i, brightness 0..255.
void ledDriverWrite(int i, uint8_t brightness) {
  // TODO: pca9685.setPWM(channel_of_voice_led[i], 0, brightness * 16);   // 12-bit
  (void)i; (void)brightness;
}

// one LED output: brightness 0..255
void ledWrite(int i, uint8_t value) {
#if LED_PWM == 1
  if (ledHasPwm[i]) analogWrite(LED_PINS[i], value);
  else              digitalWrite(LED_PINS[i], value ? HIGH : LOW);
#elif LED_PWM == 2
  ledDriverWrite(i, value);
#else
  digitalWrite(LED_PINS[i], value ? HIGH : LOW);
#endif
}

void updateLeds() {
  uint32_t now = millis();
  static uint32_t lastPwmUpdate = 0;
  bool pwmTick = (now - lastPwmUpdate >= LED_UPDATE_MS);
  if (pwmTick) lastPwmUpdate = now;

  for (int i = 0; i < NUM_VOICES; i++) {
    if (ledFlashing[i] && now - ledFlashStart[i] >= LED_FLASH_MS) ledFlashing[i] = false;
    bool lit = MUTE_MODE()  ? !voices[i].muted                 // BACK held: playing voices lit
             : GROUP_MODE() ? ((shiftGroup >> i) & 1)
             :                (i == selVoice);
    bool dimmable = (LED_PWM == 2) || (LED_PWM == 1 && ledHasPwm[i]);

    if (dimmable) {
      // follow the envelope; throttled, and no encoder blanking: a duty change
      // doesn't add switching edges (the PWM is switching all the time anyway)
      if (!pwmTick && ledOut[i] >= 0) continue;
      float env = voices[i].muted ? 0.0f : voices[i].envOut;   // a muted voice doesn't flash
      int16_t value = ledBrightness(lit, env);
      if (ledOut[i] != value) { ledWrite(i, (uint8_t)value); ledOut[i] = value; }
    } else {
      // on/off pin: lit or not, a trigger blinks it to the opposite state
      int16_t value = (ledFlashing[i] ? !lit : lit) ? 255 : 0;
      if (ledOut[i] != value) {
        ledWrite(i, (uint8_t)value);
        ledOut[i] = value;
        lastLedChangeUs = micros();           // pause encoder reads briefly (see scanEncoders)
      }
    }
  }
}

// ------------------------------------------------------------------ buttons
Debounce voiceBtn[NUM_VOICES], shiftBtn, menuBtn, backBtn;

// returns +1 on press, -1 on release, 0 otherwise
int debounce(Debounce &d, int pin) {
  bool raw = digitalRead(pin);
  uint32_t now = millis();
  if (raw != d.last) { d.last = raw; d.changed = now; }
  if (raw != d.stable && now - d.changed >= DEBOUNCE_MS) {
    d.stable = raw;
    return raw == LOW ? 1 : -1;
  }
  return 0;
}

void onVoiceButton(int i) {
  wakeScreen();                               // voice buttons act even when waking
  if (MUTE_MODE()) {                          // BACK held: toggle this voice's mute
    voices[i].muted = !voices[i].muted;
    markStateDirty();
  } else if (GROUP_MODE()) {
    if (!groupStartedThisHold) {              // first press: new group with this voice
      shiftGroup = 1 << i;
      groupStartedThisHold = true;
    } else {                                  // then add / remove voices
      uint8_t g = shiftGroup ^ (1 << i);
      if (g) shiftGroup = g;                  // never leave the group empty
    }
  } else {
    selVoice = i;
  }
  uiDirty = true;
}

void scanButtons() {
  for (int i = 0; i < NUM_VOICES; i++)
    if (debounce(voiceBtn[i], BTN_PINS[i]) == 1) onVoiceButton(i);

  int s = debounce(shiftBtn, PIN_SHIFT);
  if (s == 1)  { wakeScreen(); shiftHeld = true;  groupStartedThisHold = false; uiDirty = true; }
  if (s == -1) { shiftHeld = false; uiDirty = true; }
}

// ------------------------------------------------------------------ mux encoders
MuxEncoder enc[NUM_ENC];
const int8_t QDEC[16] = { 0, -1, 1, 0,  1, 0, 0, -1,  -1, 0, 0, 1,  0, 1, -1, 0 };

void readMuxes(uint16_t &bits1, uint16_t &bits2) {
  bits1 = 0; bits2 = 0;
  for (int ch = 0; ch < 16; ch++) {
    for (int b = 0; b < 4; b++) digitalWrite(MUX_S[b], (ch >> b) & 1);
    delayMicroseconds(ENC_SETTLE_US);
    if (digitalRead(MUX1_SIG)) bits1 |= (1u << ch);
    if (digitalRead(MUX2_SIG)) bits2 |= (1u << ch);
  }
}

static inline uint8_t encoderPins(int i, uint16_t bits1, uint16_t bits2) {
  uint16_t bits = (i < 8) ? bits1 : bits2;
  int k = i % 8;
  uint8_t a = (bits >> (2 * k)) & 1;
  uint8_t b = (bits >> (2 * k + 1)) & 1;
  return ENC_REVERSE ? (b << 1 | a) : (a << 1 | b);
}

int decodeEncoder(int i, uint8_t s) {
  MuxEncoder &e = enc[i];
  if (s == e.prev) { e.pending = s; return 0; }
#if ENC_FILTER
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
  if (micros() - lastLedChangeUs < ENC_BLANK_US) return;
  uint16_t bits1, bits2;
  readMuxes(bits1, bits2);

  for (int i = 0; i < NUM_ENC; i++) {
    int d = decodeEncoder(i, encoderPins(i, bits1, bits2));
    if (!d || PARAM_MAX[i] == 0) continue;
    if (ENC_INVERT[i]) d = -d;
    wakeScreen();                             // voice encoders act even when waking

    // which voices this turn edits: the shift group, or just the selected voice
    uint8_t targets = GROUP_MODE() ? shiftGroup : (1 << selVoice);
    int shown = -1;
    for (int v = 0; v < NUM_VOICES; v++) {
      if (!((targets >> v) & 1)) continue;
      Voice &vc = voices[v];
      int p = vc.param[i] + d;                // same amount for every voice in the group
      if (p < 0) p = 0;
      if (p > PARAM_MAX[i]) p = PARAM_MAX[i];
      if (p != vc.param[i]) { vc.param[i] = p; applyParam(v, i); markStateDirty(); }
      if (shown < 0 || v == selVoice) shown = v;
    }
    lastParam = i;
    lastParamMs = millis();
    lastParamVoice = shown;
    if (uiScreen == UI_MAIN) uiDirty = true;
#if DEBUG_SERIAL
    char val[20]; formatParam(shown, i, val, sizeof(val));
    Serial.print(GROUP_MODE() ? "group " : "v"); Serial.print(GROUP_MODE() ? shiftGroup : selVoice + 1);
    Serial.print(" "); Serial.print(PARAM_LABEL[i]); Serial.print(": "); Serial.println(val);
#endif
  }
}

// ------------------------------------------------------------------ menu encoder + button
uint8_t menuEncPrev = 0;
uint8_t menuEncPending = 0;      // candidate state, must read the same twice (noise filter)
uint8_t menuEncRest = 0;         // A/B state at a detent (read at boot)
int8_t  menuEncAcc  = 0;

uint8_t readMenuEnc() {
  uint8_t a = digitalRead(PIN_MENU_A), b = digitalRead(PIN_MENU_B);
  return MENU_ENC_REVERSE ? (b << 1 | a) : (a << 1 | b);
}

uint8_t wrapAdd(uint8_t v, int d, uint8_t n) {
  int r = ((int)v + d) % n;
  if (r < 0) r += n;
  return (uint8_t)r;
}

void applyMidiChannel() {
  MIDI.setInputChannel(midiChannel == 0 ? MIDI_CHANNEL_OMNI : midiChannel);
}

// actions offered for a slot: empty -> SAVE only, used -> LOAD / SAVE / CLEAR
uint8_t actionCount(uint8_t slot) { return presetUsed[slot] ? 3 : 1; }
PresetAction actionAt(uint8_t slot, uint8_t idx) {
  if (!presetUsed[slot]) return PA_SAVE;
  return (PresetAction)idx;            // PA_LOAD, PA_SAVE, PA_CLEAR
}

uint8_t nameCharIndex(char c) {
  for (uint8_t i = 0; i < N_NAME_CHARS; i++) if (NAME_CHARS[i] == c) return i;
  return N_NAME_CHARS - 1;             // unknown -> space
}

void startNameEntry(uint8_t slot) {
  if (presetUsed[slot]) memcpy(nameBuf, presetName[slot], NAME_LEN);   // overwrite: keep its name
  else                  memcpy(nameBuf, "NEW  ", NAME_LEN);
  nameBuf[NAME_LEN] = 0;
  nameCursor = 0;
  uiScreen = UI_NAME;
}

void onMenuTurn(int d) {
  if (wakeScreen()) return;                   // first touch only wakes the screen
  switch (uiScreen) {
    case UI_MAIN:          return;
    case UI_TOP:           topSel = wrapAdd(topSel, d, TI_COUNT); break;
    case UI_PRESETS:       listSel = wrapAdd(listSel, d, NUM_PRESETS + 1); break;      // INIT + slots
    case UI_INIT:          return;
    case UI_PRESET_ACTION: actionSel = wrapAdd(actionSel, d, actionCount(presetSel)); break;
    case UI_NAME:
      if (shiftHeld) {                        // SHIFT + turn: change the character
        uint8_t ci = wrapAdd(nameCharIndex(nameBuf[nameCursor]), d, N_NAME_CHARS);
        nameBuf[nameCursor] = NAME_CHARS[ci];
      } else {                                // turn: move between characters 1-5
        nameCursor = wrapAdd(nameCursor, d, NAME_LEN);
      }
      break;
    case UI_SETTINGS:      menuSel = wrapAdd(menuSel, d, MI_COUNT); break;
    case UI_EDIT_MIDI:     editIdx = wrapAdd(editIdx, d, 17); break;         // OMNI + 1..16
    case UI_EDIT_LEDS:     editIdx = wrapAdd(editIdx, d, N_LED_LEVELS);
                           ledLevel = editIdx; ledOut[0] = ledOut[1] = ledOut[2] = ledOut[3] = -1;  // live preview
                           break;
    case UI_EDIT_VALUES:   editIdx = wrapAdd(editIdx, d, 2); break;          // ON / OFF
    case UI_EDIT_DISPLAY:  editIdx = wrapAdd(editIdx, d, N_SAVER); break;
  }
  uiDirty = true;
}

void onMenuPress() {                          // open / select / confirm
  if (wakeScreen()) return;
  switch (uiScreen) {
    case UI_MAIN: uiScreen = UI_TOP; topSel = 0; break;
    case UI_TOP:
      if (topSel == TI_PRESETS) uiScreen = UI_PRESETS;            // remembers the last slot
      else                      { uiScreen = UI_SETTINGS; menuSel = 0; }
      break;
    case UI_PRESETS:
      if (listSel == 0) { uiScreen = UI_INIT; break; }
      if (!flashOk) { showToast("NO FLASH", "CAN'T SAVE"); break; }
      presetSel = listSel - 1;
      actionSel = 0;
      uiScreen = UI_PRESET_ACTION;
      break;
    case UI_INIT:
      initSound();
      showToast("INIT", "DEFAULT SOUND");
      uiScreen = UI_MAIN;
      break;
    case UI_PRESET_ACTION:
      switch (actionAt(presetSel, actionSel)) {
        case PA_SAVE:  startNameEntry(presetSel); break;
        case PA_LOAD:  queuePresetOp(OP_LOAD, presetSel, nullptr);  uiScreen = UI_MAIN;    break;
        case PA_CLEAR: queuePresetOp(OP_CLEAR, presetSel, nullptr); uiScreen = UI_PRESETS; break;
      }
      break;
    case UI_NAME:
      queuePresetOp(OP_SAVE, presetSel, nameBuf);
      uiScreen = UI_PRESETS;
      break;
    case UI_SETTINGS:
      switch (menuSel) {
        case MI_MIDI:    editIdx = midiChannel; uiScreen = UI_EDIT_MIDI;    break;
        case MI_LEDS:    editIdx = ledLevel; ledLevelOrig = ledLevel; uiScreen = UI_EDIT_LEDS; break;
        case MI_VALUES:  editIdx = showValues ? 0 : 1; uiScreen = UI_EDIT_VALUES; break;
        case MI_DISPLAY: editIdx = saverIdx;    uiScreen = UI_EDIT_DISPLAY; break;
      }
      break;
    case UI_EDIT_MIDI:
      if (editIdx != midiChannel) { midiChannel = editIdx; applyMidiChannel(); requestSettingsSave(); }
      uiScreen = UI_SETTINGS; break;
    case UI_EDIT_LEDS:
      ledLevel = editIdx;
      if (ledLevel != ledLevelOrig) requestSettingsSave();
      uiScreen = UI_SETTINGS; break;
    case UI_EDIT_VALUES: {
      uint8_t v = (editIdx == 0) ? 1 : 0;
      if (v != showValues) { showValues = v; requestSettingsSave(); }
      uiScreen = UI_SETTINGS; break;
    }
    case UI_EDIT_DISPLAY:
      if (editIdx != saverIdx) { saverIdx = editIdx; requestSettingsSave(); }
      uiScreen = UI_SETTINGS; break;
  }
  uiDirty = true;
}

void onBack() {                               // BACK button: cancel / up one level
  if (wakeScreen()) return;
  switch (uiScreen) {
    case UI_MAIN:          return;
    case UI_TOP:           uiScreen = UI_MAIN; break;
    case UI_PRESETS:       uiScreen = UI_TOP; break;
    case UI_PRESET_ACTION: uiScreen = UI_PRESETS; break;
    case UI_INIT:          uiScreen = UI_PRESETS; break;         // nothing changed
    case UI_NAME:          uiScreen = UI_PRESET_ACTION; break;   // cancelled: nothing saved
    case UI_SETTINGS:      uiScreen = UI_TOP; break;
    case UI_EDIT_LEDS:                        // undo the live preview
      ledLevel = ledLevelOrig;
      ledOut[0] = ledOut[1] = ledOut[2] = ledOut[3] = -1;
      uiScreen = UI_SETTINGS;
      break;
    default:               uiScreen = UI_SETTINGS; break;        // edit cancelled: nothing applied
  }
  uiDirty = true;
}

void scanMenuControls() {
  // Menu encoder (direct pins). One step per detent, counted when the encoder
  // settles back into a detent: bounce or a missed transition in between can't
  // add or drop a step, which is what made the list skip slots.
  uint8_t s = readMenuEnc();
  if (s == menuEncPrev) { menuEncPending = s; }
  else if (s != menuEncPending) { menuEncPending = s; }        // seen once: wait for a second read
  else {
    menuEncAcc += QDEC[(menuEncPrev << 2) | s];
    menuEncPrev = s;
    // detent positions: the boot state for full-cycle encoders (4 steps per click),
    // or both 00 and 11 for half-cycle ones (MENU_ENC_STEPS 2)
    bool atDetent = (MENU_ENC_STEPS == 2) ? (s == 0 || s == 3) : (s == menuEncRest);
    if (atDetent) {
      if      (menuEncAcc >=  2) onMenuTurn(+1);
      else if (menuEncAcc <= -2) onMenuTurn(-1);
      menuEncAcc = 0;
    }
  }

  // buttons act on press
  if (debounce(menuBtn, PIN_MENU_BTN) == 1) onMenuPress();
  if (debounce(backBtn, PIN_BACK) == 1)     onBack();
  bool held = (backBtn.stable == LOW);
  if (held != backHeld) { backHeld = held; uiDirty = true; }
}

// ------------------------------------------------------------------ screen saver
void processScreenSaver() {
  uint8_t mins = SAVER_MIN[saverIdx];
  if (screenAsleep || mins == 0) return;
  if (millis() - lastActivity < (uint32_t)mins * 60000UL) return;
  if (uiScreen == UI_EDIT_LEDS) {             // leave any half-finished edit
    ledLevel = ledLevelOrig;
    ledOut[0] = ledOut[1] = ledOut[2] = ledOut[3] = -1;
  }
  uiScreen = UI_MAIN;
  screenAsleep = true;
  display.ssd1306_command(SSD1306_DISPLAYOFF);
}

// ------------------------------------------------------------------ drawing (DCLK style)
void drawRightAligned(const char *s, int16_t y) {
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(s, 0, y, &x1, &y1, &w, &h);
  display.setCursor(display.width() - w - x1 - 1, y);   // 1 px right margin
  display.print(s);
}

void groupText(char *buf, size_t n) {
  if (shiftGroup == 0x0F) { snprintf(buf, n, "ALL"); return; }
  size_t k = 0;
  for (int v = 0; v < NUM_VOICES && k + 2 < n; v++) {
    if (!((shiftGroup >> v) & 1)) continue;
    buf[k++] = '1' + v;               // e.g. "13" = voices 1 and 3 (fits beside long labels)
  }
  buf[k] = 0;
}

// "1. - - -" for an empty slot, "1. NAME" for a used one (trailing spaces trimmed)
void presetSlotText(uint8_t slot, char *buf, size_t n) {
  if (!presetUsed[slot]) { snprintf(buf, n, "%u. - - -", slot + 1); return; }
  char nm[NAME_LEN + 1];
  memcpy(nm, presetName[slot], NAME_LEN + 1);
  for (int k = NAME_LEN - 1; k >= 0 && nm[k] == ' '; k--) nm[k] = 0;
  snprintf(buf, n, "%u. %s", slot + 1, nm);
}

void showToast(const char *l1, const char *l2) {
  snprintf(toast1, sizeof(toast1), "%s", l1);
  snprintf(toast2, sizeof(toast2), "%s", l2);
  toastUntil = millis() + 1500;
  if (toastUntil == 0) toastUntil = 1;
  uiDirty = true;
}

// prints text at size 3 from the cursor; drops to size 2 if it would run off the right edge
void printFit(const char *t, int16_t y) {
  int16_t x1, y1; uint16_t w, h;
  display.setTextSize(3);
  display.getTextBounds(t, display.getCursorX(), y, &x1, &y1, &w, &h);
  if (x1 + (int16_t)w > display.width() - 1) display.setTextSize(2);
  display.print(t);
  display.setTextSize(3);
}

void midiChannelText(uint8_t ch, char *buf, size_t n) {
  if (ch == 0) snprintf(buf, n, "OMNI");
  else         snprintf(buf, n, "CH %u", ch);
}

void saverText(uint8_t idx, char *buf, size_t n) {
  if (SAVER_MIN[idx] == 0) snprintf(buf, n, "OFF");
  else                     snprintf(buf, n, "%uM", SAVER_MIN[idx]);
}

// ------------------------------------------------------------------ generative art (main screen)
// Everything is drawn from the voices' live settings and envelopes each frame.
const uint8_t BAYER4[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
#define N_STARS 28
const float ART_VOICE_X[NUM_VOICES] = { 30.0f, 52.0f, 82.0f, 108.0f };   // planet centres (x)

static inline float pnorm(int v, int id) { return PARAM_MAX[id] ? (float)voices[v].param[id] / PARAM_MAX[id] : 0.0f; }

void drawArt() {
  static bool     init = false;
  static float    sx[N_STARS], sy[N_STARS];
  static uint8_t  layer[N_STARS];
  static float    spin[NUM_VOICES];
  static uint32_t lastMs = 0;
  uint32_t now = millis();
  float dt = lastMs ? (now - lastMs) * 0.001f : 0.0f;
  if (dt > 0.2f) dt = 0.2f;
  lastMs = now;

  if (!init) {                                   // fixed pseudo-random starfield
    uint32_t r = 0x4D45u;
    for (int k = 0; k < N_STARS; k++) {
      r = r * 1103515245u + 12345u; sx[k] = (r >> 16) % OLED_W;
      r = r * 1103515245u + 12345u; sy[k] = (r >> 16) % OLED_H;
      layer[k] = 1 + (k % 3);                    // 3 parallax layers
    }
    init = true;
  }

  // stars drift left, faster when the LFOs are faster
  float avgRate = 0.0f;
  for (int v = 0; v < NUM_VOICES; v++) avgRate += voices[v].set.lfoRate;
  avgRate /= NUM_VOICES;
  float drift = 2.0f + 6.0f * log2f(1.0f + avgRate);          // px / s for the far layer
  for (int k = 0; k < N_STARS; k++) {
    sx[k] -= drift * layer[k] * 0.5f * dt;
    if (sx[k] < 0) sx[k] += OLED_W;
    display.drawPixel((int)sx[k], (int)sy[k], SSD1306_WHITE);
  }

  // one planet per voice
  for (int v = 0; v < NUM_VOICES; v++) {
    Voice &vc = voices[v];
    bool  muted = vc.muted;
    float env   = muted ? 0.0f : clampf(vc.envOut, 0.0f, 1.0f);   // muted: no pulse
    float level = pnorm(v, P_LEVEL);
    float pan   = pnorm(v, P_PAN) * 2.0f - 1.0f;
    float cx    = ART_VOICE_X[v] + pan * 6.0f;
    float cy    = 19.0f - clampf(vc.set.tune / 12.0f, -1.0f, 1.0f) * 3.0f;
    float r     = 2.0f + 3.0f * level + 5.5f * env;

    // shape from the wave scrub: sides 3 (tri) -> 12 (saw, round) -> 4 (square) -> bar (PWM)
    float w = clampf(vc.set.wave, 0.0f, 3.0f);
    int   sides; float xs = 1.0f;
    if (w <= 1.0f)      sides = 3 + (int)(w * 9.0f + 0.5f);
    else if (w <= 2.0f) sides = 12 - (int)((w - 1.0f) * 8.0f + 0.5f);
    else              { sides = 4; xs = 1.0f - (w - 2.0f) * 0.75f; }

    spin[v] += dt * vc.set.lfoRate * 0.6f;                     // LFO rate -> spin
    if (spin[v] > TWOPI_F) spin[v] -= TWOPI_F;
    float wob   = 1.0f + 0.3f * pnorm(v, P_PITCH_LFO) * vc.lfoVal;   // pitch LFO -> wobble
    float rr    = r * wob;
    float sector = TWOPI_F / sides;
    float apo    = cosf(PI_F / sides);
    float dens   = clampf(vc.set.cutoffOct / CUTOFF_MAX_OCT, 0.0f, 1.0f);   // cutoff -> fill
    int   thresh = (int)(dens * dens * 16.0f + 0.5f);

    int x0 = (int)(cx - rr - 1), x1 = (int)(cx + rr + 1);
    int y0 = (int)(cy - rr - 1), y1 = (int)(cy + rr + 1);
    for (int y = y0; y <= y1; y++) {
      if (y < 0 || y >= OLED_H) continue;
      for (int x = x0; x <= x1; x++) {
        if (x < 0 || x >= OLED_W) continue;
        float dx = (x - cx) / xs, dy = y - cy;
        float d = sqrtf(dx * dx + dy * dy);
        if (d > rr + 1.0f) continue;
        float th = atan2f(dy, dx) - spin[v];
        float ph = fmodf(th, sector); if (ph < 0) ph += sector;
        float edge = rr * apo / cosf(ph - sector * 0.5f);      // regular polygon radius at this angle
        if (d > edge) continue;
        bool outline = d > edge - 1.0f;
        if (outline || (!muted && BAYER4[(y & 3) * 4 + (x & 3)] < thresh))   // muted: hollow
          display.drawPixel(x, y, SSD1306_WHITE);
        else display.drawPixel(x, y, SSD1306_BLACK);           // planets hide the stars behind them
      }
    }

    // resonance -> spikes, stretched by the cutoff envelope amount while the note sounds
    float res = vc.set.res / 0.9f;
    if (res > 0.15f) {
      int   n   = 3 + (int)(res * 6.0f);
      float len = 1.5f + 2.5f * res + 4.0f * env * clampf(vc.set.fltEnv / 5.0f, 0.0f, 1.0f);
      for (int k = 0; k < n; k++) {
        float a = -spin[v] * 0.5f + k * TWOPI_F / n;
        float ca = cosf(a), sa = sinf(a);
        display.drawLine((int)(cx + ca * (rr + 1.5f) * xs), (int)(cy + sa * (rr + 1.5f)),
                         (int)(cx + ca * (rr + 1.5f + len) * xs), (int)(cy + sa * (rr + 1.5f + len)),
                         SSD1306_WHITE);
      }
    }

    // marker under the voice(s) the encoders edit
    bool edited = GROUP_MODE() ? ((shiftGroup >> v) & 1) : (v == selVoice);
    if (edited) display.drawFastHLine((int)cx - 3, OLED_H - 1, 7, SSD1306_WHITE);
  }
}

void drawUI() {
  // Layouts for a 128x32 panel, DCLK style. Picopixel glyphs sit from 4 px
  // above the baseline to 0 px below it (x text size), and Q has a 1 px tail.
  // So at size 3 a row spans baseline-12 .. baseline+2, and the bottom row's
  // baseline is 29 (26 for the parameter value, so the tail of Q fits).
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setFont(&Picopixel);
  display.setTextWrap(false);     // right-aligned text ends at the edge; wrap would push it off-screen
  char buf[24], who[16];

  // who the encoders edit: "V2", or "S ALL" / "S 1+3" while SHIFT is held
  if (MUTE_MODE())        snprintf(who, sizeof(who), "MUTE");
  else if (GROUP_MODE()) { groupText(buf, sizeof(buf)); snprintf(who, sizeof(who), "S %s", buf); }
  else                    snprintf(who, sizeof(who), "V%d", selVoice + 1);

  if (toastUntil) {                           // SAVED / LOADED / CLEARED message
    display.setTextSize(3);
    display.setCursor(0, 12); display.print(toast1);
    display.setCursor(0, 29); printFit(toast2, 29);
    return;
  }

  switch (uiScreen) {
    case UI_MAIN: {
      uint32_t now = millis();
      if (showValues && lastParam >= 0 && now - lastParamMs < PARAM_HOLD_MS) {   // being turned
        display.setTextSize(2);
        display.setCursor(0, 10);
        display.print(PARAM_LABEL[lastParam]);
        drawRightAligned(who, 10);
        display.setTextSize(3);
        formatParam(lastParamVoice, lastParam, buf, sizeof(buf));
        drawRightAligned(buf, 26);
      } else {                                // at rest: art + which voice is selected
        drawArt();
        int16_t x1, y1; uint16_t w, h;
        display.setTextSize(2);
        display.getTextBounds(who, 1, 10, &x1, &y1, &w, &h);
        display.fillRect(0, 0, x1 + w + 2, y1 + h + 2, SSD1306_BLACK);   // keep the label readable
        display.setCursor(1, 10);
        display.print(who);
      }
      break;
    }

    case UI_TOP:
    case UI_SETTINGS: {                       // DCLK two-line paged list
      const char **labels = (uiScreen == UI_TOP) ? TOP_LABEL : MENU_LABEL;
      uint8_t count = (uiScreen == UI_TOP) ? (uint8_t)TI_COUNT : (uint8_t)MI_COUNT;
      uint8_t sel   = (uiScreen == UI_TOP) ? topSel : menuSel;
      display.setTextSize(3);
      uint8_t top = sel & ~1;
      for (uint8_t row = 0; row < 2; row++) {
        uint8_t item = top + row;
        if (item >= count) break;
        display.setCursor(0, row ? 29 : 12);
        display.print(item == sel ? "> " : "  ");
        display.print(labels[item]);
      }
      break;
    }

    case UI_PRESETS: {                        // INIT, then the slots, two rows per page
      uint8_t top = listSel & ~1;
      for (uint8_t row = 0; row < 2; row++) {
        uint8_t idx = top + row;
        if (idx > NUM_PRESETS) break;
        int16_t y = row ? 29 : 12;
        display.setTextSize(3);
        display.setCursor(0, y);
        display.print(idx == listSel ? "> " : "  ");
        if (idx == 0) display.print("INIT");
        else { presetSlotText(idx - 1, buf, sizeof(buf)); printFit(buf, y); }
      }
      break;
    }

    case UI_INIT: {                           // press again to confirm, BACK to cancel
      display.setTextSize(3);
      display.setCursor(0, 12);
      display.print("INIT");
      display.setCursor(0, 29);
      display.print("> CONFIRM");
      break;
    }

    case UI_PRESET_ACTION: {                  // slot on top, action below (turn to change)
      presetSlotText(presetSel, buf, sizeof(buf));
      display.setTextSize(3);
      display.setCursor(0, 12);
      printFit(buf, 12);
      display.setCursor(0, 29);
      display.print("> ");
      display.print(ACTION_LABEL[actionAt(presetSel, actionSel)]);
      if (actionCount(presetSel) > 1) {       // e.g. "2/3" so it's clear there's more
        snprintf(buf, sizeof(buf), "%u/%u", actionSel + 1, actionCount(presetSel));
        display.setTextSize(2);
        drawRightAligned(buf, 29);
      }
      break;
    }

    case UI_NAME: {                           // 5 fixed-width character cells, cursor bar under one
      display.setTextSize(2);
      display.setCursor(0, 10);
      display.print("NAME");
      drawRightAligned(shiftHeld ? "A-Z 0-9" : "SHIFT=ABC", 10);
      const int16_t CELL = 20, BASE = 25;   // 25: the tail of Q stays clear of the cursor line
      display.setTextSize(3);
      for (int k = 0; k < NAME_LEN; k++) {
        char c[2] = { nameBuf[k], 0 };
        int16_t x1, y1; uint16_t w, h;
        display.getTextBounds(c, 0, BASE, &x1, &y1, &w, &h);
        display.setCursor(k * CELL + (CELL - 2 - (int16_t)w) / 2 - x1, BASE);   // centre in the cell
        display.print(c);
        if (k == nameCursor) display.drawFastHLine(k * CELL, 31, CELL - 4, SSD1306_WHITE);      // cursor
        else                 display.drawFastHLine(k * CELL + 6, 31, CELL - 16, SSD1306_WHITE); // slot marker
      }
      break;
    }

    case UI_EDIT_MIDI:
    case UI_EDIT_LEDS:
    case UI_EDIT_VALUES:
    case UI_EDIT_DISPLAY: {                   // DCLK edit screen: title, then "SET >" value
      display.setTextSize(3);
      display.setCursor(0, 12);
      display.print(">");
      display.print(uiScreen == UI_EDIT_MIDI ? "MIDI" : uiScreen == UI_EDIT_LEDS ? "LEDS" :
                    uiScreen == UI_EDIT_VALUES ? "VALUES" : "DISPLAY");
      display.setCursor(0, 29);
      display.print("SET >");
      if (uiScreen == UI_EDIT_MIDI)    midiChannelText(editIdx, buf, sizeof(buf));
      if (uiScreen == UI_EDIT_LEDS)    snprintf(buf, sizeof(buf), "%s", LED_LEVELS[editIdx].name);
      if (uiScreen == UI_EDIT_VALUES)  snprintf(buf, sizeof(buf), "%s", editIdx == 0 ? "ON" : "OFF");
      if (uiScreen == UI_EDIT_DISPLAY) saverText(editIdx, buf, sizeof(buf));
      drawRightAligned(buf, 29);
      break;
    }
  }
}

// ------------------------------------------------------------------ non-blocking OLED transfer
// A full 128x32 frame is 512 bytes = ~13 ms over I2C, long enough to miss MIDI bytes
// and encoder steps. So the frame is drawn into RAM, then sent a small chunk
// (up to 31 bytes, under 1 ms) per pass through loop().
#define OLED_CHUNK 31
int8_t   flushPage = -1;             // -1 = idle
uint8_t  flushCol  = 0;

void startFlush() { flushPage = 0; flushCol = 0; }

void serviceFlush() {
  if (flushPage < 0) return;
  Wire.setClock(OLED_I2C_HZ);
  if (flushCol == 0) {                       // point the panel at the start of this page
    Wire.beginTransmission(OLED_ADDR);
    Wire.write((uint8_t)0x00);               // command stream
    Wire.write((uint8_t)0x21); Wire.write((uint8_t)0); Wire.write((uint8_t)127);
    Wire.write((uint8_t)0x22); Wire.write((uint8_t)flushPage); Wire.write((uint8_t)flushPage);
    Wire.endTransmission();
  }
  const uint8_t *p = display.getBuffer() + flushPage * OLED_W + flushCol;
  uint8_t len = 128 - flushCol;
  if (len > OLED_CHUNK) len = OLED_CHUNK;
  Wire.beginTransmission(OLED_ADDR);
  Wire.write((uint8_t)0x40);                 // data stream
  Wire.write(p, len);
  Wire.endTransmission();

  flushCol += len;
  if (flushCol >= 128) {
    flushCol = 0;
    if (++flushPage >= OLED_H / 8) flushPage = -1;   // frame done (8 px per page)
  }
}

void serviceDisplay() {
  if (toastUntil && (int32_t)(millis() - toastUntil) >= 0) { toastUntil = 0; uiDirty = true; }
  if (screenAsleep) return;
  static uint32_t lastFrame = 0;               // main screen animates at ART_FRAME_MS
  if (uiScreen == UI_MAIN && millis() - lastFrame >= ART_FRAME_MS) { lastFrame = millis(); uiDirty = true; }
  if (uiDirty && flushPage < 0) {            // start a new frame only once the last one is out
    uiDirty = false;
    drawUI();
    startFlush();
  }
  serviceFlush();
}

// ------------------------------------------------------------------ MIDI in (round-robin)
int nextVoice = 0;

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
  vc.note = n; vc.heldNote = n; vc.trigCount++; vc.gate = true;
  wakeScreen();                               // a note wakes the display (and resets the timeout)
  updateNoteLed();
  if (!vc.muted) flashVoiceLed(v);
}

void noteOff(uint8_t n) {
  for (int v = 0; v < NUM_VOICES; v++)
    if (voices[v].heldNote == n) { voices[v].heldNote = -1; voices[v].gate = false; }
  updateNoteLed();
}

void allNotesOff() {
  for (int v = 0; v < NUM_VOICES; v++) { voices[v].heldNote = -1; voices[v].gate = false; }
  updateNoteLed();
}

void handleNoteOn(byte channel, byte note, byte velocity)  { noteOn(note); }
void handleNoteOff(byte channel, byte note, byte velocity) { noteOff(note); }
void handlePitchBend(byte channel, int bend) {
#if BEND_ENABLE
  bendSt = bend / 8192.0f * BEND_RANGE_ST;
#endif
}
void handleControlChange(byte channel, byte cc, byte value) {
  if (cc == 120 || cc == 123) allNotesOff();
}

void pollMidi() { while (MIDI.read()) { } }

// ------------------------------------------------------------------ setup / loop
// ------------------------------------------------------------------ boot animation (DCLK style)
void setContrast(uint8_t c) {
  display.ssd1306_command(0x81);              // SSD1306_SETCONTRAST
  display.ssd1306_command(c);
}

// x that centres s at this text size
int16_t centredX(const char *s, uint8_t size, int16_t y, uint16_t &w) {
  int16_t x1, y1; uint16_t h;
  display.setTextSize(size);
  display.getTextBounds(s, 0, y, &x1, &y1, &w, &h);
  return (display.width() - w) / 2 - x1;
}

void bootAnimation() {                        // blocking: runs before audio and MIDI start
  display.setFont(&Picopixel);
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);
  const LedLevel &L = LED_LEVELS[ledLevel];   // LEDs flash at the saved brightness
  uint8_t peak = (uint8_t)min(255, L.base + L.flash);
  uint16_t w; int16_t x;

  // 1. maker name fades in
  display.clearDisplay();
  x = centredX(".axs instruments", 2, 20, w);
  display.setCursor(x, 20);
  display.print(".axs instruments");
  setContrast(0);
  display.display();
  for (int c = 0; c <= 0xCF; c += 9) { setContrast(c); delay(12); }
  setContrast(0xCF);
  delay(450);

  // 2. count-in: one bar per voice drops in, its LED flashing with it
  const int barW = display.width() / NUM_VOICES;
  for (int b = 0; b < NUM_VOICES; b++) {
    display.clearDisplay();
    for (int i = 0; i <= b; i++)
      display.fillRect(i * barW + 2, (i == b) ? 31 : 28, barW - 4, (i == b) ? 1 : 4, SSD1306_WHITE);
    display.display();
    ledWrite(b, peak);
    delay(60);
    display.clearDisplay();                   // the new bar settles into place
    for (int i = 0; i <= b; i++) display.fillRect(i * barW + 2, 28, barW - 4, 4, SSD1306_WHITE);
    display.display();
    ledWrite(b, 0);
    delay(90);
  }

  // 3. instrument name wipes in above the bars
  x = centredX("4-TME", 4, 22, w);
  for (int step = 1; step <= 10; step++) {
    display.clearDisplay();
    display.setTextSize(4);
    display.setCursor(x, 22);
    display.print("4-TME");
    int16_t shown = x + (int16_t)((w + 4) * step / 10);
    display.fillRect(shown, 0, display.width() - shown, 26, SSD1306_BLACK);
    for (int i = 0; i < NUM_VOICES; i++) display.fillRect(i * barW + 2, 28, barW - 4, 4, SSD1306_WHITE);
    display.display();
    delay(25);
  }
  for (int i = 0; i < NUM_VOICES; i++) ledWrite(i, peak);
  delay(120);
  for (int i = 0; i < NUM_VOICES; i++) ledWrite(i, 0);
  delay(380);

  for (int i = 0; i < NUM_VOICES; i++) ledOut[i] = -1;   // updateLeds() takes over from here
}

void setup() {
#if DEBUG_SERIAL
  Serial.begin(115200);
#endif
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;     // cycle counter for the CPU meter
  DWT->LAR = 0xC5ACCE55;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  hw = DAISY.init(DAISY_SEED, AUDIO_SR_48K);
  sampleRate = DAISY.get_samplerate();
  DAISY.SetAudioBlockSize(AUDIO_BLOCK_SIZE);
  smoothK  = 1.0f - expf(-1.0f / (SMOOTH_MS * 0.001f * sampleRate));
  smoothKc = 1.0f - expf(-(float)CONTROL_RATE / (SMOOTH_MS * 0.001f * sampleRate));

  // muxes + encoders
  for (int b = 0; b < 4; b++) { pinMode(MUX_S[b], OUTPUT); digitalWrite(MUX_S[b], LOW); }
  pinMode(MUX1_SIG, INPUT_PULLUP);
  pinMode(MUX2_SIG, INPUT_PULLUP);
  uint16_t bits1, bits2;
  readMuxes(bits1, bits2);
  for (int i = 0; i < NUM_ENC; i++) {
    enc[i].prev = enc[i].pending = encoderPins(i, bits1, bits2);
    enc[i].acc = 0; enc[i].lastClick = 0;
  }

  // buttons, menu encoder, LEDs
  for (int i = 0; i < NUM_VOICES; i++) {
    pinMode(BTN_PINS[i], INPUT_PULLUP);
    voiceBtn[i] = { HIGH, HIGH, 0 };
    pinMode(LED_PINS[i], OUTPUT);
    digitalWrite(LED_PINS[i], LOW);
#if LED_PWM == 1
    ledHasPwm[i] = digitalPinHasPWM(LED_PINS[i]);
#endif
  }
  pinMode(PIN_SHIFT, INPUT_PULLUP);    shiftBtn = { HIGH, HIGH, 0 };
  pinMode(PIN_MENU_BTN, INPUT_PULLUP); menuBtn  = { HIGH, HIGH, 0 };
  pinMode(PIN_BACK, INPUT_PULLUP);     backBtn  = { HIGH, HIGH, 0 };
  pinMode(PIN_MENU_A, INPUT_PULLUP);
  pinMode(PIN_MENU_B, INPUT_PULLUP);
  menuEncPrev = menuEncPending = menuEncRest = readMenuEnc();
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  // voices
  for (int v = 0; v < NUM_VOICES; v++) {
    voices[v].init(sampleRate);
    for (int id = 0; id < P_COUNT; id++) voices[v].param[id] = PARAM_DEFAULT[id];
    for (int id = 0; id < P_COUNT; id++) applyParam(v, id);
    voices[v].settle();
  }

  // external flash: load saved settings before anything uses them
  flashInit();
  loadSettings();
  scanPresets();
  loadState();                                // the sound as it was when switched off

  // OLED (blocking transfers are fine here, before audio and MIDI start)
  Wire.setSCL(PIN_OLED_SCL);
  Wire.setSDA(PIN_OLED_SDA);
  display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  display.setRotation(OLED_ROTATION);

#if LED_PWM == 1
  analogWriteResolution(8);
  analogWriteFrequency(LED_PWM_HZ);
#endif
  bootAnimation();                            // also checks every LED and the display
  if (!flashOk) showToast("NO FLASH", "PRESETS OFF");
  markActivity();

  DAISY.begin(AudioCallback);

  // MIDI on D30 (PB15 = USART1 RX, alternate pin), started after the audio
  Serial1.setRx(MIDI_RX_PIN);
  Serial1.begin(31250);
  Serial1.flush();
  MIDI.setHandleNoteOn(handleNoteOn);
  MIDI.setHandleNoteOff(handleNoteOff);
  MIDI.setHandlePitchBend(handlePitchBend);
  MIDI.setHandleControlChange(handleControlChange);
  MIDI.begin(MIDI_CHANNEL_OMNI);
  applyMidiChannel();
  MIDI.turnThruOff();
}

void loop() {
  pollMidi();
  scanButtons();
  scanMenuControls();
  pollMidi();
  scanEncoders();
  updateLeds();
  pollMidi();
  serviceDisplay();                           // at most one small I2C chunk per pass
  serviceSettingsSave();
  servicePresetOps();
  serviceStateSave();
  flashService();                             // background erase / program, never waits
  processScreenSaver();

#if DEBUG_SERIAL
  static uint32_t lastPrint = 0, loopCount = 0;
  loopCount++;
  if (millis() - lastPrint > 2000) {
    float elapsed = (millis() - lastPrint) * 0.001f;
    lastPrint = millis();
    float blockCycles = (float)SystemCoreClock * AUDIO_BLOCK_SIZE / sampleRate;
    uint32_t n = cbCount ? cbCount : 1;
    Serial.print("CPU avg "); Serial.print(100.0f * ((float)cbSumCycles / n) / blockCycles, 1);
    Serial.print("% max ");   Serial.print(100.0f * (float)cbMaxCycles / blockCycles, 1);
    Serial.print("% | loops/s "); Serial.print((uint32_t)(loopCount / elapsed));
    Serial.print(" | note-ons "); Serial.print(noteOnCount);
    Serial.print(" | NaN "); Serial.println(nanCount);
    cbMaxCycles = 0; cbSumCycles = 0; cbCount = 0; loopCount = 0;
  }
#endif
}