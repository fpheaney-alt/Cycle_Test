// Capacitive-touch lamp: Seeed XIAO ESP32S3 + WS2812 16-LED ring + copper tape button
//
// WIRING
//   Copper tape wire  -> D1  (GPIO2)   touch-capable pin, no resistor needed
//   WS2812 DIN        -> D10 (GPIO9)   through a ~330 ohm resistor
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
//   p          pause / resume the readings stream

#include <Adafruit_NeoPixel.h>

// ----------------------- PINS --------------------
const int TOUCH_PIN = 2;   // GPIO2 = D1
const int LED_PIN   = 9;   // GPIO9 = D10
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

void applyLeds() {
  uint8_t b = lampOn ? (uint8_t)brightness : 0;
  strip.setBrightness(b);
  for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, strip.Color(colorR, colorG, colorB));
  strip.show();
  dirty = false;
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
  Serial.println("Ready. Commands: t <n>, +, -, r, c R G B, p");
}

void loop() {
  handleSerial();

  uint32_t raw = readTouch();
  updateTouch(raw);
  updateRamp();

  if (dirty) applyLeds();

  if (printing && millis() - lastPrint >= PRINT_MS) {
    lastPrint = millis();
    Serial.printf("raw=%lu base=%.0f thresh=%.0f delta=%lu %s lamp=%s bright=%d\n",
                  (unsigned long)raw, baseline, baseline + touchDelta, (unsigned long)touchDelta,
                  touched ? "TOUCH" : "-----", lampOn ? "ON " : "OFF", (int)brightness);
  }
  delay(5);
}
