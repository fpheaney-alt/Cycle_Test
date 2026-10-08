// Minimal stand-in for the Arduino core, so the sketch code can be compiled and run on a PC.
// Time is simulated: it only moves when the simulator (or a mocked delay / display / I2C call) moves it.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef bool boolean;
typedef uint8_t byte;

#define PROGMEM
#define pgm_read_byte(addr) (*(const uint8_t*)(addr))
class __FlashStringHelper;
#define F(str) (reinterpret_cast<const __FlashStringHelper*>(str))

#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define HIGH 1
#define LOW 0

// ---- simulated time ----
extern uint64_t g_simUs;                    // microseconds since the simulated power-up (plus an offset)
void simAdvanceUs(uint64_t us);             // move time forward, running the servo "interrupt" every 20 ms
inline uint32_t millis() { return (uint32_t)(g_simUs / 1000ULL); }
inline uint32_t micros() { return (uint32_t)g_simUs; }
inline void delay(uint32_t ms) { simAdvanceUs((uint64_t)ms * 1000ULL); }
inline void delayMicroseconds(unsigned int us) { simAdvanceUs(us); }

inline void pinMode(uint8_t, uint8_t) {}
inline void digitalWrite(uint8_t, uint8_t) {}
inline void noInterrupts() {}
inline void interrupts() {}

// ---- Serial: prints to the console with a timestamp at the start of each line ----
struct SerialMock {
  bool lineStart = true;
  bool enabled = true;
  void stamp() {
    if (lineStart && enabled) { printf("  [%9.3f s] ", (double)(g_simUs % 100000000000ULL) / 1e6); }
    lineStart = false;
  }
  void begin(unsigned long) {}
  void print(const char* s)  { stamp(); if (enabled) fputs(s, stdout); }
  void print(const __FlashStringHelper* s) { print(reinterpret_cast<const char*>(s)); }
  void print(char c)         { stamp(); if (enabled) putchar(c); }
  void print(int v)          { stamp(); if (enabled) printf("%d", v); }
  void print(unsigned int v) { stamp(); if (enabled) printf("%u", v); }
  void print(long v)         { stamp(); if (enabled) printf("%ld", v); }
  void print(unsigned long v){ stamp(); if (enabled) printf("%lu", v); }
  template <typename T> void println(T v) { print(v); if (enabled) putchar('\n'); lineStart = true; }
  void println()             { if (enabled) putchar('\n'); lineStart = true; }
};
extern SerialMock Serial;
