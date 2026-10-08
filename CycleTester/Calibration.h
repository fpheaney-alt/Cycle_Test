// Calibration.h - the start and end pulse width of every servo, and how they are saved in EEPROM.
//
// Values set on the CALIBRATE screen live in the Mega's EEPROM, so they survive power-off. They are stored
// together with a signature of the servo settings in Config.h: if you edit those settings, the saved values
// no longer apply and are ignored (so an edit to Config.h always takes effect, and values measured for one
// configuration are never applied to another). Two alternating copies are kept, so a power cut in the middle
// of a save leaves the previous good copy intact.
#pragma once
#include <Arduino.h>
#include "Config.h"

struct EndPoints {
  int16_t startUs[NUM_SERVOS];
  int16_t endUs[NUM_SERVOS];
};

namespace Calibration {
  enum LoadResult : uint8_t {
    LOAD_NONE,             // nothing has ever been saved
    LOAD_OK,               // a valid block for this Config.h was loaded
    LOAD_CONFIG_CHANGED,   // a block exists but Config.h's servo settings changed since: ignored
    LOAD_BAD               // a block exists but is damaged or out of range: ignored
  };

  void defaults(EndPoints& out);            // the factory values from Config.h
  bool valid(const EndPoints& e);           // every value inside the pulse limits, every sweep long enough
  LoadResult load(EndPoints& out);          // read from EEPROM
  bool save(const EndPoints& e);            // write to EEPROM, then read back and compare. false = not saved
}
