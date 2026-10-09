#include "SerialCommands.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

// Type a position in the Serial Monitor and the arm goes there, so you can try numbers without uploading.
// The numbers are the same ones that go in Config.h, and are turned into pulse widths by the same functions
// (startUsFor / endUsFor), so a position you tried is exactly what you get after pasting the numbers there.

namespace {

CycleController* cmdCtl = nullptr;
uint32_t cmdNow = 0;

float trimDeg[NUM_SERVOS];     // what has been typed so far (starts as the Config.h numbers)
float sweepDeg[NUM_SERVOS];

char    cmdLine[48];
uint8_t cmdLen = 0;
bool    cmdTooLong = false;

void servoName(uint8_t i) { Serial.print(F("S")); Serial.print(i + 1); Serial.print(F(": ")); }

void printHelp() {
  Serial.println(F("Position commands (only between tests). Servo is S1..S4 or ALL:"));
  Serial.println(F("  S1 -5.0         S1 to its START with a start trim of -5.0 degrees"));
  Serial.println(F("  S1 -5.0 end     the same trim, but go to the END of the sweep"));
  Serial.println(F("  S1 start | end  go to the start / end with the numbers set so far"));
  Serial.println(F("  S1 sweep 177.5  say the arm turned 177.5 degrees for 180, and go to the END"));
  Serial.println(F("  S1 off          stop sending a signal (the servo goes limp)"));
  Serial.println(F("  S1 reset        back to the Config.h numbers"));
  Serial.println(F("  show            print the numbers, ready to paste into Config.h"));
  Serial.println(F("The first command for a servo makes it jump there; after that it glides. A test started"));
  Serial.println(F("afterwards uses these numbers too, until power-off."));
}

void printFloat(float v) { Serial.print(v, 3); Serial.print('f'); }

void printEnds(uint8_t i) {
  Serial.print(F("start "));
  Serial.print(cmdCtl->channel(i).startUs());
  Serial.print(F(" us, end "));
  Serial.print(cmdCtl->channel(i).endUs());
  Serial.println(F(" us"));
}

void show() {
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    servoName(i);
    Serial.print(F("trim "));  printFloat(trimDeg[i]);
    Serial.print(F(", sweep ")); printFloat(sweepDeg[i]);
    Serial.print(F(" -> "));
    printEnds(i);
  }
  Serial.println(F("Paste these two lines into Config.h to keep them:"));
  Serial.print(F("constexpr float SERVO_START_TRIM_DEG[NUM_SERVOS]     = { "));
  for (uint8_t i = 0; i < NUM_SERVOS; i++) { printFloat(trimDeg[i]); Serial.print(i + 1 < NUM_SERVOS ? F(", ") : F(" };\n")); }
  Serial.print(F("constexpr float SERVO_MEASURED_SWEEP_DEG[NUM_SERVOS] = { "));
  for (uint8_t i = 0; i < NUM_SERVOS; i++) { printFloat(sweepDeg[i]); Serial.print(i + 1 < NUM_SERVOS ? F(", ") : F(" };\n")); }
}

// "-5.0", "-5", "-5.0f" (as written in Config.h) are all fine. Anything else, or an absurd value, is refused.
bool parseNumber(const char* t, float& out) {
  char* end;
  double v = strtod(t, &end);
  if (end == t) return false;
  if (*end == 'f') end++;
  if (*end != 0) return false;
  if (!(fabs(v) <= 1000.0)) return false;           // also catches nan and inf
  out = (float)v;
  return true;
}

// -2 = not a servo, -1 = all, 0.. = that servo
int8_t parseSelector(const char* t) {
  if (!strcmp(t, "all")) return -1;
  if (t[0] == 's' && t[1] >= '1' && t[1] < '1' + NUM_SERVOS && t[2] == 0) return (int8_t)(t[1] - '1');
  return -2;
}

bool moveTo(uint8_t i, int startUs, int endUs, bool toEnd) {
  if (!cmdCtl->manualMove(i, startUs, endUs, toEnd, cmdNow)) {
    servoName(i);
    Serial.println(F("busy (still returning after a test?). Try again in a few seconds."));
    return false;
  }
  return true;
}

void report(uint8_t i, bool toEnd) {
  servoName(i);
  Serial.print(F("trim ")); printFloat(trimDeg[i]);
  Serial.print(F(", sweep ")); printFloat(sweepDeg[i]);
  Serial.print(F(" -> "));
  printEnds(i);
  servoName(i);
  Serial.println(toEnd ? F("holding the END position") : F("holding the START position"));
}

// Try a trim and measured sweep. Refused, with nothing changed, if a pulse would leave the servo's range.
void tryValues(uint8_t i, float trim, float sweep, bool toEnd) {
  if (!measuredPlausible(sweep)) {
    servoName(i);
    Serial.println(F("refused: the measured sweep must be between 90 and 270 degrees"));
    return;
  }
  int s = startUsFor(trim), e = endUsFor(trim, sweep);
  if (!pulseInLimits(s) || !pulseInLimits(e)) {
    servoName(i);
    Serial.print(F("refused: start would be ")); Serial.print(s);
    Serial.print(F(" us and end ")); Serial.print(e);
    Serial.print(F(" us, but only ")); Serial.print(SERVO_PULSE_MIN_US);
    Serial.print(F("..")); Serial.print(SERVO_PULSE_MAX_US);
    Serial.println(F(" us is allowed"));
    return;
  }
  if (!moveTo(i, s, e, toEnd)) return;
  trimDeg[i] = trim;
  sweepDeg[i] = sweep;
  report(i, toEnd);
}

void handleLine(char* text) {
  for (char* p = text; *p; ++p) *p = (char)tolower((unsigned char)*p);
  char* tok[4];
  uint8_t n = 0;
  for (char* t = strtok(text, " \t"); t && n < 4; t = strtok(nullptr, " \t")) tok[n++] = t;
  if (n == 0) return;

  if (!strcmp(tok[0], "help") || tok[0][0] == '?') { printHelp(); return; }
  if (!strcmp(tok[0], "show")) { show(); return; }

  int8_t sel = parseSelector(tok[0]);
  if (sel == -2 || n < 2 || n > 3) { Serial.println(F("Did not understand that. Type  help")); return; }
  if (cmdCtl->testActive()) {
    Serial.println(F("A test is open on the touchscreen. Tap SETUP (or NEW TEST) first, then try again."));
    return;
  }

  const char* word = tok[1];
  const char* extra = (n == 3) ? tok[2] : nullptr;
  float value = 0;
  enum { START, END, OFF, RESET, TRIM, SWEEP } what;
  if      (!strcmp(word, "start")) what = START;
  else if (!strcmp(word, "end"))   what = END;
  else if (!strcmp(word, "off"))   what = OFF;
  else if (!strcmp(word, "reset")) what = RESET;
  else if (!strcmp(word, "sweep")) {
    what = SWEEP;
    if (!extra || !parseNumber(extra, value)) { Serial.println(F("usage:  S1 sweep 177.5")); return; }
    extra = nullptr;
  } else if (parseNumber(word, value)) {
    what = TRIM;
    if (extra && strcmp(extra, "end")) { Serial.println(F("usage:  S1 -5.0   or   S1 -5.0 end")); return; }
  } else { Serial.println(F("Did not understand that. Type  help")); return; }
  if (extra && what != TRIM) { Serial.println(F("Did not understand that. Type  help")); return; }

  uint8_t first = (sel < 0) ? 0 : (uint8_t)sel;
  uint8_t last  = (sel < 0) ? (uint8_t)(NUM_SERVOS - 1) : (uint8_t)sel;
  for (uint8_t i = first; i <= last; i++) {
    switch (what) {
      case TRIM:  tryValues(i, value, sweepDeg[i], extra != nullptr); break;
      case SWEEP: tryValues(i, trimDeg[i], value, true); break;
      case START:
      case END:
        if (moveTo(i, cmdCtl->channel(i).startUs(), cmdCtl->channel(i).endUs(), what == END)) report(i, what == END);
        break;
      case OFF:
        cmdCtl->manualOff(i);
        servoName(i);
        Serial.println(F("off (no signal)"));
        break;
      case RESET:
        if (moveTo(i, SERVO_START_US[i], SERVO_END_US[i], false)) {
          trimDeg[i] = SERVO_START_TRIM_DEG[i];
          sweepDeg[i] = SERVO_MEASURED_SWEEP_DEG[i];
          report(i, false);
        }
        break;
    }
  }
}

}  // namespace

void SerialCommands::begin() {
  for (uint8_t i = 0; i < NUM_SERVOS; i++) { trimDeg[i] = SERVO_START_TRIM_DEG[i]; sweepDeg[i] = SERVO_MEASURED_SWEEP_DEG[i]; }
  Serial.println(F("Type  help  for position commands (e.g.  S1 -5.0)"));
}

void SerialCommands::update(CycleController& controller, uint32_t now) {
  cmdCtl = &controller;
  cmdNow = now;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (cmdTooLong) Serial.println(F("That line was too long. Type  help"));
      else if (cmdLen > 0) { cmdLine[cmdLen] = 0; handleLine(cmdLine); }
      cmdLen = 0;
      cmdTooLong = false;
    } else if (cmdLen < sizeof(cmdLine) - 1) {
      cmdLine[cmdLen++] = c;
    } else {
      cmdTooLong = true;
    }
  }
}
