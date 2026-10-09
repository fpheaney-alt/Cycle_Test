// SerialCommands.h - type a servo position in the Serial Monitor to try it before putting it in Config.h.
//
// Serial Monitor: 115200 baud, line ending "Newline". Only works between tests. Type  help  for the list.
#pragma once
#include <Arduino.h>
#include "CycleChannel.h"

namespace SerialCommands {
  void begin();                                         // call once from setup(), after Serial.begin()
  void update(CycleController& controller, uint32_t now);   // call from loop(): reads typed characters, runs finished lines
}
