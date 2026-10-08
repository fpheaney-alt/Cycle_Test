// sim.cpp - runs the real CycleTester sketch code on a PC against simulated hardware.
//
//   * the sketch's own CycleChannel.cpp, Ui.cpp and CycleTester.ino are compiled in (unity build)
//   * time is a fake clock; the 20 ms servo "interrupt" runs whenever time moves
//   * the display is a frame buffer that charges the real library's drawing time, so a screen
//     redraw stalls loop() here just as it does on the Mega
//   * touches are scripted: taps, long presses, flickers, phantom touches
//
// Build and run with "make" in this folder. Pictures of the screens are written to out/.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <string>
#include <algorithm>

#include "Arduino.h"
#include "Wire.h"
#include "Adafruit_FT6206.h"
#include "EEPROM.h"

// ---- simulated clock and shared mock state ----
uint64_t g_simUs = 0;
SerialMock Serial;
TwoWire Wire;
EEPROMClass EEPROM;
uint8_t g_pinState[80];
std::vector<ScriptedTouch> g_touchScript;
std::vector<uint64_t> g_glitchPolls;
int g_failBeginCount = 0;
unsigned long g_i2cReads = 0;

#include "Config.h"          // the sketch includes Config.h before ServoEasing.h (it sets the pulse limits); do the same here
#include "ServoEasing.h"
void simAdvanceUs(uint64_t us) {
  uint64_t target = g_simUs + us;
  for (;;) {
    uint64_t nextTick = (g_simUs / 20000ULL + 1ULL) * 20000ULL;     // the servo timer interrupt, every 20 ms
    if (nextTick <= target) { g_simUs = nextTick; ServoEasing::isrTick(); }
    else { g_simUs = target; break; }
  }
}

// Let the tests look inside the sketch (private members, file-static variables).
#define private public
#include "CycleChannel.cpp"
#include "Calibration.cpp"
#include "Ui.cpp"
#include "CycleTester.ino"
#undef private

// ===========================================================================
// test helpers
// ===========================================================================
static int g_checks = 0, g_failures = 0;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_failures++; printf("  FAIL (line %d): ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define SECTION(name) printf("\n== %s\n", name)

static uint64_t g_t0 = 0;
static double nowS() { return (double)(g_simUs - g_t0) / 1e6; }

struct Probe {                       // watches the cycle counters after every loop() pass
  uint32_t last[NUM_SERVOS] = {0, 0, 0, 0};
  std::vector<double> stamps[NUM_SERVOS];
  void reset() { for (int i = 0; i < NUM_SERVOS; i++) { last[i] = 0; stamps[i].clear(); } }
  void sample() {
    for (int i = 0; i < NUM_SERVOS; i++) {
      uint32_t c = controller.channel(i).count();
      if (c != last[i]) { stamps[i].push_back(nowS()); last[i] = c; }
    }
  }
} probe;

static double g_maxStallUs = 0;      // longest single loop() pass while the run screen was up
static double g_maxStallUsSetup = 0;
static double g_maxStallUsCal = 0;
static double g_maxTransitionUs = 0;
static unsigned long g_loops = 0;

static void runMs(uint32_t ms) {
  uint64_t end = g_simUs + (uint64_t)ms * 1000ULL;
  while (g_simUs < end) {
    uint64_t before = g_simUs;
    Screen screenBefore = screen;
    loop();
    double spent = (double)(g_simUs - before);
    if (screen == screenBefore) {                 // passes that change screens are deliberate full redraws
      if (screen == SCREEN_RUN && spent > g_maxStallUs) g_maxStallUs = spent;
      if (screen == SCREEN_SETUP && spent > g_maxStallUsSetup) g_maxStallUsSetup = spent;
      if (screen == SCREEN_CAL && spent > g_maxStallUsCal) g_maxStallUsCal = spent;
    } else if (spent > g_maxTransitionUs) {
      g_maxTransitionUs = spent;
    }
    g_loops++;
    probe.sample();
    simAdvanceUs(500);               // the rest of a loop pass when there is nothing to draw
  }
}

// A finger on screen position (x, y) for holdMs. The controller reports raw coordinates:
// the sketch maps  screen x = 480 - raw y,  screen y = raw x.
static void touchDown(int x, int y, uint32_t holdMs, uint32_t afterMs = 0) {
  ScriptedTouch t;
  t.startUs = g_simUs + 10000;
  t.endUs = t.startUs + (uint64_t)holdMs * 1000ULL;
  t.rawX = (int16_t)y;
  t.rawY = (int16_t)(480 - x);
  g_touchScript.push_back(t);
  runMs(holdMs + afterMs);
}
static void tap(int x, int y) {
  Screen before = screen;
  touchDown(x, y, 140, 500);
  if (screen != before) runMs(1500);     // a person waits for the new screen to appear (and for the 0.4 s input lock)
}
static void tapRect(const Rect& r) { tap(r.x + r.w / 2, r.y + r.h / 2); }
static void tapSetup(uint8_t row, SetupPart p) { tapRect(setupRect(row, p)); }
static void tapToggle(uint8_t row) { tapRect(runToggleRect(row)); }

static void snapshot(const char* name) {
  char path[128];
  snprintf(path, sizeof(path), "out/%s.ppm", name);
  if (!gfx.savePpm(path)) printf("  (could not write %s - does out/ exist?)\n", path);
  else printf("  saved %s\n", path);
}

static ServoEasing& servoOf(int i) { return controller.ch_[i].servo_; }
static double degPerUs() { return 180.0 / (double)(SERVO_END_US[0] - SERVO_START_US[0]); }

// ===========================================================================
// scenarios
// ===========================================================================
static void scenarioBoot() {
  SECTION("Boot");
  CHECK(screen == SCREEN_SETUP, "boots to the setup screen");
  for (int i = 0; i < NUM_SERVOS; i++) {
    CHECK(targets[i] == DEFAULT_TARGET_CYCLES, "S%d default target is %lu (got %lu)", i + 1, (unsigned long)DEFAULT_TARGET_CYCLES, (unsigned long)targets[i]);
    CHECK(!servoOf(i).attached(), "S%d gets no signal at power-up", i + 1);
  }
  int expectStart = (SERVO_FULL_TRAVEL_DEG == 270) ? 833 : 500;     // 500 + 45 * 2000/270 = 833,  500 + 225 * 2000/270 = 2167
  int expectEnd   = (SERVO_FULL_TRAVEL_DEG == 270) ? 2167 : 2500;
  for (int i = 0; i < NUM_SERVOS; i++)
    CHECK(SERVO_START_US[i] == expectStart && SERVO_END_US[i] == expectEnd,
          "S%d sweep is %d..%d us (expected %d..%d for a %d degree servo)", i + 1, SERVO_START_US[i], SERVO_END_US[i], expectStart, expectEnd, SERVO_FULL_TRAVEL_DEG);
  printf("  boot finished at t=%.2f s (touch controller init + first screen draw)\n", nowS());
  snapshot("01_setup_default");
}

static void scenarioSetupEditing() {
  SECTION("Setup screen: editing targets");
  tapSetup(0, P_MINUS_L);
  CHECK(targets[0] == 0, "-1000 from 1000 gives 0 / OFF (got %lu)", (unsigned long)targets[0]);
  tapSetup(0, P_MINUS_S);
  CHECK(targets[0] == 0, "-100 at 0 stays 0");
  tapSetup(0, P_PLUS_S);
  CHECK(targets[0] == 100, "+100 gives 100 (got %lu)", (unsigned long)targets[0]);
  tapSetup(0, P_PLUS_L);
  CHECK(targets[0] == 1100, "+1000 gives 1100 (got %lu)", (unsigned long)targets[0]);
  tapSetup(0, P_MINUS_L);
  CHECK(targets[0] == 100, "1100 - 1000 = 100 (got %lu)", (unsigned long)targets[0]);

  tapRect(COPY_BTN);
  for (int i = 1; i < NUM_SERVOS; i++) CHECK(targets[i] == 100, "ALL = S1 copies 100 to S%d (got %lu)", i + 1, (unsigned long)targets[i]);
  tapSetup(3, P_MINUS_S);
  CHECK(targets[3] == 0, "S4 switched OFF (got %lu)", (unsigned long)targets[3]);
  snapshot("02_setup_edited");

  // Nothing selected: START must refuse with a message and not touch the servos.
  for (int i = 0; i < NUM_SERVOS; i++) { targets[i] = 0; drawSetupValue(i); }
  drawEstimate();
  tapRect(START_BTN);
  CHECK(screen == SCREEN_SETUP && !controller.testActive(), "START with every servo OFF is refused");
  CHECK(setupMessageUntil != 0, "a 'Set a target first' message is showing");
  snapshot("03_setup_nothing_selected");
  runMs(2500);
  CHECK(setupMessageUntil == 0, "the message goes away by itself");

  targets[0] = targets[1] = targets[2] = 100; targets[3] = 0;
  for (int i = 0; i < NUM_SERVOS; i++) drawSetupValue(i);
  drawEstimate();

  // A single phantom touch on START (bad I2C read) must not start anything.
  g_glitchPolls.push_back(g_simUs + 100000);
  runMs(500);
  CHECK(screen == SCREEN_SETUP && !controller.testActive(), "one phantom touch on START does nothing");
}

static void scenarioHoldToRepeat() {
  SECTION("Setup screen: hold a button to repeat");
  uint32_t saved = targets[1];
  targets[1] = 0; drawSetupValue(1);
  Rect r = setupRect(1, P_PLUS_L);
  touchDown(r.x + r.w / 2, r.y + r.h / 2, 2000, 400);
  printf("  holding +1000 for 2 s gave %lu\n", (unsigned long)targets[1]);
  CHECK(targets[1] >= 10000 && targets[1] <= 16000, "2 s hold steps roughly 13 times (got %lu)", (unsigned long)targets[1]);
  uint32_t after = targets[1];
  runMs(1000);
  CHECK(targets[1] == after, "no more steps after release");

  touchDown(r.x + r.w / 2, r.y + r.h / 2, 90000, 500);
  CHECK(targets[1] == MAX_TARGET_CYCLES, "holding +1000 stops at the maximum (got %lu)", (unsigned long)targets[1]);
  snapshot("04_setup_max");

  Rect m = setupRect(1, P_MINUS_L);
  touchDown(m.x + m.w / 2, m.y + m.h / 2, 90000, 500);
  CHECK(targets[1] == 0, "holding -1000 stops at 0 (got %lu)", (unsigned long)targets[1]);
  targets[1] = saved; drawSetupValue(1); drawEstimate();
}

static void scenarioRunAndPause() {
  SECTION("Run: start, stagger, cycle timing");
  const double sweepS = (double)SWEEP_DEG / SERVO_SPEED_DEG_PER_SEC;
  const double cycleS = 2 * sweepS + (DWELL_AT_END_MS + DWELL_AT_START_MS) / 1000.0;
  printf("  expected cycle time %.3f s (2 x %.2f s sweep + dwells), pulse range %d..%d us\n", cycleS, sweepS, SERVO_START_US[0], SERVO_END_US[0]);
  printf("  estimate on the setup screen: '%s'\n", setupEstimateField.shown);

  probe.reset();
  for (int i = 0; i < NUM_SERVOS; i++) servoOf(i).resetStats();
  double tStart = nowS();
  tapRect(START_BTN);
  CHECK(screen == SCREEN_RUN && controller.testActive(), "START opens the run screen and starts the test");
  runMs(3000);
  snapshot("05_run_start");

  CHECK(servoOf(0).attached() && servoOf(1).attached() && servoOf(2).attached(), "S1..S3 are running");
  double gap12 = (servoOf(1).attachedAtUs() - servoOf(0).attachedAtUs()) / 1000.0;
  double gap23 = (servoOf(2).attachedAtUs() - servoOf(1).attachedAtUs()) / 1000.0;
  printf("  servos switched on %.0f ms and %.0f ms apart (START_STAGGER_MS = %d)\n", gap12, gap23, START_STAGGER_MS);
  CHECK(gap12 >= START_STAGGER_MS - 5 && gap23 >= START_STAGGER_MS - 5 && gap12 < START_STAGGER_MS + 120 && gap23 < START_STAGGER_MS + 120, "start stagger survives the slow redraw");
  CHECK(!servoOf(3).attached() && servoOf(3).attachCount() == 0, "S4 (OFF) is never attached");
  CHECK(controller.channel(3).status() == CH_OFF, "S4 reports OFF");
  printf("  (START tapped at t=%.2f s)\n", tStart);

  runMs(40000);
  // Cycle timing from the S1 counter timestamps.
  const std::vector<double>& t1 = probe.stamps[0];
  CHECK(t1.size() >= 5, "S1 completed several cycles in 43 s (got %zu)", t1.size());
  if (t1.size() >= 4) {
    for (size_t i = 2; i < t1.size(); i++) {
      double d = t1[i] - t1[i - 1];
      CHECK(fabs(d - cycleS) < 0.08, "S1 cycle %zu took %.3f s, expected %.3f s", i, d, cycleS);
    }
  }
  // The three servos run independently but at the same speed.
  CHECK(controller.channel(0).count() >= controller.channel(2).count(), "S1 started first");
  printf("  after ~43 s: counts = %lu / %lu / %lu / %lu\n",
         (unsigned long)controller.channel(0).count(), (unsigned long)controller.channel(1).count(),
         (unsigned long)controller.channel(2).count(), (unsigned long)controller.channel(3).count());

  // Peak speed check (sine easing peaks at about 1.57x the average).
  double peakDegPerS = servoOf(0).maxSlewUsPerSec() * degPerUs();
  printf("  S1 peak speed %.0f deg/s (average setting %d)\n", peakDegPerS, SERVO_SPEED_DEG_PER_SEC);
  double lo = SERVO_SPEED_DEG_PER_SEC * (SERVO_EASING == EASING_LINEAR ? 0.9 : 1.2);
  double hi = SERVO_SPEED_DEG_PER_SEC * 1.75;
  CHECK(peakDegPerS > lo && peakDegPerS < hi, "peak speed %.0f deg/s is in the expected band %.0f..%.0f", peakDegPerS, lo, hi);

  SECTION("Run: pause and resume one servo");
  // Pause S2 and watch that it really freezes while the others carry on.
  tapToggle(1);
  CHECK(controller.channel(1).status() == CH_PAUSED, "S2 shows PAUSED");
  double pausedPulse = servoOf(1).pulseUs();
  uint32_t pausedCount = controller.channel(1).count();
  uint32_t s1Before = controller.channel(0).count();
  snapshot("06_run_one_paused");
  runMs(20000);
  CHECK(servoOf(1).pulseUs() == pausedPulse, "S2 holds its position while paused (%.0f us -> %.0f us)", pausedPulse, servoOf(1).pulseUs());
  CHECK(controller.channel(1).count() == pausedCount, "S2 count frozen at %lu", (unsigned long)pausedCount);
  CHECK(controller.channel(0).count() >= s1Before + 3, "S1 keeps cycling meanwhile (%lu -> %lu)", (unsigned long)s1Before, (unsigned long)controller.channel(0).count());
  CHECK(servoOf(1).attached(), "a paused servo keeps holding (still attached)");

  Rect resumeBtn = runToggleRect(1);
  touchDown(resumeBtn.x + resumeBtn.w / 2, resumeBtn.y + resumeBtn.h / 2, 140, 0);
  while (controller.channel(1).status() != CH_RUNNING) runMs(2);     // sample the arm the moment the resume happens
  double afterResume = servoOf(1).pulseUs();
  CHECK(fabs(afterResume - pausedPulse) < 100, "no jump on resume: %.0f us before, %.0f us at the moment it resumed (a restart from the wrong place would be 1000+ us off)", pausedPulse, afterResume);
  runMs(500);
  CHECK(controller.channel(1).status() == CH_RUNNING, "S2 resumed");
  runMs(15000);
  CHECK(controller.channel(1).count() > pausedCount, "S2 counting again (%lu -> %lu)", (unsigned long)pausedCount, (unsigned long)controller.channel(1).count());

  // Pause exactly while a servo is resting between sweeps, to check the dwell timer is paused too.
  SECTION("Run: pause during a dwell");
  // Wait for S1 to be in a hold phase, pause, wait longer than the dwell, resume and make sure it still counts normally.
  while (!(controller.ch_[0].phase_ == CycleChannel::PH_HOLD_END)) runMs(5);
  tapToggle(0);   // the tap itself takes ~0.1 s so the dwell may be over already; this just must not break anything
  uint32_t c0 = controller.channel(0).count();
  runMs(5000);
  CHECK(controller.channel(0).count() == c0, "S1 frozen while paused in a hold");
  tapToggle(0);
  runMs(12000);
  CHECK(controller.channel(0).count() > c0, "S1 carries on after resume");

  SECTION("Run: pause all / resume all");
  tapRect(PAUSE_ALL_BTN);
  CHECK(controller.countWithStatus(CH_PAUSED) == 3 && controller.countWithStatus(CH_RUNNING) == 0, "PAUSE ALL pauses every active servo");
  CHECK(shownPauseAll == 0 && shownResumeAll == 1, "PAUSE ALL greys out, RESUME ALL lights up");
  double p0 = servoOf(0).pulseUs(), p1 = servoOf(1).pulseUs(), p2 = servoOf(2).pulseUs();
  uint32_t k0 = controller.channel(0).count(), k1 = controller.channel(1).count(), k2 = controller.channel(2).count();
  runMs(8000);
  snapshot("07_run_all_paused");
  CHECK(servoOf(0).pulseUs() == p0 && servoOf(1).pulseUs() == p1 && servoOf(2).pulseUs() == p2, "all arms hold still");
  CHECK(controller.channel(0).count() == k0 && controller.channel(1).count() == k1 && controller.channel(2).count() == k2, "all counts frozen");

  // Resume one by one (individual resume after PAUSE ALL).
  tapToggle(2);
  CHECK(controller.channel(2).status() == CH_RUNNING && controller.channel(0).status() == CH_PAUSED, "S3 can be resumed on its own");
  runMs(6000);
  CHECK(shownPauseAll == 1 && shownResumeAll == 1, "with a mix of running and paused servos, both buttons are live");
  tapRect(RESUME_ALL_BTN);
  CHECK(controller.countWithStatus(CH_RUNNING) == 3, "RESUME ALL restarts the paused ones even while another is running");
  CHECK(shownResumeAll == 0, "RESUME ALL greys out when nothing is paused");
  runMs(5000);

  SECTION("Run: a flickering finger on a button toggles only once");
  ScriptedTouch a, b;
  Rect tr = runToggleRect(0);
  a.startUs = g_simUs + 10000; a.endUs = a.startUs + 500000;
  b.startUs = a.endUs + 28000; b.endUs = b.startUs + 500000;     // one missed poll in the middle of a long press
  a.rawX = b.rawX = tr.y + tr.h / 2; a.rawY = b.rawY = 480 - (tr.x + tr.w / 2);
  g_touchScript.push_back(a); g_touchScript.push_back(b);
  runMs(1800);
  CHECK(controller.channel(0).status() == CH_PAUSED, "S1 toggled exactly once by a flickering press (status %d)", (int)controller.channel(0).status());
  tapToggle(0);
  CHECK(controller.channel(0).status() == CH_RUNNING, "and toggles back with the next tap");
}

static void scenarioFinish() {
  SECTION("Run: finish");
  int guard = 0;
  while (!controller.allFinished() && guard++ < 2000) runMs(1000);
  runMs(3000);
  snapshot("08_run_complete");
  for (int i = 0; i < 3; i++) {
    CHECK(controller.channel(i).count() == 100, "S%d finished exactly 100 cycles (got %lu)", i + 1, (unsigned long)controller.channel(i).count());
    CHECK(controller.channel(i).status() == CH_DONE, "S%d shows DONE", i + 1);
    CHECK(!servoOf(i).attached(), "S%d is released when done", i + 1);
    CHECK(servoOf(i).pulseUs() == SERVO_START_US[i], "S%d ended at the start position (%.0f us, wanted %d)", i + 1, servoOf(i).pulseUs(), SERVO_START_US[i]);
    CHECK(servoOf(i).minUs() == SERVO_START_US[i] && servoOf(i).maxUs() == SERVO_END_US[i],
          "S%d never left its range: saw %d..%d us (allowed %d..%d)", i + 1, servoOf(i).minUs(), servoOf(i).maxUs(), SERVO_START_US[i], SERVO_END_US[i]);
    CHECK(servoOf(i).attachCount() == 1, "S%d attached exactly once (got %d)", i + 1, servoOf(i).attachCount());
  }
  CHECK(controller.channel(3).count() == 0 && servoOf(3).attachCount() == 0, "S4 stayed off");
  CHECK(shownOverall == OV_COMPLETE, "header says COMPLETE");
  CHECK(ServoEasing::leakedAttaches() == 0, "no servo was attached twice (%d leaked table slots)", ServoEasing::leakedAttaches());
  printf("  total run time %.1f s of simulated time\n", nowS());

  SECTION("Run: NEW TEST returns to setup with no confirmation");
  tapRect(SETUP_BTN);
  CHECK(screen == SCREEN_SETUP && !controller.testActive(), "back on the setup screen");
  CHECK(targets[0] == 100 && targets[3] == 0, "previous targets are kept for the next test");
}

static void scenarioAbortFlow() {
  SECTION("Run: SETUP asks for confirmation, then homes the servos");
  probe.reset();
  for (int i = 0; i < NUM_SERVOS; i++) servoOf(i).resetStats();
  tapRect(START_BTN);
  runMs(20000);
  CHECK(controller.channel(0).count() >= 3, "test is running");

  tapRect(SETUP_BTN);
  CHECK(screen == SCREEN_RUN && confirmPending, "first tap on SETUP only asks 'SURE?'");
  snapshot("09_run_confirm");
  runMs(3500);
  CHECK(!confirmPending, "the question times out");
  CHECK(shownSetupBtn == SB_SETUP, "button goes back to SETUP");

  tapRect(SETUP_BTN);
  tapToggle(1);                                   // some other tap in between cancels the question
  CHECK(!confirmPending && screen == SCREEN_RUN, "tapping something else cancels the question");

  // Abort in the middle of a sweep: the servo should go home slowly, then be released.
  while (!(controller.ch_[0].phase_ == CycleChannel::PH_TO_END)) runMs(5);
  runMs(700);
  tapRect(SETUP_BTN);
  tapRect(SETUP_BTN);
  CHECK(screen == SCREEN_SETUP && !controller.testActive(), "second tap confirms and returns to setup");
  double rightAfter = servoOf(0).pulseUs();
  runMs(4000);
  CHECK(!servoOf(0).attached(), "S1 released after homing");
  CHECK(servoOf(0).pulseUs() == SERVO_START_US[0], "S1 went back to its start position (%.0f us -> %.0f us)", rightAfter, servoOf(0).pulseUs());
  CHECK(ServoEasing::leakedAttaches() == 0, "still no leaked servo slots");
  snapshot("10_setup_after_abort");

  // Start again immediately after an abort, while a servo might still be homing.
  tapRect(START_BTN);
  tapRect(SETUP_BTN); tapRect(SETUP_BTN);         // abort again right away
  tapRect(START_BTN);
  runMs(15000);
  CHECK(controller.channel(0).count() >= 1 && controller.channel(0).status() == CH_RUNNING, "restarting right after an abort works");
  CHECK(ServoEasing::leakedAttaches() == 0, "and never attaches a servo twice (%d)", ServoEasing::leakedAttaches());
}


// ---------------------------------------------------------------------------
// Calibration screen
// ---------------------------------------------------------------------------
static int startOf(int i) { return controller.ch_[i].startUs(); }
static int endOf(int i)   { return controller.ch_[i].endUs(); }
static int attachedCount() { int n = 0; for (int i = 0; i < NUM_SERVOS; i++) if (servoOf(i).attached()) n++; return n; }
static void tapJog(uint8_t row, uint8_t k) { tapRect(calJogRect(row, k)); }

// Puts a servo into the 'still homing' state and returns the CALIBRATE button, so the warning text can be checked.
static Rect setupMessageBusyProbe() {
  if (screen != SCREEN_SETUP) { tapRect(CAL_BACK_BTN); runMs(1000); }
  controller.begin(targets, millis());
  runMs(4000);
  controller.abort();
  return CAL_BTN;
}

static void scenarioCalibration() {
  SECTION("Calibration: getting in");
  if (screen == SCREEN_RUN) { tapRect(SETUP_BTN); tapRect(SETUP_BTN); }
  runMs(5000);
  CHECK(screen == SCREEN_SETUP && controller.calibrationAllowed(), "on the setup screen with every servo idle");
  CHECK(servoOf(0).clampMaxUs() == MAXIMUM_PULSE_WIDTH && MAXIMUM_PULSE_WIDTH == 2600,
        "the Servo library will pass pulses up to %d us (the old default clipped at 2476)", servoOf(0).clampMaxUs());

  // Not while a test is running or a servo is still homing.
  controller.begin(targets, millis());
  runMs(6000);
  CHECK(!controller.calibrationAllowed(), "calibration is refused while a test is running");
  tapRect(CAL_BTN);
  CHECK(screen == SCREEN_SETUP && setupMessageBusy && setupMessageUntil != 0, "tapping CALIBRATE then only shows a warning");
  controller.abort();
  CHECK(!controller.calibrationAllowed(), "...and while the servos are still homing");
  runMs(5000);
  CHECK(controller.calibrationAllowed(), "allowed again once they are home");
  runMs(3000);

  tapRect(CAL_BTN);
  CHECK(screen == SCREEN_CAL, "CALIBRATE opens the calibration screen");
  CHECK(attachedCount() == 0, "no servo is switched on just by opening it");
  snapshot("11_cal_entry");

  SECTION("Calibration: START shifts the sweep, END changes it");
  tapRect(calTabRect(1));
  CHECK(calServo == 1 && attachedCount() == 0, "picking a servo selects it without switching it on");
  const int s0 = startOf(1), e0 = endOf(1);
  const int stepS = CAL_STEP_US[0], stepM = CAL_STEP_US[1], stepL = CAL_STEP_US[2];
  printf("  S2 starts at %d..%d us (sweep %d us); steps are %d / %d / %d us\n", s0, e0, e0 - s0, stepS, stepM, stepL);

  tapJog(0, 3);                                           // START, +small
  CHECK(startOf(1) == s0 + stepS && endOf(1) == e0 + stepS, "START +%d us moves BOTH ends by %d us (sweep unchanged): now %d..%d", stepS, stepS, startOf(1), endOf(1));
  CHECK(servoOf(1).attached() && attachedCount() == 1, "only the servo being adjusted is switched on");
  CHECK(servoOf(1).pulseUs() == startOf(1), "the arm moved to the new start (%.0f us)", servoOf(1).pulseUs());
  double stepDeg = stepS * 180.0 / (double)(SERVO_END_US[0] - SERVO_START_US[0]);
  printf("  one small step = %d us = about %.2f degrees\n", stepS, stepDeg);
  CHECK(stepDeg < 1.0, "the smallest step is well under one degree (%.2f)", stepDeg);

  tapJog(1, 1);                                           // END, -medium
  CHECK(endOf(1) == e0 + stepS - stepM && startOf(1) == s0 + stepS, "END -%d us moves only the end: now %d..%d", stepM, startOf(1), endOf(1));
  double mid = servoOf(1).pulseUs();
  CHECK(mid > startOf(1) + 50 && mid < endOf(1) - 100, "going from the START row to the END row GLIDES (arm at %.0f us, between %d and %d), it does not jump", mid, startOf(1), endOf(1));
  runMs(3500);
  CHECK(servoOf(1).pulseUs() == endOf(1), "and arrives at the end position (%.0f us)", servoOf(1).pulseUs());
  snapshot("12_cal_adjusted");

  // limits: shifting never changes the sweep, and never leaves the pulse limits
  int span = endOf(1) - startOf(1);
  for (int i = 0; i < 400; i++) controller.calJog(1, false, +stepL, millis());
  CHECK((startOf(1) > endOf(1) ? startOf(1) : endOf(1)) == SERVO_PULSE_MAX_US && endOf(1) - startOf(1) == span, "shifting up stops exactly at %d us with the sweep still %d us", SERVO_PULSE_MAX_US, span);
  for (int i = 0; i < 400; i++) controller.calJog(1, false, -stepL, millis());
  CHECK((startOf(1) < endOf(1) ? startOf(1) : endOf(1)) == SERVO_PULSE_MIN_US && endOf(1) - startOf(1) == span, "shifting down stops exactly at %d us with the sweep still %d us", SERVO_PULSE_MIN_US, span);
  bool moved = controller.calJog(1, false, -stepL, millis());
  CHECK(!moved, "a jog at the limit reports that nothing changed");
  tapRect(calJogRect(0, 0));
  CHECK(calMessage != nullptr && strcmp(calMessage, "At the limit") == 0, "and the screen says so");

  for (int i = 0; i < 400; i++) controller.calJog(1, true, +stepL, millis());
  CHECK(endOf(1) == SERVO_PULSE_MAX_US, "END can reach the top limit, %d us", endOf(1));
  CHECK(servoOf(1).clampMaxUs() >= SERVO_PULSE_MAX_US, "...and the Servo library really lets that pulse through");
  for (int i = 0; i < 400; i++) controller.calJog(1, true, -stepL, millis());
  CHECK(endOf(1) - startOf(1) == (int)CAL_MIN_SWEEP_US, "END cannot come closer than %d us to START (it is %d)", (int)CAL_MIN_SWEEP_US, endOf(1) - startOf(1));
  for (int i = 0; i < 400; i++) controller.calJog(2, true, +stepL, millis());      // a different servo; S2 is released
  CHECK(attachedCount() == 1 && servoOf(2).attached() && !servoOf(1).attached(), "jogging another servo releases the first");

  SECTION("Calibration: DEFAULT, GO START and GO END glide slowly");
  tapRect(calTabRect(1));
  tapRect(CAL_DEFAULT_BTN);
  CHECK(startOf(1) == SERVO_START_US[1] && endOf(1) == SERVO_END_US[1], "DEFAULT restores the Config.h values");
  tapJog(0, 3);
  tapJog(0, 2);                                            // net zero
  runMs(5000);
  CHECK(servoOf(1).pulseUs() == startOf(1), "arm at start");
  tapRect(CAL_GO_END_BTN);
  double p1 = servoOf(1).pulseUs();
  CHECK(p1 > startOf(1) + 100 && p1 < endOf(1) - 100, "GO END is a glide, not a jump (%.0f us between %d and %d)", p1, startOf(1), endOf(1));
  runMs(400);
  double p2 = servoOf(1).pulseUs();
  double rate = (p2 - p1) / 0.4;
  CHECK(rate > CAL_SLEW_US_PER_SEC * 0.75 && rate < CAL_SLEW_US_PER_SEC * 1.25, "glide speed %.0f us/s (setting %d)", rate, (int)CAL_SLEW_US_PER_SEC);
  runMs(4000);
  CHECK(servoOf(1).pulseUs() == endOf(1), "arm arrives at the end");
  tapRect(CAL_GO_START_BTN);
  runMs(4500);
  CHECK(servoOf(1).pulseUs() == startOf(1), "and back at the start");

  SECTION("Calibration: saving, loading, damage");
  tapJog(0, 4);                                            // START +medium
  tapJog(1, 0);                                            // END -large
  const int sSaved = startOf(1), eSaved = endOf(1);
  CHECK(controller.calDirty(), "changes are marked unsaved");
  snapshot("13_cal_unsaved");
  CHECK(EEPROM.byteWrites == 0, "jogging writes nothing to EEPROM (%lu bytes so far)", EEPROM.byteWrites);
  tapRect(CAL_SAVE_BTN);
  CHECK(!controller.calDirty() && calMessage != nullptr && strcmp(calMessage, "Saved") == 0, "SAVE stores the values and says so");
  unsigned long afterSave = EEPROM.byteWrites;
  CHECK(afterSave > 0 && afterSave <= sizeof(Block), "a save writes %lu bytes", afterSave);
  tapRect(CAL_SAVE_BTN);
  CHECK(EEPROM.byteWrites == afterSave, "saving identical values again writes nothing (no wear)");

  controller.ch_[1].setEndpoints(1000, 2000);               // pretend a power cycle: forget everything, load again
  controller.init();
  CHECK(startOf(1) == sSaved && endOf(1) == eSaved && controller.calSource() == CycleController::CAL_FROM_SAVED, "after a restart the saved values are back (%d..%d)", startOf(1), endOf(1));

  EEPROM.mem[10] ^= 0xFF;                                   // damage one byte of the only saved copy
  controller.init();
  CHECK(startOf(1) == SERVO_START_US[1] && controller.calSource() == CycleController::CAL_SAVED_BAD, "damaged data is rejected, the Config.h values are used, and it is reported as damaged");
  EEPROM.mem[10] ^= 0xFF;
  controller.init();
  CHECK(startOf(1) == sSaved, "(undamaged again, it loads)");

  Block b; EEPROM.get(0, b);                                // a block saved under a different Config.h
  b.signature ^= 0x1234; b.crc = blockCrc(b); EEPROM.put(0, b);
  controller.init();
  CHECK(startOf(1) == SERVO_START_US[1] && controller.calSource() == CycleController::CAL_SAVED_IGNORED, "values saved for another Config.h are ignored, so a Config.h edit always wins");
  controller.ch_[1].setEndpoints(sSaved, eSaved);           // put the good values back
  CHECK(controller.calSave(), "and a new save replaces the stale block");

  memset(EEPROM.mem, 0xFF, 2 * sizeof(Block));              // blank EEPROM
  controller.init();
  CHECK(controller.calSource() == CycleController::CAL_FROM_CONFIG && startOf(1) == SERVO_START_US[1], "a blank EEPROM gives the Config.h values");
  controller.ch_[1].setEndpoints(sSaved, eSaved);
  controller.calSave();

  SECTION("Calibration: moving between servos and rows never whips an arm");
  controller.calReleaseAll();
  runMs(100);
  for (int i = 0; i < NUM_SERVOS; i++) servoOf(i).resetStats();
  tapRect(calTabRect(1));
  tapRect(CAL_GO_END_BTN);                                  // switches S2 on (where it was left) and glides to its end
  runMs(5000);
  const int endPulse = (int)servoOf(1).pulseUs();
  CHECK(endPulse == endOf(1), "S2 is at its end (%d us)", endPulse);
  g_pinState[SERVO_PINS[1]] = HIGH;                         // pretend a detach left the signal line stuck high
  tapRect(calTabRect(2));                                   // pick S3: S2 is released limp at its end
  CHECK(!servoOf(1).attached(), "S2 released");
  CHECK(g_pinState[SERVO_PINS[1]] == LOW, "the released servo's signal line is driven LOW, not left high");
  tapRect(calTabRect(1));
  servoOf(1).resetStats();
  tapRect(CAL_GO_START_BTN);                                // switch S2 on again
  runMs(300);
  CHECK(abs(servoOf(1).firstHeldUs() - endPulse) <= 20, "switching a servo on again starts from where the arm was left (%d us), not from the start pulse %d us: the first pulse sent was %d us", endPulse, startOf(1), servoOf(1).firstHeldUs());
  runMs(5000);
  CHECK(servoOf(1).pulseUs() == startOf(1), "then it glides to the start");
  for (int i = 0; i < NUM_SERVOS; i++) {
    CHECK(servoOf(i).maxStepUs() <= 160, "S%d never took a step bigger than 160 us between pulses during calibration (biggest %d)", i + 1, servoOf(i).maxStepUs());
  }

  SECTION("Calibration: a power cut in the middle of SAVE never loses the previous values");
  tapRect(CAL_BACK_BTN);                                    // nothing unsaved: leaves at once
  runMs(4500);
  CHECK(screen == SCREEN_SETUP, "on the setup screen");
  EndPoints orig;
  for (int i = 0; i < NUM_SERVOS; i++) { orig.startUs[i] = startOf(i); orig.endUs[i] = endOf(i); }
  EndPoints oldSet, newSet;                                  // two different, valid sets of values
  for (int i = 0; i < NUM_SERVOS; i++) {
    oldSet.startUs[i] = (int16_t)(900 + 5 * i);  oldSet.endUs[i] = (int16_t)(2100 - 3 * i);
    newSet.startUs[i] = (int16_t)(1000 + 7 * i); newSet.endUs[i] = (int16_t)(2000 + 11 * i);
  }
  auto apply = [](const EndPoints& e) { for (int i = 0; i < NUM_SERVOS; i++) controller.ch_[i].setEndpoints(e.startUs[i], e.endUs[i]); };
  auto matches = [](const EndPoints& e) {
    for (int i = 0; i < NUM_SERVOS; i++) if (startOf(i) != e.startUs[i] || endOf(i) != e.endUs[i]) return false;
    return true;
  };
  apply(oldSet);
  CHECK(controller.calSave(), "stored the starting set");
  int keptOld = 0, gotNew = 0, bad = 0;
  for (long cut = 0; cut <= (long)(2 * sizeof(Block)); cut++) {
    apply(newSet);
    EEPROM.powerCutAfterBytes = cut;                        // power fails after `cut` bytes of this save
    controller.calSave();
    EEPROM.powerCutAfterBytes = -1;                         // power is back
    apply(newSet);
    for (int i = 0; i < NUM_SERVOS; i++) controller.ch_[i].setEndpoints(1000, 2000);   // forget everything, as a restart does
    controller.init();
    bool isOld = matches(oldSet), isNew = matches(newSet);
    if (controller.calSource() != CycleController::CAL_FROM_SAVED || (!isOld && !isNew)) bad++;
    else if (isOld) keptOld++; else gotNew++;
    apply(oldSet);                                          // restore the starting state for the next cut position
    controller.calSave();
    controller.init();
    if (!matches(oldSet)) { bad++; break; }
  }
  printf("  a power cut after every byte of the write: %d times the old values survived, %d times the new ones, %d times anything else\n", keptOld, gotNew, bad);
  CHECK(bad == 0, "a power cut during SAVE never produced factory values, a damaged-data report or a mix of old and new");
  CHECK(keptOld > 0 && gotNew > 0, "both outcomes were exercised (old survived %d, new stored %d)", keptOld, gotNew);

  SECTION("Calibration: damaged copies are reported, one good copy is enough");
  apply(oldSet);
  controller.calSave();
  controller.calSave();
  const int damageSlot0 = 10, damageSlot1 = (int)sizeof(Block) + 10;
  EEPROM.mem[damageSlot0] ^= 0xFF;
  controller.init();
  CHECK(controller.calSource() == CycleController::CAL_FROM_SAVED, "with one copy damaged the other still loads");
  EEPROM.mem[damageSlot1] ^= 0xFF;
  controller.init();
  CHECK(controller.calSource() == CycleController::CAL_SAVED_BAD && startOf(1) == SERVO_START_US[1], "with both copies damaged: factory values, reported as damaged");
  tapRect(CAL_BTN);
  CHECK(screen == SCREEN_CAL && calMessage != nullptr && strcmp(calMessage, "Saved cal damaged, factory used") == 0, "and the calibration screen tells you");
  tapRect(CAL_BACK_BTN);
  runMs(2000);
  EEPROM.mem[damageSlot0] ^= 0xFF;
  EEPROM.mem[damageSlot1] ^= 0xFF;                           // repair both
  controller.init();
  CHECK(controller.calSource() == CycleController::CAL_FROM_SAVED && matches(oldSet), "repaired copies load again");
  apply(orig);                                               // back to what the later sections expect
  controller.calSave();
  controller.init();
  CHECK(matches(orig), "(original values restored)");

  SECTION("Calibration: values saved by the first release are still read");
  {
    EndPoints legacyValues = oldSet;
    legacyValues.startUs[1] = 861;
    legacyValues.endUs[1] = 2194;
    memset(EEPROM.mem, 0xFF, 4 * sizeof(Block));
    LegacyBlock lb;
    memset(&lb, 0, sizeof(lb));
    lb.magic = 0xCA1B; lb.version = 1; lb.servos = NUM_SERVOS;
    // use the real signature: save a current-format block, copy its signature, then wipe and write the v1 layout instead
    apply(legacyValues);
    controller.calSave();
    Block current; EEPROM.get(0, current);
    lb.signature = current.signature;
    lb.points = legacyValues;
    lb.crc = crc16((const uint8_t*)&lb, offsetof(LegacyBlock, crc));
    memset(EEPROM.mem, 0xFF, 4 * sizeof(Block));
    EEPROM.put(0, lb);
    apply(orig);
    controller.init();
    CHECK(controller.calSource() == CycleController::CAL_FROM_SAVED && matches(legacyValues), "a block written by the first release loads: S2 start %d end %d", startOf(1), endOf(1));
    EndPoints newer = legacyValues;
    newer.startUs[1] = 870;
    apply(newer);
    uint8_t legacyBytes[sizeof(LegacyBlock)];
    memcpy(legacyBytes, EEPROM.mem, sizeof(legacyBytes));
    CHECK(controller.calSave(), "saving with the old block present works");
    CHECK(memcmp(legacyBytes, EEPROM.mem, sizeof(legacyBytes)) == 0, "and leaves the old block intact (it goes to the other slot), so a power cut cannot lose it");
    for (int i = 0; i < NUM_SERVOS; i++) controller.ch_[i].setEndpoints(1000, 2000);
    controller.init();
    CHECK(controller.calSource() == CycleController::CAL_FROM_SAVED && matches(newer), "and the new values win on the next start-up");
    apply(orig);
    controller.calSave();
    controller.init();
  }

  SECTION("Calibration: the text of the new screens is drawn completely");
  gfx.Fill_Rect(0, 0, 480, 320, 0);
  drawGlyph(100, 100, 'g', 2, 0xFFFF, 0);                    // 'g' has its tail in the 8th row of the glyph
  CHECK(gfx.pixel(100 + 1 * 2, 100 + 7 * 2) == 0xFFFF, "the tail of a 'g' is drawn (descenders used to be cut off)");
  tapRect(setupMessageBusyProbe());
  CHECK(screen == SCREEN_SETUP && strncmp(setupEstimateField.shown, "Servos still moving, wait", 25) == 0, "the warning is shown in full, not cut off: '%s'", setupEstimateField.shown);
  runMs(8000);
  tapRect(CAL_BTN);
  CHECK(screen == SCREEN_CAL, "back in calibration");
  tapRect(calTabRect(1));

  SECTION("Calibration: BACK, discard, and using the result");
  tapJog(0, 4);                                            // an unsaved change
  CHECK(controller.calDirty(), "an unsaved change");
  tapRect(CAL_BACK_BTN);
  CHECK(screen == SCREEN_CAL && calDiscardPending, "BACK with unsaved changes asks first");
  snapshot("14_cal_discard_question");
  runMs(3500);
  CHECK(!calDiscardPending && screen == SCREEN_CAL, "the question times out");
  tapRect(CAL_BACK_BTN);
  tapRect(CAL_BACK_BTN);
  CHECK(screen == SCREEN_SETUP, "a second tap leaves");
  CHECK(startOf(1) == sSaved && endOf(1) == eSaved && !controller.calDirty(), "the unsaved change was discarded");
  runMs(5000);
  CHECK(attachedCount() == 0, "every servo is released once it has glided home");
  CHECK(servoOf(1).pulseUs() == sSaved, "and it was released AT the start position (%.0f us, start is %d)", servoOf(1).pulseUs(), sSaved);

  tapRect(CAL_BTN);
  CHECK(screen == SCREEN_CAL && calMessage != nullptr && strcmp(calMessage, "Loaded saved calibration") == 0, "reopening says the saved calibration is in use");
  tapRect(calTabRect(1));
  tapJog(0, 5);                                            // START +large, then keep it
  tapRect(CAL_SAVE_BTN);
  tapRect(CAL_BACK_BTN);
  CHECK(screen == SCREEN_SETUP, "BACK with nothing unsaved leaves at once");

  for (int i = 0; i < NUM_SERVOS; i++) servoOf(i).resetStats();
  targets[0] = targets[1] = targets[2] = 100; targets[3] = 100;
  tapRect(START_BTN);
  runMs(40000);
  CHECK(servoOf(1).minUs() == (startOf(1) < endOf(1) ? startOf(1) : endOf(1)) && servoOf(1).maxUs() == (startOf(1) < endOf(1) ? endOf(1) : startOf(1)),
        "a test uses the calibrated range: S2 moved over %d..%d us (calibrated %d..%d)", servoOf(1).minUs(), servoOf(1).maxUs(), startOf(1), endOf(1));
  CHECK(servoOf(0).minUs() == SERVO_START_US[0] && servoOf(0).maxUs() == SERVO_END_US[0], "an uncalibrated servo still uses the Config.h values (%d..%d)", servoOf(0).minUs(), servoOf(0).maxUs());
  CHECK(ServoEasing::leakedAttaches() == 0, "no servo was attached twice during all of this");
  tapRect(SETUP_BTN); tapRect(SETUP_BTN);
  runMs(4000);

  SECTION("Calibration: hold to repeat, an OFF servo, reversed servo");
  tapRect(CAL_BTN);
  tapRect(calTabRect(3));                                   // servo 4 is OFF in the test but can still be calibrated
  tapRect(CAL_DEFAULT_BTN);
  const int h0 = startOf(3);
  Rect plus1 = calJogRect(0, 3);
  touchDown(plus1.x + plus1.w / 2, plus1.y + plus1.h / 2, 2000, 500);
  int steps = startOf(3) - h0;
  printf("  holding +%d us for 2 s gave %d steps\n", stepS, steps);
  CHECK(steps >= 8 && steps <= 20, "holding a jog button repeats (%d steps)", steps);
  CHECK(servoOf(3).attached() && attachedCount() == 1, "an OFF servo can be calibrated too");
  tapRect(CAL_DEFAULT_BTN);
  controller.ch_[3].setEndpoints(endOf(3), startOf(3));      // swapped ends = reversed direction
  CHECK(controller.calJog(3, true, +stepM, millis()) && endOf(3) < startOf(3), "a reversed servo keeps its direction when END is jogged");
  snapshot("15_cal_reversed");
  tapRect(CAL_DEFAULT_BTN);
  tapRect(CAL_BACK_BTN);
  CHECK(screen == SCREEN_SETUP || calDiscardPending, "leaving");
  if (screen == SCREEN_CAL) { tapRect(CAL_BACK_BTN); }
  CHECK(screen == SCREEN_SETUP, "back on the setup screen");
}

// Used by "make test180"/"make test270": start a test with the default targets and make sure the arms use exactly their configured pulse range.
static void scenarioShortRun() {
  SECTION("Short run: pulse range actually used");
  tapRect(START_BTN);
  runMs(30000);
  for (int i = 0; i < NUM_SERVOS; i++) {
    CHECK(servoOf(i).minUs() == SERVO_START_US[i] && servoOf(i).maxUs() == SERVO_END_US[i],
          "S%d moved over %d..%d us (configured %d..%d)", i + 1, servoOf(i).minUs(), servoOf(i).maxUs(), SERVO_START_US[i], SERVO_END_US[i]);
    CHECK(controller.channel(i).count() >= 3, "S%d is cycling", i + 1);
  }
}


// With a 180 degree servo the factory sweep already fills the servo's whole travel (500..2500 us), so the START
// row cannot shift anything until END has been shortened. Used by "make test180".
static void scenarioCalibration180() {
  SECTION("Calibration with a 180 degree servo (the sweep fills the whole travel)");
  tapRect(CAL_BTN);
  CHECK(screen == SCREEN_CAL, "calibration opens");
  tapRect(calTabRect(0));
  CHECK(startOf(0) == SERVO_PULSE_MIN_US && endOf(0) == SERVO_PULSE_MAX_US, "the factory sweep is the whole travel, %d..%d us", startOf(0), endOf(0));
  CHECK(!controller.calJog(0, false, +25, millis()) && !servoOf(0).attached(), "START cannot shift a sweep that already fills the travel (and nothing is switched on)");
  tapJog(0, 5);
  CHECK(calMessage != nullptr && strcmp(calMessage, "At the limit") == 0, "the screen says 'At the limit'");
  tapJog(1, 0);                                              // END -25
  CHECK(endOf(0) == SERVO_PULSE_MAX_US - 25, "END can be shortened (%d us)", endOf(0));
  tapJog(0, 5);                                              // START +25 now has room
  CHECK(startOf(0) == SERVO_PULSE_MIN_US + 25 && endOf(0) == SERVO_PULSE_MAX_US, "then START shifts both ends: %d..%d us", startOf(0), endOf(0));
  tapRect(CAL_GO_END_BTN);
  runMs(6000);
  CHECK(servoOf(0).pulseUs() == SERVO_PULSE_MAX_US, "the arm really reaches %d us (the Servo library no longer clips at 2476)", SERVO_PULSE_MAX_US);
  tapRect(CAL_SAVE_BTN);
  tapRect(CAL_DEFAULT_BTN);                                  // put the factory values back for what follows
  tapRect(CAL_SAVE_BTN);
  tapRect(CAL_BACK_BTN);
  runMs(6000);
  CHECK(screen == SCREEN_SETUP && attachedCount() == 0, "back on the setup screen, servo released");
}

// With USE_SAVED_CALIBRATION = false the Config.h values are always used, and SAVE must not touch what is stored.
// Used by "make testnosave".
static void scenarioNoSave() {
  SECTION("Saving switched off in Config.h (USE_SAVED_CALIBRATION = false)");
  CHECK(!USE_SAVED_CALIBRATION, "(this variant is built with saved calibration switched off)");
  EndPoints stored;
  Calibration::defaults(stored);
  stored.startUs[1] += 20; stored.endUs[1] -= 20;
  CHECK(Calibration::save(stored), "(a valid block is already stored in EEPROM)");
  unsigned long written = EEPROM.byteWrites;
  controller.init();
  CHECK(startOf(1) == SERVO_START_US[1] && controller.calSource() == CycleController::CAL_FROM_CONFIG, "stored values are ignored at start-up");
  tapRect(CAL_BTN);
  CHECK(screen == SCREEN_CAL, "calibration still opens");
  tapRect(calTabRect(0));
  tapJog(0, 4);
  CHECK(controller.calDirty(), "adjusting still works");
  tapRect(CAL_SAVE_BTN);
  CHECK(calMessage != nullptr && strcmp(calMessage, "Saving is off in Config.h") == 0, "SAVE says it is switched off, instead of reporting 'Saved'");
  CHECK(EEPROM.byteWrites == written, "and writes nothing, so the stored calibration is left alone");
  EndPoints check;
  CHECK(Calibration::load(check) == Calibration::LOAD_OK && check.startUs[1] == stored.startUs[1], "what was stored is still intact");
}

static void reportStalls() {
  SECTION("Responsiveness");
  printf("  longest single pass of loop(): %.0f ms on the run screen, %.0f ms on the setup screen\n", g_maxStallUs / 1000.0, g_maxStallUsSetup / 1000.0);
  printf("  calibration screen: longest pass %.0f ms\n", g_maxStallUsCal / 1000.0);
  printf("  switching screens (full redraw) takes up to %.1f s\n", g_maxTransitionUs / 1e6);
  printf("  (the servos are moved by the timer interrupt, so these pauses do not make the arms stutter)\n");
  printf("  touch controller I2C reads: %lu over %.0f s = %.0f per second\n", g_i2cReads, nowS(), g_i2cReads / nowS());
  CHECK(g_maxStallUs < 400000, "run-screen updates never block loop() for more than 0.4 s");
  CHECK(g_maxStallUsSetup < 400000, "setup-screen updates never block loop() for more than 0.4 s");
  CHECK(g_maxStallUsCal < 400000, "calibration-screen updates never block loop() for more than 0.4 s");
}

// ===========================================================================
int main(int argc, char** argv) {
  bool quick = false, shortRun = false, noSave = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--quick")) quick = true;
    if (!strcmp(argv[i], "--nosave")) { quick = true; noSave = true; }
    if (!strcmp(argv[i], "--short")) { quick = true; shortRun = true; }
    if (!strcmp(argv[i], "--touch-fail") && i + 1 < argc) g_failBeginCount = atoi(argv[++i]);
  }

  // Start the clock a minute before millis() rolls over (2^32 ms), so a wrap-around happens mid-test.
  g_simUs = (0x100000000ULL - 75000ULL) * 1000ULL;
  g_t0 = g_simUs;

  printf("Cycle Tester host simulation (%d servos, speed %d deg/s, pulse range %d..%d us)\n",
         NUM_SERVOS, SERVO_SPEED_DEG_PER_SEC, SERVO_START_US[0], SERVO_END_US[0]);
  setup();
  runMs(300);

  scenarioBoot();
  if (noSave) scenarioNoSave();
  if (shortRun && SERVO_FULL_TRAVEL_DEG == 180) scenarioCalibration180();
  if (shortRun) scenarioShortRun();
  if (!quick) {
    scenarioSetupEditing();
    scenarioHoldToRepeat();
    scenarioRunAndPause();
    scenarioFinish();
    scenarioAbortFlow();
    scenarioCalibration();
    reportStalls();
  }

  printf("\n%d checks, %d failed\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
