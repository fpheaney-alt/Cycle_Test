#pragma once
#include "Arduino.h"
#define WIRE_HAS_TIMEOUT
struct TwoWire {
  void begin() {}
  void setWireTimeout(uint32_t, bool) {}
};
extern TwoWire Wire;
