// Config.h - every setting you are likely to change lives in this file.
//
// Board: Arduino Mega 2560.
#pragma once
#include <Arduino.h>

// ===========================================================================
// ABSOLUTE PULSE LIMITS  (no pulse outside this range is ever sent)
// ===========================================================================
// These two macros are also handed to the Servo library by ServoEasing. ServoEasing's own default is
// 400..3500 us, but the Servo library keeps that range in signed 8-bit numbers that overflow, so with
// the default it silently cuts every pulse longer than 2476 us. 400..2600 is safe.
// Config.h must be included BEFORE ServoEasing.h (CycleChannel.h does this). Do not raise MAXIMUM above 2900.
#define MINIMUM_PULSE_WIDTH   400
#define MAXIMUM_PULSE_WIDTH  2600

// ===========================================================================
// PINS
// ===========================================================================

// ----------------------- SERVO PINS --------------------
const int SERVO1_PIN = 5;
const int SERVO2_PIN = 6;
const int SERVO3_PIN = 3;
const int SERVO4_PIN = 4;

const uint8_t NUM_SERVOS = 4;
const uint8_t SERVO_PINS[NUM_SERVOS] = { SERVO1_PIN, SERVO2_PIN, SERVO3_PIN, SERVO4_PIN };

// ----------------------- SCREEN PINS --------------------
// IF YOU GET THE ERROR MESSAGE THAT THE TOUCH CONTROLLER IS NOT FOUND, MAKE SURE THAT THE CTP_INT WIRE IS FULLY INSERTED INTO THE ARDUINO.
//VCC - 5V
//GND - GND
//LCD_CS - 8
//LCD_RST - 7
//LCD_RS - 9
//SDI(MOSI) - 51
//SCK - 52
//LED - 5V
//SDO(MOSI) - 50
//CTP_SCL - SCL
//CTP_RST - 3.3v
//CTP_SDA - SDA
//CTP_INT - 2
//
// CTP_RST is wired to 3.3V, so it is NOT an Arduino pin. (The older Snap & Test sketch defined
// "CTP_RST 6", which is the same pin as SERVO2_PIN. That is deliberately not repeated here.)
const int8_t LCD_CS_PIN  = 8;
const int8_t LCD_DC_PIN  = 9;    // labelled LCD_RS on the module
const int8_t LCD_RST_PIN = 7;
const int8_t LCD_LED_PIN = -1;   // backlight (LED) is wired straight to 5V, so no pin to control
const uint8_t TOUCH_INT_PIN = 2;

// ===========================================================================
// MOTION  (applies to all four servos)
// ===========================================================================

// THE universal speed knob: how fast every servo moves, in degrees per second.
//
// The time for one 180 degree sweep is 180 / speed:   45 -> 4.0 s,   90 -> 2.0 s,   180 -> 1.0 s.
// One full cycle (out and back) is two sweeps plus the two dwells below.
//
// With sine easing the instantaneous peak is about 1.57x this value. The servo itself tops out
// at roughly 430 deg/s with no load at 7.4 V, so stay well below that. Allowed range: 5 .. 400.
const uint16_t SERVO_SPEED_DEG_PER_SEC = 90;

// How a sweep accelerates. Gentler ramps are kinder to the servo gears and the product being tested,
// and keep the peak current down. EASING_LINEAR gives constant speed with abrupt starts and stops.
enum EasingStyle : uint8_t { EASING_LINEAR, EASING_QUADRATIC, EASING_SINE, EASING_CUBIC };
const EasingStyle SERVO_EASING = EASING_SINE;

// How long each servo rests at either end of its sweep before reversing (milliseconds).
const uint16_t DWELL_AT_END_MS   = 250;   // after reaching 180 degrees
const uint16_t DWELL_AT_START_MS = 250;   // after returning to the starting position

// Servos are started one after another, this far apart, instead of all in the same instant.
// Four 35 kg servos moving off together is the biggest current spike the power supply will see.
// Set to 0 to start them all together.
const uint16_t START_STAGGER_MS = 250;

// When a servo finishes its target number of cycles, stop sending it a signal (it goes limp at the
// start position, stays cool and silent). Set to false to keep holding the start position.
const bool DETACH_WHEN_DONE = true;

// After the last cycle, wait this long for the servo to settle before releasing it.
const uint16_t DONE_SETTLE_MS = 500;

// When a servo first receives a signal it may have to swing to the start position from wherever
// it was left; it waits this long there before the first sweep.
const uint16_t ATTACH_SETTLE_MS = 600;

// ===========================================================================
// SERVO CALIBRATION  (factory defaults - fine tuning is done on the CALIBRATE screen)
// ===========================================================================
// These servos take 500..2500 us pulses (1500 us = centre). Yours turn out to be the 270 degree
// version, so a 180 degree sweep uses only the middle two thirds of the travel.
// Everything in this section is the STARTING POINT. On the touchscreen, CALIBRATE lets you tune each
// servo's start and end to a fraction of a degree and SAVE it; saved values override these defaults
// until you change something in this section of Config.h (then the saved values are discarded).

const int SERVO_PULSE_MIN_US = 500;        // pulse at one end of the servo's full travel
const int SERVO_PULSE_MAX_US = 2500;       // pulse at the other end of its full travel

// How far the arm turns between SERVO_PULSE_MIN_US and SERVO_PULSE_MAX_US:  180  or  270.
const int SERVO_FULL_TRAVEL_DEG = 270;

// Where the 180 degree sweep begins, measured from the MIN end of the full travel.
//   180 degree servo:  0   (sweep uses the whole travel)
//   270 degree servo:  45  (sweep is centred, 45 degrees of spare travel on each side)
const int SWEEP_START_OFFSET_DEG = 45;

// The sweep is always 180 degrees, out and back.
const int SWEEP_DEG = 180;

// Derived pulse widths for the two ends of the sweep. Do not edit these two lines.
constexpr int pulseAtDeg(int deg) {   // rounds to the nearest microsecond
  return SERVO_PULSE_MIN_US + (int)(((long)(SERVO_PULSE_MAX_US - SERVO_PULSE_MIN_US) * deg + SERVO_FULL_TRAVEL_DEG / 2) / SERVO_FULL_TRAVEL_DEG);
}
constexpr int DEFAULT_START_US = pulseAtDeg(SWEEP_START_OFFSET_DEG);
constexpr int DEFAULT_END_US   = pulseAtDeg(SWEEP_START_OFFSET_DEG + SWEEP_DEG);

// Per-servo factory ends of the sweep, in microseconds. "Start" is where a cycle begins and ends.
// All four default to the values above. Swapping START and END reverses that servo's direction.
constexpr int SERVO_START_US[NUM_SERVOS] = { DEFAULT_START_US, DEFAULT_START_US, DEFAULT_START_US, DEFAULT_START_US };
constexpr int SERVO_END_US[NUM_SERVOS]   = { DEFAULT_END_US,   DEFAULT_END_US,   DEFAULT_END_US,   DEFAULT_END_US   };

// ---- Calibration screen -------------------------------------------------
// Use the calibration values saved on the touchscreen. Set to false to always use the numbers above.
const bool USE_SAVED_CALIBRATION = true;

// Jog buttons on the calibration screen, in microseconds: small, medium, large. With a 270 degree servo
// 1 us is about 0.13 degrees, so the smallest button moves the arm by about an eighth of a degree.
// (The servo's own dead band, about 2-3 us, is the limit of how finely the arm can really be placed.)
const uint8_t CAL_STEP_US[3] = { 1, 5, 25 };

// The arm may never be set closer than this between its start and end (keeps a sweep from collapsing).
const uint16_t CAL_MIN_SWEEP_US = 100;

// How fast the arm glides when you press GO START / GO END, in microseconds per second
// (600 us per second is roughly 80 degrees per second on a 270 degree servo).
const uint16_t CAL_SLEW_US_PER_SEC = 600;

// Compile-time sanity checks. If one of these fires, the message says what to fix.
constexpr bool pulseInLimits(int us) { return us >= SERVO_PULSE_MIN_US && us <= SERVO_PULSE_MAX_US; }
static_assert(SERVO_SPEED_DEG_PER_SEC >= 5 && SERVO_SPEED_DEG_PER_SEC <= 400,
              "SERVO_SPEED_DEG_PER_SEC must be between 5 and 400");
static_assert(SERVO_FULL_TRAVEL_DEG == 180 || SERVO_FULL_TRAVEL_DEG == 270,
              "SERVO_FULL_TRAVEL_DEG must be 180 or 270");
static_assert(SWEEP_START_OFFSET_DEG >= 0 && SWEEP_START_OFFSET_DEG + SWEEP_DEG <= SERVO_FULL_TRAVEL_DEG,
              "The 180 degree sweep does not fit inside the servo's travel: lower SWEEP_START_OFFSET_DEG");
static_assert(SERVO_PULSE_MIN_US >= MINIMUM_PULSE_WIDTH && SERVO_PULSE_MAX_US <= MAXIMUM_PULSE_WIDTH && SERVO_PULSE_MIN_US < SERVO_PULSE_MAX_US,
              "Servo pulse widths must be inside 400..2600 us and MIN must be below MAX");
static_assert(pulseInLimits(SERVO_START_US[0]) && pulseInLimits(SERVO_START_US[1]) && pulseInLimits(SERVO_START_US[2]) && pulseInLimits(SERVO_START_US[3]) &&
              pulseInLimits(SERVO_END_US[0])   && pulseInLimits(SERVO_END_US[1])   && pulseInLimits(SERVO_END_US[2])   && pulseInLimits(SERVO_END_US[3]),
              "Every SERVO_START_US / SERVO_END_US value must lie between SERVO_PULSE_MIN_US and SERVO_PULSE_MAX_US");

// ===========================================================================
// TEST SETUP SCREEN
// ===========================================================================
const uint32_t DEFAULT_TARGET_CYCLES = 1000;      // what each servo starts at on the setup screen (0 = off)
const uint32_t MAX_TARGET_CYCLES     = 999900;    // the display has room for 6 digits
const uint32_t STEP_SMALL            = 100;       // the two step sizes on the buttons
const uint32_t STEP_LARGE            = 1000;

// ===========================================================================
// TOUCH
// ===========================================================================
const uint16_t TOUCH_POLL_MS       = 25;    // how often the touch controller is read (I2C). Do not go much lower:
                                            // frequent I2C traffic delays the servo timer interrupt and shows up as jitter.
const uint8_t  TOUCH_THRESHOLD     = 40;    // controller sensitivity (same value the old sketch used)
const uint8_t  TOUCH_SLOP_PX       = 2;     // buttons also react this many pixels outside their edges
const uint16_t REPEAT_DELAY_MS     = 450;   // hold a +/- button this long before it starts repeating
const uint16_t REPEAT_INTERVAL_MS  = 130;   // time between repeats while held
const uint16_t REPEAT_FAST_AFTER   = 20;    // after this many repeats...
const uint16_t REPEAT_FAST_INTERVAL_MS = 60; // ...repeat faster still
const uint16_t CONFIRM_WINDOW_MS   = 3000;  // time to tap the SETUP button a second time to confirm

// ===========================================================================
// DIAGNOSTICS
// ===========================================================================
const bool          SERIAL_LOG  = true;     // print events (start, pause, done...) to the Serial Monitor
const unsigned long SERIAL_BAUD = 115200;
const bool          TOUCH_DEBUG = false;    // true: print every recognised touch (raw and screen position) to Serial
