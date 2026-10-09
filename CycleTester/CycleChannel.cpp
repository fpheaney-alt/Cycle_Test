#include "CycleChannel.h"
#include <ServoEasing.hpp>   // contains the library's code, so it is included from exactly one .cpp file

static uint_fast8_t easingCode(EasingStyle style) {
  switch (style) {
    case EASING_LINEAR:    return EASE_LINEAR;
    case EASING_QUADRATIC: return EASE_QUADRATIC_IN_OUT;
    case EASING_CUBIC:     return EASE_CUBIC_IN_OUT;
    default:               return EASE_SINE_IN_OUT;
  }
}

// How fast the arm glides when it is moved from the Serial Monitor: the universal speed, as pulse width per second.
static const float MANUAL_US_PER_SEC = (float)SERVO_SPEED_DEG_PER_SEC * (float)(SERVO_PULSE_MAX_US - SERVO_PULSE_MIN_US) / (float)SERVO_FULL_TRAVEL_DEG;

// True once `now` has reached `deadline`, correct even when millis() wraps around.
static bool reached(uint32_t now, uint32_t deadline) {
  return (int32_t)(now - deadline) >= 0;
}

// ---------------------------------------------------------------------------
// CycleChannel
// ---------------------------------------------------------------------------

void CycleChannel::init(uint8_t index) {
  index_   = index;
  pin_     = SERVO_PINS[index];
  startUs_ = SERVO_START_US[index];
  endUs_   = SERVO_END_US[index];
  servo_.setEasingType(easingCode(SERVO_EASING));
  // The servo is NOT attached yet, so it receives no signal and stays limp until a test starts.
}

void CycleChannel::log(const __FlashStringHelper* what) const {
  if (!SERIAL_LOG) return;
  Serial.print(F("S"));
  Serial.print(index_ + 1);
  Serial.print(' ');
  Serial.println(what);
}

// ServoEasing::attach() hands out a new slot in the library's servo table every time it is
// called, even for a servo that is already attached. So attach only when we are not attached.
// attach() with an initial angle also writes that angle, so the arm goes straight to the
// start position instead of first flicking to 90 degrees.
bool CycleChannel::ensureAttached() {
  if (!attached_) {
    servo_.attach(pin_, 0, startUs_, endUs_);   // logical 0 deg = start pulse, logical 180 deg = end pulse
    attached_ = servo_.attached();
  }
  return attached_;
}

void CycleChannel::release() {
  if (attached_) {
    servo_.stop();
    servo_.detach();
    digitalWrite(pin_, LOW);        // Servo::detach() can leave the line HIGH if it lands in the middle of a pulse
    attached_ = false;
  }
}

void CycleChannel::startMove(int degree) {
  servo_.startEaseTo(degree, SERVO_SPEED_DEG_PER_SEC, START_UPDATE_BY_INTERRUPT);
}

void CycleChannel::begin(uint32_t target, uint32_t now, uint32_t startDelayMs) {
  if (phase_ == PH_MANUAL) { release(); phase_ = PH_IDLE; }   // a typed position drove the pulse directly: attach afresh
  count_  = 0;
  target_ = target;
  paused_ = false;
  fault_  = false;
  enabled_ = (target > 0);
  if (enabled_) {
    phase_ = PH_WAIT;
    deadline_ = now + startDelayMs;
  } else if (phase_ != PH_HOMING) {   // a servo still homing from the previous test finishes doing so
    phase_ = PH_IDLE;
  }
}

void CycleChannel::abort() {
  paused_  = false;
  enabled_ = false;
  if (phase_ == PH_MANUAL) {          // nothing to finish: a typed position drove the pulse directly
    release();
    phase_ = PH_IDLE;
    return;
  }
  if (attached_) {
    phase_ = PH_HOMING;
    startMove(0);                     // from wherever it is, back to the start position
  } else {
    phase_ = PH_IDLE;
  }
}

void CycleChannel::pause(uint32_t now) {
  if (paused_ || !enabled_ || !isPausablePhase()) return;
  if (isMovePhase()) {
    resumeDegree_ = (phase_ == PH_TO_END) ? SWEEP_DEG : 0;
    servo_.stop();                    // freeze at the current position; the servo keeps holding it
  } else {
    int32_t left = (int32_t)(deadline_ - now);
    remainingMs_ = (left > 0) ? (uint32_t)left : 0;
  }
  paused_ = true;
  log(F("paused"));
}

void CycleChannel::resume(uint32_t now) {
  if (!paused_) return;
  paused_ = false;
  if (isMovePhase()) {
    startMove(resumeDegree_);         // finish the sweep from the current position, at the same speed
  } else {
    deadline_ = now + remainingMs_;
  }
  log(F("resumed"));
}

ChannelStatus CycleChannel::status() const {
  if (!enabled_)                                 return CH_OFF;
  if (fault_)                                    return CH_FAULT;
  if (phase_ == PH_FINISHING || phase_ == PH_DONE) return CH_DONE;
  if (paused_)                                   return CH_PAUSED;
  return CH_RUNNING;
}

void CycleChannel::update(uint32_t now) {
  if (paused_) return;

  switch (phase_) {
    case PH_WAIT:
      if (reached(now, deadline_)) {
        if (!ensureAttached()) {      // never count cycles for a servo that is not really being driven
          fault_ = true;
          phase_ = PH_IDLE;
          log(F("FAULT: could not attach"));
          break;
        }
        phase_ = PH_HOLD_START;       // give it time to reach the start position, then sweep
        deadline_ = now + ATTACH_SETTLE_MS;
        log(F("started"));
      }
      break;

    case PH_HOLD_START:
      if (reached(now, deadline_)) {
        startMove(SWEEP_DEG);
        phase_ = PH_TO_END;
      }
      break;

    case PH_TO_END:
      if (!servo_.isMoving()) {
        phase_ = PH_HOLD_END;
        deadline_ = now + DWELL_AT_END_MS;
      }
      break;

    case PH_HOLD_END:
      if (reached(now, deadline_)) {
        startMove(0);
        phase_ = PH_TO_START;
      }
      break;

    case PH_TO_START:
      if (!servo_.isMoving()) {
        count_++;
        if (SERIAL_LOG && (count_ % 100 == 0 || count_ >= target_)) {
          Serial.print(F("S"));
          Serial.print(index_ + 1);
          Serial.print(F(" cycles "));
          Serial.print(count_);
          Serial.print('/');
          Serial.println(target_);
        }
        if (count_ >= target_) {
          phase_ = PH_FINISHING;
          deadline_ = now + DONE_SETTLE_MS;
        } else {
          phase_ = PH_HOLD_START;
          deadline_ = now + DWELL_AT_START_MS;
        }
      }
      break;

    case PH_FINISHING:
      if (reached(now, deadline_)) {
        if (DETACH_WHEN_DONE) release();
        phase_ = PH_DONE;
        log(F("done"));
      }
      break;

    case PH_HOMING:
      if (!servo_.isMoving()) {
        release();
        phase_ = PH_IDLE;
      }
      break;

    case PH_MANUAL: {
      int32_t dt = (int32_t)(now - manualLastMs_);
      manualLastMs_ = now;
      if (dt > 250) dt = 250;                       // after a long pause (a screen redraw) do not lurch
      float target = (float)manualTargetUs_;
      if (manualUs_ != target) {
        float step = MANUAL_US_PER_SEC * (float)dt / 1000.0f;
        if (manualUs_ < target) { manualUs_ += step; if (manualUs_ > target) manualUs_ = target; }
        else                    { manualUs_ -= step; if (manualUs_ < target) manualUs_ = target; }
        servo_.writeMicroseconds((int)(manualUs_ + 0.5f));
      }
      break;
    }

    default:
      break;
  }
}

// Bench positioning. The pulse is written directly (Servo::writeMicroseconds), so any end points can be tried
// without re-attaching the servo. A test that starts afterwards detaches and attaches afresh with these end points.
bool CycleChannel::manualMove(int startUs, int endUs, bool toEnd, uint32_t now) {
  if (manualBusy()) return false;
  startUs_ = startUs;
  endUs_   = endUs;
  int target = toEnd ? endUs : startUs;
  if (phase_ == PH_IDLE) {
    if (!ensureAttached()) return false;
    servo_.writeMicroseconds(target);               // attach() has just sent the start pulse; replace it at once
    phase_ = PH_MANUAL;
    manualUs_ = (float)target;
    manualLastMs_ = now;
  }
  manualTargetUs_ = target;
  return true;
}

void CycleChannel::manualOff() {
  if (phase_ != PH_MANUAL) return;
  release();
  phase_ = PH_IDLE;
}

// ---------------------------------------------------------------------------
// CycleController
// ---------------------------------------------------------------------------

void CycleController::init() {
  for (uint8_t i = 0; i < NUM_SERVOS; i++) ch_[i].init(i);
}

void CycleController::begin(const uint32_t targets[NUM_SERVOS], uint32_t now) {
  uint8_t started = 0;   // servos are staggered in the order they start, skipping any that are off
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    uint32_t delayMs = (targets[i] > 0) ? (uint32_t)started * START_STAGGER_MS : 0;
    ch_[i].begin(targets[i], now, delayMs);
    if (targets[i] > 0) started++;
  }
  active_ = true;
  nextStartMs_ = now;
  if (SERIAL_LOG) Serial.println(F("Test started"));
}

void CycleController::abort() {
  for (uint8_t i = 0; i < NUM_SERVOS; i++) ch_[i].abort();
  active_ = false;
  if (SERIAL_LOG) Serial.println(F("Test ended"));
}

void CycleController::update(uint32_t now) {
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    if (ch_[i].waitingToStart()) {
      // Switch servos on at least START_STAGGER_MS apart, even if the main loop was held up (say by a
      // screen redraw) and several of them are overdue. Otherwise they would all start at once.
      if ((int32_t)(now - nextStartMs_) < 0) continue;
      ch_[i].update(now);
      if (!ch_[i].waitingToStart()) nextStartMs_ = now + START_STAGGER_MS;
    } else {
      ch_[i].update(now);
    }
  }
}

void CycleController::pauseAll(uint32_t now)  { for (uint8_t i = 0; i < NUM_SERVOS; i++) ch_[i].pause(now); }
void CycleController::resumeAll(uint32_t now) { for (uint8_t i = 0; i < NUM_SERVOS; i++) ch_[i].resume(now); }

void CycleController::toggle(uint8_t index, uint32_t now) {
  if (index >= NUM_SERVOS) return;
  ChannelStatus s = ch_[index].status();
  if (s == CH_RUNNING)     ch_[index].pause(now);
  else if (s == CH_PAUSED) ch_[index].resume(now);
}

uint8_t CycleController::countWithStatus(ChannelStatus s) const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < NUM_SERVOS; i++) if (ch_[i].status() == s) n++;
  return n;
}

bool CycleController::allFinished() const {
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    ChannelStatus s = ch_[i].status();
    if (s != CH_OFF && s != CH_DONE && s != CH_FAULT) return false;   // a faulted servo will never finish
  }
  return true;
}
