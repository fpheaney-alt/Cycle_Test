// CycleChannel.h - the cycling logic for one servo, and a controller that owns all four.
//
// Each servo runs completely independently:  start position -> 180 degrees -> start position
// is one cycle, and the cycle count goes up each time the servo gets back to the start.
// Motion is done by the ServoEasing library, which updates the servo from a timer interrupt, so
// the arm keeps moving smoothly even while the main loop is busy redrawing the screen.
#pragma once
#include <Arduino.h>
#define SUPPRESS_HPP_WARNING   // the library's code (ServoEasing.hpp) is included from CycleChannel.cpp only
#include <ServoEasing.h>
#include "Config.h"

enum ChannelStatus : uint8_t {
  CH_OFF,       // target is 0: this servo is not part of the test
  CH_RUNNING,   // cycling (or waiting its turn to start)
  CH_PAUSED,    // stopped in place by the user, still holding its position
  CH_DONE,      // reached its target number of cycles
  CH_FAULT      // the servo could not be attached; nothing is counted for it
};

class CycleChannel {
 public:
  void init(uint8_t index);                                         // call once, from setup()

  void begin(uint32_t target, uint32_t now, uint32_t startDelayMs); // start a new test (target 0 = off)
  void abort();                                                     // end the test: go back to the start position, then release
  void pause(uint32_t now);                                         // freeze in place
  void resume(uint32_t now);                                        // carry on from where it stopped
  void update(uint32_t now);                                        // call as often as possible from loop()

  uint32_t      count()  const { return count_; }
  uint32_t      target() const { return target_; }
  ChannelStatus status() const;
  bool          waitingToStart() const { return enabled_ && !paused_ && phase_ == PH_WAIT; }

 private:
  enum Phase : uint8_t {
    PH_IDLE,          // not part of a test
    PH_WAIT,          // waiting for its staggered start time
    PH_TO_END,        // sweeping out to 180 degrees
    PH_HOLD_END,      // resting at 180 degrees
    PH_TO_START,      // sweeping back to the start position
    PH_HOLD_START,    // resting at the start position
    PH_FINISHING,     // last cycle done, letting the servo settle before it is released
    PH_DONE,          // finished
    PH_HOMING         // test was aborted: gently returning to the start position
  };

  bool isMovePhase() const    { return phase_ == PH_TO_END || phase_ == PH_TO_START; }
  bool isPausablePhase() const { return phase_ == PH_WAIT || phase_ == PH_TO_END || phase_ == PH_HOLD_END ||
                                        phase_ == PH_TO_START || phase_ == PH_HOLD_START; }
  bool ensureAttached();
  void release();
  void startMove(int degree);
  void log(const __FlashStringHelper* what) const;

  ServoEasing servo_;
  uint8_t  index_ = 0;
  uint8_t  pin_ = 0;
  int      startUs_ = 1500, endUs_ = 1500;

  Phase    phase_ = PH_IDLE;
  bool     enabled_ = false;      // part of the current test
  bool     paused_ = false;
  bool     attached_ = false;
  bool     fault_ = false;
  uint32_t count_ = 0;
  uint32_t target_ = 0;
  uint32_t deadline_ = 0;         // when a timed phase (wait / hold / finishing) ends
  uint32_t remainingMs_ = 0;      // time left in a timed phase, saved while paused
  int      resumeDegree_ = 0;     // where an interrupted sweep was heading
};

class CycleController {
 public:
  void init();

  void begin(const uint32_t targets[NUM_SERVOS], uint32_t now);  // start a new test
  void abort();                                                   // end the test, return servos to start
  void update(uint32_t now);

  void pauseAll(uint32_t now);
  void resumeAll(uint32_t now);
  void toggle(uint8_t index, uint32_t now);                      // pause or resume one servo

  const CycleChannel& channel(uint8_t index) const { return ch_[index]; }
  bool testActive() const { return active_; }
  uint8_t countWithStatus(ChannelStatus s) const;
  bool allFinished() const;                                       // every servo is DONE or OFF

 private:
  CycleChannel ch_[NUM_SERVOS];
  bool active_ = false;
  uint32_t nextStartMs_ = 0;      // earliest time the next waiting servo may switch on
};
