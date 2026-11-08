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
//   Double tap                -> next effect (candle <-> chase)
//   Hold (HOLD_MS)            -> brightness ramps while held; direction alternates each hold
//
// SERIAL MONITOR (115200 baud) - readings stream every PRINT_MS
//   t <n>      set touch sensitivity (delta above baseline), e.g. "t 1500"  (smaller = more sensitive)
//   +  /  -    nudge sensitivity by 100
//   r          re-calibrate baseline (don't touch the tape)
//   c R G B    set lamp colour live, e.g. "c 255 140 40"
//   e          switch effect;  "e candle" or "e chase" picks one
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
const unsigned long DOUBLE_TAP_MS = 350; // max gap between two taps for a double tap (0 = disable
                                         //   double tap; a single tap then acts instantly instead
                                         //   of after this delay)

// ----------------------- TOUCH SENSITIVITY --------------------
// On the ESP32-S3 the raw reading goes UP when touched. A touch is detected when
// raw > baseline + touchDelta. Watch the Serial Monitor: touch the tape and see how far
// "raw" rises above "base", then set the delta to roughly half of that rise.
uint32_t touchDelta = 2000;
const float HYSTERESIS = 0.6;           // release when raw falls below baseline + delta * this
const int   SAMPLES    = 4;             // readings averaged per loop
const unsigned long PRINT_MS = 100;

// ----------------------- EFFECTS --------------------
// Both effects use the colour above (and "c R G B"). For a candle try something orange,
// e.g. "c 255 90 10".
enum Effect { FX_CANDLE, FX_CHASE, FX_COUNT };
const char* FX_NAMES[FX_COUNT] = { "candle", "chase" };
Effect effect = FX_CANDLE;                      // effect at power-up

const unsigned long FRAME_MS = 25;              // animation frame time (40 fps)

// Candle
const float CANDLE_MIN         = 0.55;          // dimmest normal flicker (1.0 = full)
const float CANDLE_GUST_CHANCE = 0.06;          // chance per flicker step of a deep dip
const float CANDLE_GUST_LEVEL  = 0.30;          // how low a dip goes
const unsigned long CANDLE_STEP_MIN_MS = 200;   // time between new brightness targets
const unsigned long CANDLE_STEP_MAX_MS = 700;   //   (longer = slower flicker)
const float CANDLE_SMOOTH      = 0.08;          // 0..1, how fast the level chases its target (lower = slower)
const float CANDLE_PIXEL_SPREAD = 0.10;         // how much each LED varies from the others
const float CANDLE_PIXEL_FOLLOW = 0.15;         // 0..1, how fast each LED follows (lower = slower shimmer)

// Chase - every LED stays lit; the "chase" is a bright spot moving around a dimmer glow
const unsigned long CHASE_MS    = 4000;         // ms for one full rotation (higher = slower)
const bool          CHASE_CLOCKWISE = true;     // false = other direction
const float         CHASE_MIN   = 0.25;         // dimmest LED, 0..1 (keep above 0 so none go dark)
const float         CHASE_MAX   = 1.0;          // brightest LED (the head), 0..1
const float         CHASE_TAIL  = 10.0;         // LEDs over which it fades from MAX to MIN
                                                //   (16 = whole ring is a gradient, small = tight comet)

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// ----------------------- STATE --------------------
float baseline = 0;
bool  touched = false;
bool  lampOn = false;
bool  lastRampUp = false;       // first hold goes brighter
bool  rampingUp = true;
bool  holding = false;          // a hold is in progress (ramping)
bool  pendingTap = false;       // a tap happened; waiting to see if it's a double tap
bool  secondPress = false;      // the current press began inside the double-tap window
unsigned long tapReleased = 0;
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

float candleLevel = 0.8, candleTarget = 0.8;
float pixLevel[NUM_LEDS];
unsigned long candleNext = 0;

void fxCandle(unsigned long now) {
  if (now >= candleNext) {
    // Pick a new brightness target now and then; occasionally a deep dip, like a draught
    if (random(1000) / 1000.0 < CANDLE_GUST_CHANCE) candleTarget = CANDLE_GUST_LEVEL;
    else candleTarget = CANDLE_MIN + (1.0 - CANDLE_MIN) * (random(1000) / 1000.0);
    candleNext = now + random(CANDLE_STEP_MIN_MS, CANDLE_STEP_MAX_MS);
  }
  candleLevel += (candleTarget - candleLevel) * CANDLE_SMOOTH;

  for (int i = 0; i < NUM_LEDS; i++) {
    float want = candleLevel + (random(-1000, 1001) / 1000.0) * CANDLE_PIXEL_SPREAD;
    pixLevel[i] += (want - pixLevel[i]) * CANDLE_PIXEL_FOLLOW;
    float f = constrain(pixLevel[i], 0.0f, 1.0f);
    // Dimmer flame shifts redder: green and blue fall faster than red
    strip.setPixelColor(i, strip.Color((uint8_t)(colorR * f), (uint8_t)(colorG * f * f), (uint8_t)(colorB * f * f)));
  }
}

void fxChase(unsigned long now) {
  float head = (now % CHASE_MS) * (float)NUM_LEDS / CHASE_MS;
  if (!CHASE_CLOCKWISE) head = NUM_LEDS - head;
  for (int i = 0; i < NUM_LEDS; i++) {
    float behind = CHASE_CLOCKWISE ? head - i : i - head;
    float d = fmod(behind + NUM_LEDS, (float)NUM_LEDS);       // distance behind the head
    float f = d < CHASE_TAIL ? 1.0 - d / CHASE_TAIL : 0.0;
    strip.setPixelColor(i, colorScaled(CHASE_MIN + (CHASE_MAX - CHASE_MIN) * f));
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
    case FX_CANDLE: fxCandle(now); break;
    case FX_CHASE:  fxChase(now);  break;
    default:        fxCandle(now); break;
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
  unsigned long now = millis();

  // A single tap waits DOUBLE_TAP_MS in case a second tap follows; if none does, it counts now
  if (pendingTap && !touched && now - tapReleased >= DOUBLE_TAP_MS) {
    pendingTap = false;
    lampOn = !lampOn;
    dirty = true;
  }

  bool wasTouched = touched;

  if (!touched && raw > onThresh) touched = true;
  else if (touched && raw < offThresh) touched = false;

  // Let the baseline slowly follow drift while nobody is touching
  if (!touched && raw < offThresh) baseline += ((float)raw - baseline) * 0.01;

  if (touched && !wasTouched) {            // press started
    pressStart = now;
    holding = false;
    secondPress = pendingTap;              // follows a tap within the double-tap window
  }

  if (touched && !holding && lampOn && now - pressStart >= HOLD_MS) {
    holding = true;                        // hold begins: pick direction (opposite of last time)
    rampingUp = !lastRampUp;
    pendingTap = false;                    // a tap followed by a hold is just a hold
  }

  if (!touched && wasTouched) {            // released
    unsigned long dur = now - pressStart;
    if (holding) {
      lastRampUp = rampingUp;
      holding = false;
    } else if (dur >= MIN_TAP_MS && dur < HOLD_MS) {
      if (DOUBLE_TAP_MS == 0) {            // double-tap off: tap acts immediately
        lampOn = !lampOn;
        dirty = true;
      } else if (secondPress) {            // double tap: next effect (and light up if off)
        pendingTap = false;
        setEffect("");
        lampOn = true;
        dirty = true;
      } else {                             // first tap: wait to see if a second follows
        pendingTap = true;
        tapReleased = now;
      }
    } else if (!lampOn && dur >= HOLD_MS) {
      lampOn = true;                       // holding while off just turns it on
      pendingTap = false;
      dirty = true;
    }
    secondPress = false;
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

  bool animated = lampOn;
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
