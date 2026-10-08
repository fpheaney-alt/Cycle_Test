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

// True once `now` has reached `deadline`, correct even when millis() wraps around.
static bool reached(uint32_t now, uint32_t deadline) {
  return (int32_t)(now - deadline) >= 0;
}

static int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

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
  // ServoEasing's 0..180 degree scale is fixed when it is attached. If the end points have been
  // changed since (calibration), detach first so that the next attach uses the new ones.
  if (attached_ && (mapStartUs_ != startUs_ || mapEndUs_ != endUs_)) release();
  if (!attached_) {
    servo_.attach(pin_, 0, startUs_, endUs_);   // logical 0 deg = start pulse, logical 180 deg = end pulse
    attached_ = servo_.attached();
    mapStartUs_ = startUs_;
    mapEndUs_ = endUs_;
  }
  return attached_;
}

void CycleChannel::release() {
  if (attached_) {
    servo_.stop();
    servo_.detach();
    attached_ = false;
  }
}

void CycleChannel::startMove(int degree) {
  servo_.startEaseTo(degree, SERVO_SPEED_DEG_PER_SEC, START_UPDATE_BY_INTERRUPT);
}

void CycleChannel::begin(uint32_t target, uint32_t now, uint32_t startDelayMs) {
  if (phase_ == PH_CAL) { release(); phase_ = PH_IDLE; }   // calibration drove the pulse directly: attach afresh
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
  if (phase_ == PH_CAL) {              // calibration drove the pulse directly, so there is no move to finish
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

    case PH_CAL: {
      int32_t dt = (int32_t)(now - calLastMs_);
      calLastMs_ = now;
      if (dt > 250) dt = 250;                       // after a long pause (a screen redraw) do not lurch
      float target = (float)calTargetUs_;
      if (calUs_ != target) {
        float step = (float)CAL_SLEW_US_PER_SEC * (float)dt / 1000.0f;
        if (calUs_ < target) { calUs_ += step; if (calUs_ > target) calUs_ = target; }
        else                 { calUs_ -= step; if (calUs_ < target) calUs_ = target; }
        servo_.writeMicroseconds((int)(calUs_ + 0.5f));
      }
      break;
    }

    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Calibration: the screen drives the pulse width directly (Servo::writeMicroseconds), bypassing
// ServoEasing's degree scale, which is only valid for the end points it was attached with.
// ---------------------------------------------------------------------------

bool CycleChannel::calEnergize(uint32_t now) {
  if (phase_ == PH_CAL) return true;
  if (phase_ != PH_IDLE) return false;              // a test is running, or the servo is still homing
  if (!ensureAttached()) return false;              // the arm goes to the start pulse, since attach() writes position 0
  phase_ = PH_CAL;
  calLastMs_ = now;
  calUs_ = (float)startUs_;
  calTargetUs_ = startUs_;
  return true;
}

bool CycleChannel::calJumpTo(int us, uint32_t now) {
  if (!calEnergize(now)) return false;
  calUs_ = (float)us;
  calTargetUs_ = us;
  servo_.writeMicroseconds(us);
  return true;
}

bool CycleChannel::calGlideTo(int us, uint32_t now) {
  if (!calEnergize(now)) return false;
  calTargetUs_ = us;
  return true;
}

void CycleChannel::calRelease() {
  if (phase_ == PH_CAL) {
    release();
    phase_ = PH_IDLE;
  }
}

// ---------------------------------------------------------------------------
// CycleController
// ---------------------------------------------------------------------------

void CycleController::init() {
  for (uint8_t i = 0; i < NUM_SERVOS; i++) ch_[i].init(i);

  // Start from the Config.h values, then use what was saved on the calibration screen if it is still valid for this Config.h.
  Calibration::defaults(saved_);
  calSource_ = CAL_FROM_CONFIG;
  if (USE_SAVED_CALIBRATION) {
    EndPoints stored;
    switch (Calibration::load(stored)) {
      case Calibration::LOAD_OK:             saved_ = stored; calSource_ = CAL_FROM_SAVED; break;
      case Calibration::LOAD_CONFIG_CHANGED: calSource_ = CAL_SAVED_IGNORED; break;
      default: break;
    }
  }
  for (uint8_t i = 0; i < NUM_SERVOS; i++) ch_[i].setEndpoints(saved_.startUs[i], saved_.endUs[i]);

  if (SERIAL_LOG) {
    Serial.print(F("Servo end points: "));
    Serial.println(calSource_ == CAL_FROM_SAVED ? F("saved calibration") :
                   calSource_ == CAL_SAVED_IGNORED ? F("Config.h (saved calibration ignored: Config.h changed)") : F("Config.h"));
  }
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

// ---------------------------------------------------------------------------
// CycleController: calibration
// ---------------------------------------------------------------------------

bool CycleController::calibrationAllowed() const {
  if (active_) return false;
  for (uint8_t i = 0; i < NUM_SERVOS; i++) if (!ch_[i].calIdle()) return false;
  return true;
}

// Only one servo at a time is switched on while calibrating (less current, nothing else holding a load).
void CycleController::calSelect(uint8_t index) {
  for (uint8_t i = 0; i < NUM_SERVOS; i++) if (i != index) ch_[i].calRelease();
}

bool CycleController::calJog(uint8_t index, bool jogEnd, int deltaUs, uint32_t now) {
  if (index >= NUM_SERVOS || !calibrationAllowed()) return false;
  calSelect(index);
  CycleChannel& c = ch_[index];
  int s = c.startUs(), e = c.endUs();

  if (!jogEnd) {
    // START: move the whole sweep, so the 180 degrees stays 180 degrees. Stop at the pulse limits.
    int lo = (s < e) ? s : e, hi = (s < e) ? e : s;
    deltaUs = clampInt(deltaUs, SERVO_PULSE_MIN_US - lo, SERVO_PULSE_MAX_US - hi);
    if (deltaUs == 0) return false;
    s += deltaUs;
    e += deltaUs;
    c.setEndpoints(s, e);
    return c.calJumpTo(s, now);
  }

  // END: move the end only, keeping it on the same side of the start and at least CAL_MIN_SWEEP_US away.
  int dir = (e >= s) ? 1 : -1;
  int ne = clampInt(e + deltaUs, SERVO_PULSE_MIN_US, SERVO_PULSE_MAX_US);
  if (dir * (ne - s) < (int)CAL_MIN_SWEEP_US) ne = clampInt(s + dir * (int)CAL_MIN_SWEEP_US, SERVO_PULSE_MIN_US, SERVO_PULSE_MAX_US);
  if (ne == e) return false;
  c.setEndpoints(s, ne);
  return c.calJumpTo(ne, now);
}

bool CycleController::calGo(uint8_t index, bool toEnd, uint32_t now) {
  if (index >= NUM_SERVOS || !calibrationAllowed()) return false;
  calSelect(index);
  return ch_[index].calGlideTo(toEnd ? ch_[index].endUs() : ch_[index].startUs(), now);
}

bool CycleController::calSetDefaults(uint8_t index, uint32_t now) {
  if (index >= NUM_SERVOS || !calibrationAllowed()) return false;
  EndPoints d;
  Calibration::defaults(d);
  ch_[index].setEndpoints(d.startUs[index], d.endUs[index]);
  if (ch_[index].calEnergized()) ch_[index].calGlideTo(d.startUs[index], now);
  return true;
}

void CycleController::calReleaseAll() {
  for (uint8_t i = 0; i < NUM_SERVOS; i++) ch_[i].calRelease();
}

bool CycleController::calDirty() const {
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    if (ch_[i].startUs() != saved_.startUs[i] || ch_[i].endUs() != saved_.endUs[i]) return true;
  }
  return false;
}

bool CycleController::calSave() {
  EndPoints now;
  for (uint8_t i = 0; i < NUM_SERVOS; i++) { now.startUs[i] = ch_[i].startUs(); now.endUs[i] = ch_[i].endUs(); }
  if (!Calibration::save(now)) {
    if (SERIAL_LOG) Serial.println(F("Calibration NOT saved"));
    return false;
  }
  saved_ = now;
  calSource_ = CAL_FROM_SAVED;
  if (SERIAL_LOG) Serial.println(F("Calibration saved"));
  return true;
}

void CycleController::calDiscard() {
  for (uint8_t i = 0; i < NUM_SERVOS; i++) ch_[i].setEndpoints(saved_.startUs[i], saved_.endUs[i]);
  calReleaseAll();
}
