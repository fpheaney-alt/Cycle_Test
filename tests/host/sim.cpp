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

// ---- simulated clock and shared mock state ----
uint64_t g_simUs = 0;
SerialMock Serial;
TwoWire Wire;
std::vector<ScriptedTouch> g_touchScript;
std::vector<uint64_t> g_glitchPolls;
int g_failBeginCount = 0;
unsigned long g_i2cReads = 0;

#include "Config.h"        // before the servo mock, as in the sketch: it sets the pulse limits the Servo library reads
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
static bool g_trimRun = false;       // "make testtrim": Config.h has non-zero fine-tuning numbers
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
  if (!g_trimRun)                                                   // (the trim variant has its own numbers, see scenarioTrim)
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

// Used by "make test180" and "make testtrim": start a test with the default targets and make sure the arms use exactly their configured pulse range.
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

// Used by "make testtrim": Config.h was edited to S1 trim +2.0, S2 trim -1.5, S3 trim +0.5 with 177.5 measured, S4 183.0 measured.
// The expected pulse widths below were worked out separately with exact arithmetic (270 degree servo, offset 45 degrees).
static void scenarioTrim() {
  SECTION("Fine tuning in degrees (SERVO_START_TRIM_DEG, SERVO_MEASURED_SWEEP_DEG)");
  static const int    expStart[4] = { 848, 822, 837, 833 };
  static const int    expEnd[4]   = { 2181, 2156, 2189, 2145 };
  static const double trim[4]     = { 2.0, -1.5, 0.5, 0.0 };
  static const double measured[4] = { 180.0, 180.0, 177.5, 183.0 };
  CHECK(SERVO_FULL_TRAVEL_DEG == 270 && SWEEP_START_OFFSET_DEG == 45, "this test is written for the 270 degree settings");
  const double usPerDeg = (double)(SERVO_PULSE_MAX_US - SERVO_PULSE_MIN_US) / SERVO_FULL_TRAVEL_DEG;
  for (int i = 0; i < NUM_SERVOS; i++) {
    CHECK(SERVO_START_US[i] == expStart[i] && SERVO_END_US[i] == expEnd[i],
          "S%d sweep is %d..%d us (expected %d..%d)", i + 1, SERVO_START_US[i], SERVO_END_US[i], expStart[i], expEnd[i]);
    // The arm turns `measured` degrees for the nominal 180 degree span, so its real scale is measured / (180 * usPerDeg).
    // The corrected sweep must come out as a true 180 degrees (to within the one-pulse-step rounding, 0.14 degrees).
    double turned = (SERVO_END_US[i] - SERVO_START_US[i]) * measured[i] / (180.0 * usPerDeg);
    CHECK(fabs(turned - 180.0) <= 0.15, "S%d turns %.2f degrees (wanted 180)", i + 1, turned);
    // The start moved by the trim (to within rounding to a whole microsecond).
    double moved = (SERVO_START_US[i] - (SERVO_PULSE_MIN_US + SWEEP_START_OFFSET_DEG * usPerDeg)) / usPerDeg;
    CHECK(fabs(moved - trim[i]) <= 0.08, "S%d start moved %.2f degrees (trim is %.2f)", i + 1, moved, trim[i]);
  }
  // Same measured value for two servos but different trims: the sweeps are the same size, only shifted.
  CHECK(abs((SERVO_END_US[0] - SERVO_START_US[0]) - (SERVO_END_US[1] - SERVO_START_US[1])) <= 1,
        "S1 and S2 (trim only) have sweeps of the same size (%d and %d us)", SERVO_END_US[0] - SERVO_START_US[0], SERVO_END_US[1] - SERVO_START_US[1]);
}

static void reportStalls() {
  SECTION("Responsiveness");
  printf("  longest single pass of loop(): %.0f ms on the run screen, %.0f ms on the setup screen\n", g_maxStallUs / 1000.0, g_maxStallUsSetup / 1000.0);
  printf("  switching screens (full redraw) takes up to %.1f s\n", g_maxTransitionUs / 1e6);
  printf("  (the servos are moved by the timer interrupt, so these pauses do not make the arms stutter)\n");
  printf("  touch controller I2C reads: %lu over %.0f s = %.0f per second\n", g_i2cReads, nowS(), g_i2cReads / nowS());
  CHECK(g_maxStallUs < 400000, "run-screen updates never block loop() for more than 0.4 s");
  CHECK(g_maxStallUsSetup < 400000, "setup-screen updates never block loop() for more than 0.4 s");
}

// ===========================================================================
int main(int argc, char** argv) {
  bool quick = false, shortRun = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--quick")) quick = true;
    if (!strcmp(argv[i], "--short")) { quick = true; shortRun = true; }
    if (!strcmp(argv[i], "--trim")) g_trimRun = true;
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
  if (g_trimRun) scenarioTrim();
  if (shortRun) scenarioShortRun();
  if (!quick) {
    scenarioSetupEditing();
    scenarioHoldToRepeat();
    scenarioRunAndPause();
    scenarioFinish();
    scenarioAbortFlow();
    reportStalls();
  }

  printf("\n%d checks, %d failed\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
