// Fake ServoEasing library for the PC build. It copies the behaviour of the real library that the
// sketch depends on (read from ServoEasing 3.6.0's source):
//   * moves are advanced by a 20 ms "interrupt" (see simAdvanceUs), using millis() for timing
//   * a move's duration is |distance| * 1000 / speed, held in 16 bits
//   * startEaseTo() on a detached servo silently does nothing and isMoving() stays false
//   * attach() always takes a NEW slot in the library's table, even if already attached (we count these as "leaks")
//   * THE SERVO LIBRARY CLIPS PULSES: attach(pin, min, max) stores the limits as signed 8-bit multiples of 4 us
//     relative to 544..2400, so ServoEasing's default range of 400..3500 us overflows and every pulse above
//     2476 us is silently cut to 2476 us. The sketch avoids this by defining MAXIMUM_PULSE_WIDTH in Config.h.
// It also records the pulse width the servo would be receiving, so tests can check what the arm did.
#pragma once
#include "Arduino.h"
#include <vector>
#include <math.h>

// Same defaults and guards as ServoEasing.h
#if !defined(MINIMUM_PULSE_WIDTH)
#define MINIMUM_PULSE_WIDTH   400
#endif
#if !defined(MAXIMUM_PULSE_WIDTH)
#define MAXIMUM_PULSE_WIDTH  3500
#endif

#define EASE_LINEAR            0x00
#define EASE_QUADRATIC_IN_OUT  0x81
#define EASE_CUBIC_IN_OUT      0x82
#define EASE_SINE_IN_OUT       0x88
#define START_UPDATE_BY_INTERRUPT true
#define DO_NOT_START_UPDATE_BY_INTERRUPT false

class ServoEasing {
 public:
  ServoEasing() { all().push_back(this); }

  uint8_t attach(int pin, int initialDeg, int us0, int us180) {
    if (attached_) leakedAttaches()++;            // the real library would use up another table slot
    attached_ = true; pin_ = pin; us0_ = us0; us180_ = us180; attachedAtUs_ = g_simUs;
    min8_ = (int8_t)((544 - MINIMUM_PULSE_WIDTH) / 4);     // int8_t overflow here is exactly the real library's behaviour
    max8_ = (int8_t)((2400 - MAXIMUM_PULSE_WIDTH) / 4);
    moves_ = false;
    curDeg_ = initialDeg;
    writeDegrees(initialDeg);
    attachCount_++;
    return pin;
  }
  void detach() { attached_ = false; moves_ = false; detachCount_++; }
  bool attached() const { return attached_; }
  void setEasingType(uint_fast8_t type) { easing_ = type; }

  bool startEaseTo(int targetDeg, uint_fast16_t dps, bool) {
    if (!attached_) return true;                  // real library: "Error: detached servo", does nothing
    if (dps == 0) dps = 1;
    uint16_t ms = (uint16_t)((long)abs(targetDeg - (int)lround(curDeg_)) * 1000L / dps);   // 16 bit, like the library
    startDeg_ = curDeg_; endDeg_ = targetDeg; durMs_ = ms; startMs_ = millis();
    bool wasStill = !moves_;
    moves_ = true;
    moveStarts_++;
    return wasStill;
  }
  bool isMoving() { return moves_; }
  void stop() { moves_ = false; }
  int getCurrentAngle() { return (int)lround(curDeg_); }

  // ---- test-side inspection ----
  double pulseUs() const { return lastUs_; }
  double degrees() const { return curDeg_; }
  int minUs() const { return minUs_; }
  int maxUs() const { return maxUs_; }
  void resetStats() { minUs_ = 100000; maxUs_ = -100000; maxSlewUsPerS_ = 0; moveStarts_ = 0; attachCount_ = 0; detachCount_ = 0; }
  int clampMinUs() const { return 544 - min8_ * 4; }      // the pulse range the Servo library really allows
  int clampMaxUs() const { return 2400 - max8_ * 4; }
  int attachCount() const { return attachCount_; }
  uint64_t attachedAtUs() const { return attachedAtUs_; }
  int detachCount() const { return detachCount_; }
  int moveStarts() const { return moveStarts_; }
  double maxSlewUsPerSec() const { return maxSlewUsPerS_; }

  static std::vector<ServoEasing*>& all() { static std::vector<ServoEasing*> v; return v; }
  static int& leakedAttaches() { static int n = 0; return n; }

  // The 20 ms timer interrupt: advance every moving servo.
  static void isrTick() {
    for (ServoEasing* s : all()) if (s->attached_ && s->moves_) s->update();
  }

 private:
  double ease(double t) const {
    switch (easing_) {
      case EASE_LINEAR: return t;
      case EASE_QUADRATIC_IN_OUT: return t < 0.5 ? 2 * t * t : 1 - 2 * (1 - t) * (1 - t);
      case EASE_CUBIC_IN_OUT: return t < 0.5 ? 4 * t * t * t : 1 - 4 * (1 - t) * (1 - t) * (1 - t);
      default: return 0.5 * (1 - cos(M_PI * t));   // sine in/out
    }
  }
  void writeDegrees(double deg) {
    curDeg_ = deg;
    double us = us0_ + (us180_ - us0_) * deg / 180.0;
    writeMicroseconds((int)lround(us));
  }
 public:
  // Servo::writeMicroseconds(): the pulse the Servo library holds and sends every 20 ms (clamped to its limits)
  void writeMicroseconds(int rounded) {
    int lo = 544 - min8_ * 4, hi = 2400 - max8_ * 4;      // the Servo library's clamp
    if (rounded < lo) rounded = lo;
    else if (rounded > hi) rounded = hi;
    double dt = (g_simUs - lastWriteUs_) / 1e6;
    if (lastWriteUs_ != 0 && dt > 0.0001) {
      double slew = fabs((double)rounded - lastUs_) / dt;
      if (slew > maxSlewUsPerS_ && dt < 0.1) maxSlewUsPerS_ = slew;
    }
    lastWriteUs_ = g_simUs;
    lastUs_ = rounded;
    if (rounded < minUs_) minUs_ = rounded;
    if (rounded > maxUs_) maxUs_ = rounded;
  }
 private:
  void update() {
    uint32_t elapsed = millis() - startMs_;
    if (elapsed >= durMs_) { writeDegrees(endDeg_); moves_ = false; return; }
    double t = (double)elapsed / (double)durMs_;
    writeDegrees(startDeg_ + (endDeg_ - startDeg_) * ease(t));
  }

  bool attached_ = false, moves_ = false;
  int pin_ = 0, us0_ = 1000, us180_ = 2000;
  uint_fast8_t easing_ = EASE_LINEAR;
  double curDeg_ = 0, startDeg_ = 0, endDeg_ = 0, lastUs_ = 0, maxSlewUsPerS_ = 0;
  uint64_t lastWriteUs_ = 0, attachedAtUs_ = 0;
  uint16_t durMs_ = 0;
  uint32_t startMs_ = 0;
  int8_t min8_ = 0, max8_ = 0;
  int minUs_ = 100000, maxUs_ = -100000, attachCount_ = 0, detachCount_ = 0, moveStarts_ = 0;
};
