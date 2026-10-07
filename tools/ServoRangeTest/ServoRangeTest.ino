// ServoRangeTest - find out what your servos really do BEFORE running a cycle test.
//
// The ANNIMOS 35KG servos are listed by sellers as either 180 or 270 degree models, both over
// 500..2500 us pulses. This sketch lets you send exact pulse widths from the Serial Monitor
// and watch the arm, so you can measure the real travel and find the safe ends.
//
//   Serial Monitor: 115200 baud, line ending "Newline".
//   Same servo pins as CycleTester. Power the servos from their own 7.4 V supply (common ground).
//
// Commands:
//   <n> <us>      move servo n (1-4) to a pulse width, e.g.  1 1500
//   all <us>      move all four servos, e.g.  all 1500
//   <n> +10       nudge servo n by +10 us (use -10 to go back; n may be "all")
//   off <n|all>   stop sending a signal (the servo goes limp)
//   show          print the current pulse widths
//   help
//
// Moves are slowed to about 30 degrees per second so nothing slams into an end stop.
#include <Servo.h>

const uint8_t NUM_SERVOS = 4;
const uint8_t PINS[NUM_SERVOS] = { 5, 6, 3, 4 };   // SERVO1..SERVO4 pins, as in Config.h of CycleTester

const int ABS_MIN_US = 400;            // never send anything outside this range
const int ABS_MAX_US = 2600;
const int SLEW_US_PER_SEC = 330;       // 330 us per second is roughly 30 degrees per second
const unsigned long STEP_MS = 10;

Servo servos[NUM_SERVOS];
bool  active[NUM_SERVOS];
float currentUs[NUM_SERVOS];
int   targetUs[NUM_SERVOS];

void printHelp() {
  Serial.println(F("Commands:  <n> <us> | all <us> | <n> +10 | <n> -10 | off <n|all> | show | help"));
  Serial.println(F("Start with  all 1500  (centre). Then step toward 500 and toward 2500 and watch the arm."));
  Serial.println(F("Stop pushing as soon as the arm stops moving, or the servo hums or strains, and back off ~20 us."));
}

void printStatus(int i) {
  if (!active[i]) { Serial.print(F("S")); Serial.print(i + 1); Serial.println(F(": off")); return; }
  Serial.print(F("S"));
  Serial.print(i + 1);
  Serial.print(F(": target "));
  Serial.print(targetUs[i]);
  Serial.print(F(" us   (if this servo is 180 deg: "));
  Serial.print((targetUs[i] - 500) * 180L / 2000L);
  Serial.print(F(" deg from the 500 us end; if 270 deg: "));
  Serial.print((targetUs[i] - 500) * 270L / 2000L);
  Serial.println(F(" deg)"));
}

void setTarget(int i, int us) {
  us = constrain(us, ABS_MIN_US, ABS_MAX_US);
  if (!active[i]) {
    servos[i].attach(PINS[i], ABS_MIN_US, ABS_MAX_US);
    servos[i].writeMicroseconds(us);          // first command: the arm jumps to this position at full speed
    currentUs[i] = us;
    active[i] = true;
  }
  targetUs[i] = us;
  printStatus(i);
}

void stopServo(int i) {
  if (active[i]) { servos[i].detach(); active[i] = false; }
  printStatus(i);
}

// Parse "<n|all>" into a first/last servo index. Returns false if it is not a valid servo selector.
bool parseSelector(const char* token, int& first, int& last) {
  if (!strcmp(token, "all")) { first = 0; last = NUM_SERVOS - 1; return true; }
  int n = atoi(token);
  if (n >= 1 && n <= NUM_SERVOS) { first = last = n - 1; return true; }
  return false;
}

void handleLine(char* line) {
  char* a = strtok(line, " \t\r");
  if (!a) return;
  if (!strcmp(a, "help") || a[0] == '?') { printHelp(); return; }
  if (!strcmp(a, "show")) { for (int i = 0; i < NUM_SERVOS; i++) printStatus(i); return; }

  int first, last;
  if (!strcmp(a, "off")) {
    char* b = strtok(NULL, " \t\r");
    if (b && parseSelector(b, first, last)) { for (int i = first; i <= last; i++) stopServo(i); return; }
    Serial.println(F("usage: off <1-4|all>"));
    return;
  }

  char* b = strtok(NULL, " \t\r");
  if (parseSelector(a, first, last) && b) {
    bool relative = (b[0] == '+' || b[0] == '-');
    int value = atoi(b);
    for (int i = first; i <= last; i++) {
      if (relative && !active[i]) { Serial.print(F("S")); Serial.print(i + 1); Serial.println(F(": send an absolute value first, e.g.  1 1500")); continue; }
      setTarget(i, relative ? targetUs[i] + value : value);
    }
    return;
  }
  Serial.println(F("Did not understand that. Type  help"));
}

void setup() {
  Serial.begin(115200);
  Serial.println(F("ServoRangeTest ready."));
  printHelp();
}

void loop() {
  // collect one line of text
  static char buf[32];
  static uint8_t len = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') { buf[len] = 0; handleLine(buf); len = 0; }
    else if (len < sizeof(buf) - 1) buf[len++] = c;
  }

  // move every active servo a small step toward its target
  static unsigned long last = 0;
  if (millis() - last >= STEP_MS) {
    last = millis();
    float step = SLEW_US_PER_SEC * (STEP_MS / 1000.0f);
    for (int i = 0; i < NUM_SERVOS; i++) {
      if (!active[i]) continue;
      if (currentUs[i] < targetUs[i])      currentUs[i] = min(currentUs[i] + step, (float)targetUs[i]);
      else if (currentUs[i] > targetUs[i]) currentUs[i] = max(currentUs[i] - step, (float)targetUs[i]);
      servos[i].writeMicroseconds((int)(currentUs[i] + 0.5f));
    }
  }
}
