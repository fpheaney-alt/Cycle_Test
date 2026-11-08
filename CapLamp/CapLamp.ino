// Capacitive-touch lamp: Seeed XIAO ESP32S3 + WS2812 16-LED ring + copper tape button
//
// WIRING
//   Copper tape wire  -> D10 (GPIO9)   touch-capable pin, no resistor needed
//   WS2812 DIN        -> D1  (GPIO2)   through a ~330 ohm resistor
//   WS2812 5V         -> 5V pin
//   WS2812 GND        -> GND           (share ground with the XIAO)
//
// CONTROLS
//   Tap                       -> lamp on / off
//   Hold (HOLD_MS)            -> brightness ramps while held; direction alternates each hold
//
// SERIAL MONITOR (115200 baud) - readings stream every PRINT_MS
//   t <n>      set touch sensitivity (delta above baseline), e.g. "t 1500"  (smaller = more sensitive)
//   +  /  -    nudge sensitivity by 100
//   r          re-calibrate baseline (don't touch the tape)
//   c R G B    set lamp colour live, e.g. "c 255 140 40"
//   e          next lighting effect;  "e candle" or "e 1" picks one;  "e ?" lists them
//   p          pause / resume the readings stream

#include <Adafruit_NeoPixel.h>

// ----------------------- PINS --------------------
const int TOUCH_PIN = 9;   // GPIO9 = D10
const int LED_PIN   = 2;   // GPIO2 = D1
const int NUM_LEDS  = 16;

// ----------------------- COLOUR (edit here) --------------------
uint8_t colorR = 255;      // warm white by default
uint8_t colorG = 147;
uint8_t colorB = 41;

// ----------------------- BRIGHTNESS --------------------
const uint8_t BRIGHT_MIN   = 5;     // lowest the dimmer will go
const uint8_t BRIGHT_MAX   = 200;   // cap (255 = full; lower to limit current draw)
const float   BRIGHT_RATE  = 60.0;  // brightness units per second while holding
float brightness = 100;             // starting brightness

// ----------------------- BUTTON TIMING --------------------
const unsigned long HOLD_MS    = 600;   // press length that counts as a "hold" instead of a tap
const unsigned long MIN_TAP_MS = 30;    // ignore touches shorter than this (noise)

// ----------------------- TOUCH SENSITIVITY --------------------
// On the ESP32-S3 the raw reading goes UP when touched. A touch is detected when
// raw > baseline + touchDelta. Watch the Serial Monitor: touch the tape and see how far
// "raw" rises above "base", then set the delta to roughly half of that rise.
uint32_t touchDelta = 2000;
const float HYSTERESIS = 0.6;           // release when raw falls below baseline + delta * this
const int   SAMPLES    = 4;             // readings averaged per loop
const unsigned long PRINT_MS = 100;

// ----------------------- EFFECTS --------------------
// solid, breathe, twinkle and chase use the colour above (and "c R G B"); candle does too,
// so try something orange like "c 255 90 10". fire and rainbow use their own colours.
enum Effect { FX_SOLID, FX_CANDLE, FX_FIRE, FX_BREATHE, FX_RAINBOW, FX_TWINKLE, FX_CHASE, FX_COUNT };
const char* FX_NAMES[FX_COUNT] = { "solid", "candle", "fire", "breathe", "rainbow", "twinkle", "chase" };
Effect effect = FX_CANDLE;                      // effect at power-up

const unsigned long FRAME_MS = 25;              // animation frame time (40 fps)

// Candle
const float CANDLE_MIN         = 0.55;          // dimmest normal flicker (1.0 = full)
const float CANDLE_GUST_CHANCE = 0.06;          // chance per flicker step of a deep dip
const float CANDLE_GUST_LEVEL  = 0.30;          // how low a dip goes
const float CANDLE_SMOOTH      = 0.25;          // 0..1, higher = jumpier
const float CANDLE_PIXEL_SPREAD = 0.10;         // how much each LED varies from the others

// Fire (classic "Fire2012" heat model, wrapped around the ring)
const int FIRE_COOLING  = 55;                   // higher = shorter, redder flames
const int FIRE_SPARKING = 120;                  // 0..255, higher = more new flare-ups

// Timing for the slow effects (ms for one full cycle)
const unsigned long BREATHE_MS = 4000;
const unsigned long RAINBOW_MS = 8000;
const unsigned long CHASE_MS   = 1500;
const float         CHASE_TAIL = 6.0;           // tail length in LEDs

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// ----------------------- STATE --------------------
float baseline = 0;
bool  touched = false;
bool  lampOn = false;
bool  lastRampUp = false;       // first hold goes brighter
bool  rampingUp = true;
bool  holding = false;          // a hold is in progress (ramping)
unsigned long pressStart = 0;
unsigned long lastFrame = 0;
unsigned long lastPrint = 0;
unsigned long lastRender = 0;
bool  printing = true;
bool  dirty = true;             // LEDs need refreshing

uint32_t readTouch() {
  uint32_t sum = 0;
  for (int i = 0; i < SAMPLES; i++) sum += touchRead(TOUCH_PIN);
  return sum / SAMPLES;
}

void calibrate() {
  Serial.println("Calibrating baseline - don't touch the tape...");
  delay(300);
  uint32_t sum = 0;
  const int n = 40;
  for (int i = 0; i < n; i++) { sum += readTouch(); delay(10); }
  baseline = (float)sum / n;
  Serial.printf("Baseline = %.0f\n", baseline);
}

// ----------------------- EFFECT RENDERERS --------------------
// Each one only sets pixels; renderFrame() applies brightness first and calls show().

uint32_t colorScaled(float f) {
  f = constrain(f, 0.0f, 1.0f);
  return strip.Color((uint8_t)(colorR * f), (uint8_t)(colorG * f), (uint8_t)(colorB * f));
}

void fxSolid() {
  for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, colorScaled(1.0));
}

float candleLevel = 0.8, candleTarget = 0.8;
float pixLevel[NUM_LEDS];
unsigned long candleNext = 0;

void fxCandle(unsigned long now) {
  if (now >= candleNext) {
    // Pick a new brightness target now and then; occasionally a deep dip, like a draught
    if (random(1000) / 1000.0 < CANDLE_GUST_CHANCE) candleTarget = CANDLE_GUST_LEVEL;
    else candleTarget = CANDLE_MIN + (1.0 - CANDLE_MIN) * (random(1000) / 1000.0);
    candleNext = now + random(50, 220);
  }
  candleLevel += (candleTarget - candleLevel) * CANDLE_SMOOTH;

  for (int i = 0; i < NUM_LEDS; i++) {
    float want = candleLevel + (random(-1000, 1001) / 1000.0) * CANDLE_PIXEL_SPREAD;
    pixLevel[i] += (want - pixLevel[i]) * 0.35;
    float f = constrain(pixLevel[i], 0.0f, 1.0f);
    // Dimmer flame shifts redder: green and blue fall faster than red
    strip.setPixelColor(i, strip.Color((uint8_t)(colorR * f), (uint8_t)(colorG * f * f), (uint8_t)(colorB * f * f)));
  }
}

uint8_t heat[NUM_LEDS];

uint32_t heatColor(uint8_t h) {
  uint8_t t192 = (uint16_t)h * 191 / 255;
  uint8_t ramp = (t192 & 0x3F) << 2;
  if (t192 & 0x80) return strip.Color(255, 255, ramp);
  if (t192 & 0x40) return strip.Color(255, ramp, 0);
  return strip.Color(ramp, 0, 0);
}

void fxFire() {
  for (int i = 0; i < NUM_LEDS; i++) {                       // cool every cell a little
    uint8_t cool = random(0, (FIRE_COOLING * 10) / NUM_LEDS + 2);
    heat[i] = heat[i] > cool ? heat[i] - cool : 0;
  }
  uint8_t t[NUM_LEDS];                                       // let heat spread to neighbours
  for (int i = 0; i < NUM_LEDS; i++)
    t[i] = (heat[(i + NUM_LEDS - 1) % NUM_LEDS] + heat[i] * 2 + heat[(i + 1) % NUM_LEDS]) / 4;
  memcpy(heat, t, sizeof(heat));
  if (random(255) < FIRE_SPARKING) {                         // random new spark
    int y = random(NUM_LEDS);
    heat[y] = min(255, heat[y] + (int)random(160, 255));
  }
  for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, heatColor(heat[i]));
}

void fxBreathe(unsigned long now) {
  float phase = (now % BREATHE_MS) / (float)BREATHE_MS;
  float f = 0.12 + 0.88 * (0.5 - 0.5 * cos(2.0 * PI * phase));
  for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, colorScaled(f));
}

void fxRainbow(unsigned long now) {
  uint32_t base = (uint32_t)((uint64_t)(now % RAINBOW_MS) * 65536 / RAINBOW_MS);
  for (int i = 0; i < NUM_LEDS; i++) {
    uint16_t hue = (uint16_t)(base + (uint32_t)i * 65536UL / NUM_LEDS);
    strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(hue)));
  }
}

float twinkle[NUM_LEDS];

void fxTwinkle() {
  if (random(100) < 12) twinkle[random(NUM_LEDS)] = 1.0;      // light a random LED
  for (int i = 0; i < NUM_LEDS; i++) {
    twinkle[i] *= 0.90;                                       // fade
    strip.setPixelColor(i, colorScaled(0.06 + 0.94 * twinkle[i]));
  }
}

void fxChase(unsigned long now) {
  float pos = (now % CHASE_MS) * (float)NUM_LEDS / CHASE_MS;
  for (int i = 0; i < NUM_LEDS; i++) {
    float d = fmod(pos - i + NUM_LEDS, (float)NUM_LEDS);      // distance behind the head
    float f = d < CHASE_TAIL ? 1.0 - d / CHASE_TAIL : 0.0;
    strip.setPixelColor(i, colorScaled(0.03 + 0.97 * f));
  }
}

void renderFrame() {
  dirty = false;
  if (!lampOn) {
    strip.clear();
    strip.show();
    return;
  }
  strip.setBrightness((uint8_t)brightness);   // must come before setPixelColor
  unsigned long now = millis();
  switch (effect) {
    case FX_SOLID:   fxSolid();        break;
    case FX_CANDLE:  fxCandle(now);    break;
    case FX_FIRE:    fxFire();         break;
    case FX_BREATHE: fxBreathe(now);   break;
    case FX_RAINBOW: fxRainbow(now);   break;
    case FX_TWINKLE: fxTwinkle();      break;
    case FX_CHASE:   fxChase(now);     break;
    default:         fxSolid();        break;
  }
  strip.show();
}

void listEffects() {
  Serial.print("Effects:");
  for (int i = 0; i < FX_COUNT; i++) Serial.printf(" %d=%s", i, FX_NAMES[i]);
  Serial.println();
}

void setEffect(const String &arg) {
  int idx = -1;
  if (arg.length() == 0) {
    idx = (effect + 1) % FX_COUNT;                            // "e" alone cycles
  } else if (isDigit(arg.charAt(0))) {
    idx = arg.toInt();
  } else {
    for (int i = 0; i < FX_COUNT; i++)
      if (arg.equalsIgnoreCase(FX_NAMES[i])) idx = i;
  }
  if (idx < 0 || idx >= FX_COUNT) { listEffects(); return; }
  effect = (Effect)idx;
  dirty = true;
  Serial.printf("effect = %s\n", FX_NAMES[effect]);
}

void updateTouch(uint32_t raw) {
  float onThresh  = baseline + touchDelta;
  float offThresh = baseline + touchDelta * HYSTERESIS;
  bool wasTouched = touched;

  if (!touched && raw > onThresh) touched = true;
  else if (touched && raw < offThresh) touched = false;

  // Let the baseline slowly follow drift while nobody is touching
  if (!touched && raw < offThresh) baseline += ((float)raw - baseline) * 0.01;

  unsigned long now = millis();

  if (touched && !wasTouched) {            // press started
    pressStart = now;
    holding = false;
  }

  if (touched && !holding && lampOn && now - pressStart >= HOLD_MS) {
    holding = true;                        // hold begins: pick direction (opposite of last time)
    rampingUp = !lastRampUp;
  }

  if (!touched && wasTouched) {            // released
    unsigned long dur = now - pressStart;
    if (holding) {
      lastRampUp = rampingUp;
      holding = false;
    } else if (dur >= MIN_TAP_MS && dur < HOLD_MS) {
      lampOn = !lampOn;                    // tap toggles
      dirty = true;
    } else if (!lampOn && dur >= HOLD_MS) {
      lampOn = true;                       // holding while off just turns it on
      dirty = true;
    }
  }
}

void updateRamp() {
  unsigned long now = millis();
  float dt = (now - lastFrame) / 1000.0;
  lastFrame = now;
  if (!holding) return;

  brightness += (rampingUp ? 1 : -1) * BRIGHT_RATE * dt;
  if (brightness > BRIGHT_MAX) brightness = BRIGHT_MAX;
  if (brightness < BRIGHT_MIN) brightness = BRIGHT_MIN;
  dirty = true;
}

void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

  char c = line.charAt(0);
  if (c == 't' || c == 'T') {
    long v = line.substring(1).toInt();
    if (v > 0) touchDelta = v;
    Serial.printf("touchDelta = %lu\n", (unsigned long)touchDelta);
  } else if (c == '+') {
    touchDelta += 100;
    Serial.printf("touchDelta = %lu\n", (unsigned long)touchDelta);
  } else if (c == '-') {
    if (touchDelta > 100) touchDelta -= 100;
    Serial.printf("touchDelta = %lu\n", (unsigned long)touchDelta);
  } else if (c == 'r' || c == 'R') {
    calibrate();
  } else if (c == 'p' || c == 'P') {
    printing = !printing;
  } else if (c == 'e' || c == 'E') {
    String arg = line.substring(1);
    arg.trim();
    setEffect(arg);
  } else if (c == 'c' || c == 'C') {
    int r, g, b;
    if (sscanf(line.c_str() + 1, "%d %d %d", &r, &g, &b) == 3) {
      colorR = constrain(r, 0, 255);
      colorG = constrain(g, 0, 255);
      colorB = constrain(b, 0, 255);
      dirty = true;
      Serial.printf("colour = %d %d %d\n", colorR, colorG, colorB);
    } else {
      Serial.println("usage: c R G B");
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  strip.begin();
  strip.clear();
  strip.show();
  calibrate();
  lastFrame = millis();
  Serial.println("Ready. Commands: t <n>, +, -, r, c R G B, e [name], p");
  listEffects();
}

void loop() {
  handleSerial();

  uint32_t raw = readTouch();
  updateTouch(raw);
  updateRamp();

  bool animated = lampOn && effect != FX_SOLID;
  if (dirty || (animated && millis() - lastRender >= FRAME_MS)) {
    renderFrame();
    lastRender = millis();
  }

  if (printing && millis() - lastPrint >= PRINT_MS) {
    lastPrint = millis();
    Serial.printf("raw=%lu base=%.0f thresh=%.0f delta=%lu %s lamp=%s bright=%d\n",
                  (unsigned long)raw, baseline, baseline + touchDelta, (unsigned long)touchDelta,
                  touched ? "TOUCH" : "-----", lampOn ? "ON " : "OFF", (int)brightness);
  }
  delay(5);
}
