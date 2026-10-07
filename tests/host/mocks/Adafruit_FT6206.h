// Fake touch controller: touches are scripted by the simulator, in the controller's own raw
// coordinates (x 0..319, y 0..479, i.e. rotated relative to the screen).
#pragma once
#include "Arduino.h"
#include <vector>

struct TS_Point {
  int16_t x, y, z;
  TS_Point() : x(0), y(0), z(0) {}
  TS_Point(int16_t _x, int16_t _y, int16_t _z) : x(_x), y(_y), z(_z) {}
};

struct ScriptedTouch { uint64_t startUs, endUs; int16_t rawX, rawY; };
extern std::vector<ScriptedTouch> g_touchScript;   // finger-down intervals
extern std::vector<uint64_t> g_glitchPolls;        // times at which one read returns a bogus touch
extern int g_failBeginCount;                       // make begin() fail this many times first
extern unsigned long g_i2cReads;

struct Adafruit_FT6206 {
  bool begin(uint8_t = 128, void* = nullptr, uint8_t = 0x38) {
    if (g_failBeginCount > 0) { g_failBeginCount--; return false; }
    return true;
  }
  TS_Point getPoint(uint8_t = 0) {
    g_i2cReads++;
    simAdvanceUs(1800);                            // a 16-byte I2C read at 100 kHz blocks for roughly this long
    for (size_t i = 0; i < g_glitchPolls.size(); i++) {
      if (g_simUs >= g_glitchPolls[i] && g_simUs < g_glitchPolls[i] + 30000) {
        g_glitchPolls.erase(g_glitchPolls.begin() + i);
        return TS_Point(285, 160, 1);              // a one-off phantom touch, on top of the START button
      }
    }
    for (const ScriptedTouch& t : g_touchScript) {
      if (g_simUs >= t.startUs && g_simUs < t.endUs) return TS_Point(t.rawX, t.rawY, 1);
    }
    return TS_Point(0, 0, 0);
  }
};
