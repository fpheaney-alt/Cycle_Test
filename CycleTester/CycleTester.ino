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
//   SerialCommands.* type a servo position in the Serial Monitor to try it before putting it in Config.h
//   Font5x7.h       the text font used by the UI
#include "Config.h"
#include "CycleChannel.h"
#include "Ui.h"
#include "SerialCommands.h"

static CycleController controller;

void setup() {
  if (SERIAL_LOG || SERIAL_COMMANDS) {
    Serial.begin(SERIAL_BAUD);
    Serial.println(F("Cycle Tester starting"));
  }

  // The servos stay unpowered (no signal, limp) until you press START, so nothing moves at power-up.
  controller.init();
  Ui::begin(controller);
  if (SERIAL_COMMANDS) SerialCommands::begin();
}

void loop() {
  uint32_t now = millis();
  controller.update(now);   // advance each servo's cycle (quick)
  if (SERIAL_COMMANDS) SerialCommands::update(controller, now);   // typed position commands (quick)
  Ui::update(now);          // touch and screen (can take a few tens of milliseconds when it redraws)
}
