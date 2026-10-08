// CycleTester - four-servo cycle testing robot with a touchscreen front end.
//
// Board:      Arduino Mega 2560
// Libraries:  ServoEasing (and its Servo dependency), LCDWIKI_SPI + LCDWIKI_gui (display),
//             Adafruit FT6206 + Adafruit BusIO (touch). See README.md.
//
// Every setting you might want to change (speed, pins, servo travel, dwell times) is in Config.h.
//
//   CycleChannel.*  the cycling logic: one state machine per servo, plus a controller for all four
//   Ui.*            the touchscreen: setup screen, run screen, touch handling
//   Font5x7.h       the text font used by the UI
#include "Config.h"
#include "CycleChannel.h"
#include "Ui.h"

static CycleController controller;

void setup() {
  if (SERIAL_LOG) {
    Serial.begin(SERIAL_BAUD);
    Serial.println(F("Cycle Tester starting"));
  }

  // The servos get no signal (they stay limp) until you press START, or first jog / GO on the CALIBRATE
  // screen, so nothing moves at power-up.
  controller.init();
  Ui::begin(controller);
}

void loop() {
  uint32_t now = millis();
  controller.update(now);   // advance each servo's cycle (quick)
  Ui::update(now);          // touch and screen (can take a few tens of milliseconds when it redraws)
}
