// Minimal stand-in for the Arduino core, so the sketch code can be compiled and run on a PC.
// Time is simulated: it only moves when the simulator (or a mocked delay / display / I2C call) moves it.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>

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
  // everything printed is also kept, so tests can look at what a Serial Monitor would show
  std::string captured;
  // text "typed" into the Serial Monitor by a test
  std::string input;
  size_t inputPos = 0;
  void typeText(const char* text) { input += text; }
  int available() { return (int)(input.size() - inputPos); }
  int read() { return inputPos < input.size() ? (unsigned char)input[inputPos++] : -1; }

  void out(const char* s)    { captured += s; stamp(); if (enabled) fputs(s, stdout); }
  void begin(unsigned long) {}
  void print(const char* s)  { out(s); }
  void print(const __FlashStringHelper* s) { print(reinterpret_cast<const char*>(s)); }
  void print(char c)         { char b[2] = { c, 0 }; out(b); }
  void print(int v)          { char b[24]; snprintf(b, sizeof(b), "%d", v); out(b); }
  void print(unsigned int v) { char b[24]; snprintf(b, sizeof(b), "%u", v); out(b); }
  void print(long v)         { char b[24]; snprintf(b, sizeof(b), "%ld", v); out(b); }
  void print(unsigned long v){ char b[24]; snprintf(b, sizeof(b), "%lu", v); out(b); }
  void print(double v, int digits = 2) { char b[40]; snprintf(b, sizeof(b), "%.*f", digits, v); out(b); }
  template <typename T> void println(T v) { print(v); out("\n"); lineStart = true; }
  void println()             { out("\n"); lineStart = true; }
};
extern SerialMock Serial;
