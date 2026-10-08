// Fake EEPROM for the PC build: 4 KB that starts erased (0xFF), with get/put like the real library.
// put() only rewrites bytes that differ (as the real one does), and every byte actually written is counted,
// so tests can check how much wear a series of actions causes.
#pragma once
#include "Arduino.h"

struct EEPROMClass {
  uint8_t mem[4096];
  unsigned long byteWrites = 0;
  long powerCutAfterBytes = -1;     // >= 0: only this many more bytes get written, as if power failed part way through
  EEPROMClass() { memset(mem, 0xFF, sizeof(mem)); }
  template <typename T> T& get(int address, T& value) {
    memcpy(&value, mem + address, sizeof(T));
    return value;
  }
  template <typename T> const T& put(int address, const T& value) {
    const uint8_t* p = (const uint8_t*)&value;
    for (size_t i = 0; i < sizeof(T); i++) {
      if (mem[address + i] != p[i]) {
        if (powerCutAfterBytes == 0) continue;           // power is gone: nothing more gets written
        if (powerCutAfterBytes > 0) powerCutAfterBytes--;
        mem[address + i] = p[i];
        byteWrites++;
      }
    }
    return value;
  }
};
extern EEPROMClass EEPROM;
