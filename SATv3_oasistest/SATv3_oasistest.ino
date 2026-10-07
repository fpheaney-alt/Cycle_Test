#include <LCDWIKI_SPI.h>
#include <lcd_spi_registers.h>
#include <mcu_spi_magic.h>

#include <Arduino.h>
#include <Wire.h>
#include <ServoEasing.hpp>

#include <LCDWIKI_GUI.h>
#include <LCDWIKI_SPI.h>
#include <Adafruit_FT6206.h>

#define MODEL    ST7796S
#define LCD_CS   8
#define LCD_DC    9
#define LCD_RST   7
#define LCD_LED  -1
LCDWIKI_SPI gfx(MODEL, LCD_CS, LCD_DC, LCD_RST, LCD_LED);

#define CTP_RST 6
const int TOUCH_INT_PIN = 2;
Adafruit_FT6206 ctp;

// ----------------------- SCREEN PINS --------------------
// IF YOU GET THE ERROR MESSAGE THAT THE TOUCH CONTROLLER IS NOT FOUND, MAKE SURE THAT THE CTP_INT WIRE IS FULLY INSERTED INTO THE ARDUINO.
//VCC - 5V
//GND - GND
//LCD_CS - 8
//LCD_RST - 7
//LCD_RS - 9
//SDI(MOSI) - 51
//SCK - 52
//LED - 5V
//SDO(MOSI) - 50
//CTP_SCL - SCL
//CTP_RST - 3.3v
//CTP_SDA - SDA
//CTP_INT - 2

// ----------------------- SERVO SETUP --------------------
//SERVO ANGLE TEST - SERIAL MONITOR AT 9600 "S1 45" WILL MOVE SERVO 1 TO CORRESPONDING POSITION (CASE SENSITIVE).
const int SERVO1_PIN = 5;
const int SERVO2_PIN = 6;
const int SERVO3_PIN = 3;  // Servo 3 pin
const int SERVO4_PIN = 4;  // Servo 4 pin
ServoEasing s1, s2, s3, s4;
// Use these exactly as currently defined in your fixture
const int S1_OPEN_ANGLE   = 30;
const int S1_CLOSED_ANGLE = 60;
const int S2_OPEN_ANGLE   = 10;
const int S2_CLOSED_ANGLE = 40;
// Initial defaults for servos 3 & 4; adjust as needed
const int S3_OPEN_ANGLE   = 0;
const int S3_CLOSED_ANGLE = 180;
const int S4_OPEN_ANGLE   = 10;
const int S4_CLOSED_ANGLE = 40;

// Global servo speed. ONE value drives all four servos (average deg/sec over a move).
// The user picks one of these presets on the Set Target screen.
const int SPEED_PRESET_COUNT = 3;
const int SPEED_PRESET_DPS[SPEED_PRESET_COUNT]          = {40, 80, 120};
const char* const SPEED_PRESET_NAME[SPEED_PRESET_COUNT] = {"Slow", "Medium", "Fast"};
const int DEFAULT_SPEED_PRESET = 1;   // Medium = 80 deg/sec, same as before
int speedPreset   = DEFAULT_SPEED_PRESET;
int servoSpeedDps = SPEED_PRESET_DPS[DEFAULT_SPEED_PRESET];

// Easing curve used for every move. In/out easing ramps the speed up and down
// instead of starting and stopping at full speed, which is much gentler on the
// servos and the fixture. The preset speed above is the AVERAGE speed of a move
// (peak is about 1.6x with sine). Use EASE_LINEAR to get the old constant-speed
// moves back, or EASE_QUADRATIC_IN_OUT for a slightly cheaper curve.
const uint_fast8_t SERVO_EASING = EASE_SINE_IN_OUT;

// Global dwell at each end of travel for ALL servos (ms)
const unsigned long SERVO_DWELL_MS = 1000;  // adjust to taste

// How often the touch controller is polled over I2C (ms). Polling it flat out keeps
// the I2C interrupt busy and delays the servo timer interrupt a few microseconds at
// random, which shows up as servo jitter. 25 ms is still instant to the finger.
const unsigned long TOUCH_POLL_MS = 25;

// ---------------------------------------------------------------------------
// Servo attach helpers
// ---------------------------------------------------------------------------
// ServoEasing::attach() always claims a NEW slot in its internal servo table and
// never checks whether the servo is already attached. Calling it on an attached
// servo leaks a slot (and the update interrupt then services the servo twice), and
// after enough pause/resume taps the table is full and servos stop responding.
// So: only ever attach a servo that is not currently attached.

// Make sure the servo is attached (parked at parkAngle if it had to be attached),
// with the current global speed and easing curve.
void ensureAttached(ServoEasing &servo, int pin, int parkAngle) {
  if (!servo.attached()) {
    servo.attach(pin, parkAngle);   // attach and write the start angle in one call
  }
  servo.setSpeed(servoSpeedDps);
  servo.setEasingType(SERVO_EASING);
}

// Attach if needed, cancel any move in progress, and put the servo at angle.
// Cancelling first matters: otherwise the update interrupt keeps easing toward the
// old target and drags the servo back out of position.
void parkServo(ServoEasing &servo, int pin, int angle) {
  ensureAttached(servo, pin, angle);
  servo.stop();
  servo.write(angle);
}

const int SCREEN_W = 480;
const int SCREEN_H = 320;
const int BTN_W = 160, BTN_H = 48;
const int BTN_X = (SCREEN_W - BTN_W) / 2;
const int BTN_Y = SCREEN_H - BTN_H - 12;

// Servo label button geometry
const int SERVO_LABEL_X = 10;
const int SERVO_LABEL_W = 200;
const int SERVO_LABEL_H = 28;
const int SERVO1_LABEL_Y = 50;
const int SERVO2_LABEL_Y = 105;
const int SERVO3_LABEL_Y = 160;
const int SERVO4_LABEL_Y = 215;

bool started = false;

#define C_BLACK   0x0000
#define C_WHITE   0xFFFF
#define C_DKGRAY  0x7BEF
#define C_CYAN    0x07FF
#define C_BLUE    0x001F
#define C_GREEN   0x07E0
#define C_RED     0xF800

enum Phase {
  IDLE_WAIT_START,
  S1_CLOSING,
  S1_OPENING,
  S2_CLOSING,
  S2_OPENING
};
Phase phase = IDLE_WAIT_START;

// Screen states: first a set-target screen, then the run/cycling screen
enum ScreenMode {
  SCREEN_SET_TARGET,
  SCREEN_RUN
};
ScreenMode currentScreen = SCREEN_SET_TARGET;

volatile uint32_t count1 = 0;
volatile uint32_t count2 = 0;
volatile uint32_t count3 = 0;
volatile uint32_t count4 = 0;

// Target cycles for this test and completed full cycles
// One "cycle" = pair (1&3) + pair (2&4)
uint32_t targetCycles    = 0;   // user input (0 means none / unused)
uint32_t completedCycles = 0;   // full 4-servo cycles completed
bool     infiniteMode    = false; // true = run indefinitely

// Global pause state (for the whole machine)
bool paused = false;

// Per-servo pause flags (true = that servo is paused and held open)
bool servo1Paused = false;
bool servo2Paused = false;
bool servo3Paused = false;
bool servo4Paused = false;

unsigned long moveStartTime = 0;
unsigned long moveDuration  = 0;

// ---------------------------------------------------------------------------
// UI helpers
// ---------------------------------------------------------------------------

// Forward declarations for UI helpers that are referenced before definition
void updateMainButton();
void drawServoLabel(uint8_t index);

void drawHeader() {
  // Clear header band
  gfx.Fill_Rect(0, 0, SCREEN_W, 32, C_BLACK);
  gfx.Set_Text_Mode(0);
  gfx.Set_Text_Back_colour(C_BLACK);

  // Left: title
  gfx.Set_Text_Size(2);
  gfx.Set_Text_colour(C_WHITE);
  gfx.Print_String("Snap & Test V2", 10, 8);

  // Right: target info for this run
  // (Shown only on the run screen; set by target screen beforehand.)
  gfx.Set_Text_Size(2);
  if (infiniteMode) {
    gfx.Set_Text_colour(C_GREEN);
    gfx.Print_String("Target: INF", 260, 8);
  } else {
    gfx.Set_Text_colour(C_CYAN);
    char buf[24];
    sprintf(buf, "Target: %lu", (unsigned long)targetCycles);
    gfx.Print_String(buf, 260, 8);
  }

  // Bottom divider line
  gfx.Set_Draw_color(C_DKGRAY);
  gfx.Draw_Fast_HLine(0, 30, SCREEN_W);
}

void updateCount1Display() {
  // Only clear and redraw the numeric area for Servo 1
  gfx.Set_Text_Size(2);
  gfx.Set_Text_Back_colour(C_BLACK);
  gfx.Set_Text_colour(C_CYAN);
  gfx.Fill_Rect(220, 40, 140, 24, C_BLACK); // small box around number only
  gfx.Print_Number_Int((long)count1, 220, 50, 10, ' ', 10);
}

void updateCount2Display() {
  // Only clear and redraw the numeric area for Servo 2
  gfx.Set_Text_Size(2);
  gfx.Set_Text_Back_colour(C_BLACK);
  gfx.Set_Text_colour(C_CYAN);
  gfx.Fill_Rect(220, 95, 140, 24, C_BLACK); // small box around number only
  gfx.Print_Number_Int((long)count2, 220, 105, 10, ' ', 10);
}

void updateCount3Display() {
  // Only clear and redraw the numeric area for Servo 3
  gfx.Set_Text_Size(2);
  gfx.Set_Text_Back_colour(C_BLACK);
  gfx.Set_Text_colour(C_CYAN);
  gfx.Fill_Rect(220, 150, 140, 24, C_BLACK); // small box around number only
  gfx.Print_Number_Int((long)count3, 220, 160, 10, ' ', 10);
}
void updateCount4Display() {
  // Only clear and redraw the numeric area for Servo 4
  gfx.Set_Text_Size(2);
  gfx.Set_Text_Back_colour(C_BLACK);
  gfx.Set_Text_colour(C_CYAN);
  gfx.Fill_Rect(220, 205, 140, 24, C_BLACK); // small box around number only
  gfx.Print_Number_Int((long)count4, 220, 215, 10, ' ', 10);
}

void drawCounters() {
  // One-time draw of labels and initial numbers. This is called
  // from drawUI(). During runtime we ONLY call the per-servo
  // update*Display() functions so labels never flicker.
  gfx.Fill_Rect(0, 40, SCREEN_W, 240, C_BLACK);

  gfx.Set_Text_Size(2);
  gfx.Set_Text_Back_colour(C_BLACK);

  // Servo label "buttons" (tap to pause/resume each servo)
  drawServoLabel(1);
  updateCount1Display();

  drawServoLabel(2);
  updateCount2Display();

  drawServoLabel(3);
  updateCount3Display();

  drawServoLabel(4);
  updateCount4Display();
}

void drawButton(const char* label, uint16_t fill, uint16_t outline, uint16_t text) {
  int x1 = BTN_X, y1 = BTN_Y;
  int x2 = BTN_X + BTN_W - 1;
  int y2 = BTN_Y + BTN_H - 1;
  gfx.Set_Draw_color(fill);
  gfx.Fill_Round_Rectangle(x1, y1, x2, y2, 10);
  gfx.Set_Draw_color(outline);
  gfx.Draw_Round_Rectangle(x1, y1, x2, y2, 10);
  gfx.Set_Text_colour(text);
  gfx.Set_Text_Back_colour(fill);
  gfx.Set_Text_Size(2);
  gfx.Print_String(label, BTN_X + 20, BTN_Y + BTN_H/2 - 8);
}

// Update the main Start/Pause/Resume button based on current state
void updateMainButton() {
  const char* label;
  uint16_t fill;
  if (!started) {
    label = "Start";
    fill  = C_BLUE;
  } else if (paused) {
    label = "Resume";
    fill  = C_GREEN;
  } else {
    label = "Pause";
    fill  = C_GREEN;
  }
  drawButton(label, fill, C_WHITE, C_WHITE);
}

// Draw a blue label button for each servo, showing cycles or paused state
void drawServoLabel(uint8_t index) {
  int y;
  bool isPaused = false;
  switch (index) {
    case 1: y = SERVO1_LABEL_Y; isPaused = servo1Paused; break;
    case 2: y = SERVO2_LABEL_Y; isPaused = servo2Paused; break;
    case 3: y = SERVO3_LABEL_Y; isPaused = servo3Paused; break;
    case 4: y = SERVO4_LABEL_Y; isPaused = servo4Paused; break;
    default: return;
  }

  uint16_t fill = C_BLUE;
  uint16_t outline = C_WHITE;
  gfx.Set_Draw_color(fill);
  gfx.Fill_Round_Rectangle(SERVO_LABEL_X, y - 4,
                           SERVO_LABEL_X + SERVO_LABEL_W,
                           y + SERVO_LABEL_H - 4, 5);
  gfx.Set_Draw_color(outline);
  gfx.Draw_Round_Rectangle(SERVO_LABEL_X, y - 4,
                           SERVO_LABEL_X + SERVO_LABEL_W,
                           y + SERVO_LABEL_H - 4, 5);

  gfx.Set_Text_Back_colour(fill);
  gfx.Set_Text_colour(C_WHITE);
  gfx.Set_Text_Size(2);

  char buf[24];
  if (isPaused) {
    sprintf(buf, "Servo %d Paused", index);
  } else {
    sprintf(buf, "Servo %d cycles:", index);
  }
  gfx.Print_String(buf, SERVO_LABEL_X + 4, y);
}

// Show the selected speed to the left of the Start/Pause button
void drawSpeedInfo() {
  gfx.Set_Text_Size(2);
  gfx.Set_Text_Back_colour(C_BLACK);
  gfx.Set_Text_colour(C_CYAN);
  char buf[20];
  sprintf(buf, "%s %d/s", SPEED_PRESET_NAME[speedPreset], servoSpeedDps);
  gfx.Print_String(buf, 10, BTN_Y + BTN_H / 2 - 8);
}

void drawUI() {
  // Main cycling screen
  gfx.Fill_Screen(C_BLACK);
  drawHeader();
  drawCounters();
  drawSpeedInfo();
  updateMainButton();
}

// ---------------- SET TARGET SCREEN -----------------

const int SET_BTN_W  = 140;
const int SET_BTN_H  = 50;
const int SET_MARGIN = 20;

// Button positions on the set-target screen
const int SET_MINUS_X = SET_MARGIN;
const int SET_MINUS_Y = 140;
const int SET_PLUS_X  = SCREEN_W - SET_MARGIN - SET_BTN_W;
const int SET_PLUS_Y  = 140;
const int SET_INDEF_X = SET_MARGIN;
const int SET_INDEF_Y = 210;
const int SET_OK_X    = SCREEN_W - SET_MARGIN - SET_BTN_W;
const int SET_OK_Y    = 210;

// Speed preset row (Slow / Medium / Fast) along the bottom of the screen
const int SET_SPEED_Y   = 270;
const int SET_SPEED_H   = 40;
const int SET_SPEED_GAP = (SCREEN_W - 2 * SET_MARGIN - SPEED_PRESET_COUNT * SET_BTN_W) / (SPEED_PRESET_COUNT - 1);

int speedButtonX(int i) { return SET_MARGIN + i * (SET_BTN_W + SET_SPEED_GAP); }

// Forward declarations for partial updates on the Set Target screen
void updateTargetText();
void updateIndefButton();
void updateSpeedButtons();

void drawSetTargetScreen() {
  gfx.Fill_Screen(C_BLACK);

  // Title
  gfx.Set_Text_Size(3);
  gfx.Set_Text_Back_colour(C_BLACK);
  gfx.Set_Text_colour(C_WHITE);
  gfx.Print_String("Set Target", 140, 40);

  // Current target / mode (drawn via helper so we can update it without
  // clearing the whole screen on every button press)
  updateTargetText();

  // -1000 button
  gfx.Set_Draw_color(C_DKGRAY);
  gfx.Fill_Round_Rectangle(SET_MINUS_X, SET_MINUS_Y,
                           SET_MINUS_X + SET_BTN_W, SET_MINUS_Y + SET_BTN_H, 8);
  gfx.Set_Text_Back_colour(C_DKGRAY);
  gfx.Set_Text_colour(C_WHITE);
  gfx.Set_Text_Size(2);
  gfx.Print_String("- 1000", SET_MINUS_X + 20, SET_MINUS_Y + 16);

  // +1000 button
  gfx.Fill_Round_Rectangle(SET_PLUS_X, SET_PLUS_Y,
                           SET_PLUS_X + SET_BTN_W, SET_PLUS_Y + SET_BTN_H, 8);
  gfx.Set_Text_Back_colour(C_DKGRAY);
  gfx.Set_Text_colour(C_WHITE);
  gfx.Print_String("+ 1000", SET_PLUS_X + 20, SET_PLUS_Y + 16);

  // Indefinite button (drawn via helper so we can toggle highlight only)
  updateIndefButton();

  // Speed preset buttons (drawn via helper so we can move the highlight only)
  updateSpeedButtons();

  // OK / Continue button
  gfx.Set_Draw_color(C_BLUE);
  gfx.Fill_Round_Rectangle(SET_OK_X, SET_OK_Y,
                           SET_OK_X + SET_BTN_W, SET_OK_Y + SET_BTN_H, 8);
  gfx.Set_Text_Back_colour(C_BLUE);
  gfx.Set_Text_colour(C_WHITE);
  gfx.Print_String("Continue", SET_OK_X + 18, SET_OK_Y + 16);
}

// Only redraw the line that shows the current target or mode
void updateTargetText() {
  gfx.Set_Text_Size(2);
  gfx.Set_Text_Back_colour(C_BLACK);
  // Clear just the strip where the text lives
  gfx.Fill_Rect(80, 80, 360, 30, C_BLACK);

  if (infiniteMode) {
    gfx.Set_Text_colour(C_GREEN);
    gfx.Print_String("Mode: Run indefinitely", 100, 90);
  } else {
    gfx.Set_Text_colour(C_CYAN);
    char buf[32];
    sprintf(buf, "Target cycles: %lu", (unsigned long)targetCycles);
    gfx.Print_String(buf, 120, 90);
  }
}

// Only redraw the Indefinite button, preserving the rest of the screen
void updateIndefButton() {
  uint16_t indefFill = infiniteMode ? C_GREEN : C_DKGRAY;
  gfx.Set_Draw_color(indefFill);
  gfx.Fill_Round_Rectangle(SET_INDEF_X, SET_INDEF_Y,
                           SET_INDEF_X + SET_BTN_W, SET_INDEF_Y + SET_BTN_H, 8);
  gfx.Set_Text_Back_colour(indefFill);
  gfx.Set_Text_colour(C_WHITE);
  gfx.Set_Text_Size(2);
  gfx.Print_String("Indefinite", SET_INDEF_X + 12, SET_INDEF_Y + 16);
}

// Redraw the three speed preset buttons; the selected one is highlighted green.
void updateSpeedButtons() {
  gfx.Set_Text_Size(2);
  for (int i = 0; i < SPEED_PRESET_COUNT; i++) {
    uint16_t fill = (i == speedPreset) ? C_GREEN : C_DKGRAY;
    int x = speedButtonX(i);
    gfx.Set_Draw_color(fill);
    gfx.Fill_Round_Rectangle(x, SET_SPEED_Y, x + SET_BTN_W, SET_SPEED_Y + SET_SPEED_H, 8);
    gfx.Set_Text_Back_colour(fill);
    gfx.Set_Text_colour(C_WHITE);

    char buf[16];
    sprintf(buf, "%s %d", SPEED_PRESET_NAME[i], SPEED_PRESET_DPS[i]);
    int textW = (int)strlen(buf) * 12;   // text size 2 = 12 px per character
    gfx.Print_String(buf, x + (SET_BTN_W - textW) / 2, SET_SPEED_Y + (SET_SPEED_H - 16) / 2);
  }
}

// ---------------------------------------------------------------------------
// Generic helpers
// ---------------------------------------------------------------------------

unsigned long estimateServoMoveTime(int delta) {
  return (unsigned long)(1000L * delta / servoSpeedDps);
}

bool getTouch(int16_t &x, int16_t &y) {
  if (!ctp.touched()) return false;
  TS_Point p = ctp.getPoint();
  x = SCREEN_W - p.y;
  y = p.x;
  return (x >= 0 && y >= 0 && x < SCREEN_W && y < SCREEN_H);
}

void resetTouch() {
  pinMode(CTP_RST, OUTPUT);
  digitalWrite(CTP_RST, LOW);
  delay(20);
  digitalWrite(CTP_RST, HIGH);
  delay(100);
}

// ----------------- SERIAL SERVO TEST (S1 45 / S2 90) -----------------
//String command = Serial.readStringUntil('\n');

void handleSerialCommands() {
  if (Serial.available()) {
    // Read a line from Serial, terminated by newline
String command = Serial.readStringUntil('\n');

    command.trim();
    if (command.startsWith("S1 ")) {
      int angle = command.substring(3).toInt();
      ensureAttached(s1, SERVO1_PIN, S1_OPEN_ANGLE);             // attach only if needed
      s1.easeTo(constrain(angle, 0, 180));
    } else if (command.startsWith("S2 ")) {
      int angle = command.substring(3).toInt();
      ensureAttached(s2, SERVO2_PIN, S2_OPEN_ANGLE);             // attach only if needed
      s2.easeTo(constrain(angle, 0, 180));
    }
  }
}

// ---------------------------------------------------------------------------
// Touch handling
// ---------------------------------------------------------------------------

// Forward declarations for pause/resume helpers
void startCycleFromOpen();
void pauseAll();
void resumeAll();
void toggleServoPause(uint8_t index);

bool inRect(int16_t x, int16_t y, int16_t rx, int16_t ry, int16_t rw, int16_t rh) {
  return (x >= rx && x <= (rx + rw) && y >= ry && y <= (ry + rh));
}

void handleTouchSetTarget(int16_t tx, int16_t ty) {
  // -1000
  if (inRect(tx, ty, SET_MINUS_X, SET_MINUS_Y, SET_BTN_W, SET_BTN_H)) {
    infiniteMode = false;
    if (targetCycles >= 1000) {
      targetCycles -= 1000;
    } else {
      targetCycles = 0;
    }
    // Only update the changing elements, not the whole screen
    updateTargetText();
    updateIndefButton();
    return;
  }

  // +1000
  if (inRect(tx, ty, SET_PLUS_X, SET_PLUS_Y, SET_BTN_W, SET_BTN_H)) {
    infiniteMode = false;
    if (targetCycles <= 4000000000UL - 1000UL) {
      targetCycles += 1000;
    }
    updateTargetText();
    updateIndefButton();
    return;
  }

  // Indefinite
  if (inRect(tx, ty, SET_INDEF_X, SET_INDEF_Y, SET_BTN_W, SET_BTN_H)) {
    infiniteMode = true;
    targetCycles = 0;  // ensure consistent semantics
    updateTargetText();
    updateIndefButton();
    return;
  }

  // Speed presets: one global speed for all four servos
  for (int i = 0; i < SPEED_PRESET_COUNT; i++) {
    if (inRect(tx, ty, speedButtonX(i), SET_SPEED_Y, SET_BTN_W, SET_SPEED_H)) {
      speedPreset   = i;
      servoSpeedDps = SPEED_PRESET_DPS[i];
      updateSpeedButtons();
      return;
    }
  }

  // Continue -> go to main cycling screen (no motion yet)
  if (inRect(tx, ty, SET_OK_X, SET_OK_Y, SET_BTN_W, SET_BTN_H)) {
    currentScreen = SCREEN_RUN;
    started = false;
    completedCycles = 0;
    count1 = count2 = count3 = count4 = 0;
    drawUI();
    return;
  }
}

// Start a servo cycle from the "all open" baseline, respecting per-servo pauses
void startCycleFromOpen() {
  unsigned long d1 = 0, d3 = 0;

  // Pair (1 & 3) closing first, if either is active
  if (!servo1Paused) {
    s1.startEaseTo(S1_CLOSED_ANGLE, servoSpeedDps, START_UPDATE_BY_INTERRUPT);
    d1 = estimateServoMoveTime(abs(S1_CLOSED_ANGLE - S1_OPEN_ANGLE));
  }
  if (!servo3Paused) {
    s3.startEaseTo(S3_CLOSED_ANGLE, servoSpeedDps, START_UPDATE_BY_INTERRUPT);
    d3 = estimateServoMoveTime(abs(S3_CLOSED_ANGLE - S3_OPEN_ANGLE));
  }

  if (d1 == 0 && d3 == 0) {
    // No active servos in pair (1 & 3); start with pair (2 & 4) instead.
    unsigned long d2 = 0, d4 = 0;
    if (!servo2Paused) {
      s2.startEaseTo(S2_CLOSED_ANGLE, servoSpeedDps, START_UPDATE_BY_INTERRUPT);
      d2 = estimateServoMoveTime(abs(S2_CLOSED_ANGLE - S2_OPEN_ANGLE));
    }
    if (!servo4Paused) {
      s4.startEaseTo(S4_CLOSED_ANGLE, servoSpeedDps, START_UPDATE_BY_INTERRUPT);
      d4 = estimateServoMoveTime(abs(S4_CLOSED_ANGLE - S4_OPEN_ANGLE));
    }
    moveDuration = (d2 > d4) ? d2 : d4;
    phase = S2_CLOSING;
  } else {
    moveDuration = (d1 > d3) ? d1 : d3;
    phase = S1_CLOSING;
  }

  moveStartTime = millis();
}

void startNewTest() {
  // Start a new test; assumes target/infinite were configured
  started = true;
  paused  = false;
  completedCycles = 0;
  count1 = count2 = count3 = count4 = 0;
  updateCount1Display();
  updateCount2Display();
  updateCount3Display();
  updateCount4Display();
  updateMainButton();

  // Re-attach servos when we actually start cycling, using the selected speed.
  // parkServo() also syncs the logical and physical angle at OPEN before the first
  // eased move (the first-run overshoot fix). Servos that were individually paused
  // stay detached so they don't sit there buzzing.
  if (!servo1Paused) parkServo(s1, SERVO1_PIN, S1_OPEN_ANGLE);
  if (!servo2Paused) parkServo(s2, SERVO2_PIN, S2_OPEN_ANGLE);
  if (!servo3Paused) parkServo(s3, SERVO3_PIN, S3_OPEN_ANGLE);
  if (!servo4Paused) parkServo(s4, SERVO4_PIN, S4_OPEN_ANGLE);
  delay(20); // let the PWM settle for one frame

  // Begin the first cycle
  startCycleFromOpen();
}

void pauseAll() {
  // Global pause: bring all servos to OPEN and detach, but do not reset counts
  paused = true;

  // Move all servos to OPEN (even if individually paused) and detach
  parkServo(s1, SERVO1_PIN, S1_OPEN_ANGLE);
  parkServo(s2, SERVO2_PIN, S2_OPEN_ANGLE);
  parkServo(s3, SERVO3_PIN, S3_OPEN_ANGLE);
  parkServo(s4, SERVO4_PIN, S4_OPEN_ANGLE);
  delay(20);
  s1.detach();
  s2.detach();
  s3.detach();
  s4.detach();

  updateMainButton();
}

void resumeAll() {
  // Resume from global pause: ensure servos are at OPEN and restart cycle.
  // Individually paused servos stay detached.
  if (!servo1Paused) parkServo(s1, SERVO1_PIN, S1_OPEN_ANGLE);
  if (!servo2Paused) parkServo(s2, SERVO2_PIN, S2_OPEN_ANGLE);
  if (!servo3Paused) parkServo(s3, SERVO3_PIN, S3_OPEN_ANGLE);
  if (!servo4Paused) parkServo(s4, SERVO4_PIN, S4_OPEN_ANGLE);
  delay(20);

  paused = false;
  updateMainButton();

  // Continue cycling from the open baseline
  startCycleFromOpen();
}

void toggleServoPause(uint8_t index) {
  bool* flag = nullptr;
  ServoEasing* servo = nullptr;
  int pin = 0;
  int openAngle = 0;

  switch (index) {
    case 1: flag = &servo1Paused; servo = &s1; pin = SERVO1_PIN; openAngle = S1_OPEN_ANGLE; break;
    case 2: flag = &servo2Paused; servo = &s2; pin = SERVO2_PIN; openAngle = S2_OPEN_ANGLE; break;
    case 3: flag = &servo3Paused; servo = &s3; pin = SERVO3_PIN; openAngle = S3_OPEN_ANGLE; break;
    case 4: flag = &servo4Paused; servo = &s4; pin = SERVO4_PIN; openAngle = S4_OPEN_ANGLE; break;
    default: return;
  }

  if (!(*flag)) {
    // Pause this servo: move to OPEN and detach
    *flag = true;
    parkServo(*servo, pin, openAngle);
    delay(20);
    servo->detach();
  } else {
    // Resume this servo: ensure at OPEN and leave attached for next cycle.
    // If the test isn't running (not started yet, or globally paused) there is no
    // next cycle coming, so release it again instead of holding it energised.
    *flag = false;
    parkServo(*servo, pin, openAngle);
    delay(20);
    if (!started || paused) {
      servo->detach();
    }
  }

  // Redraw this servo's label to reflect paused/running state
  drawServoLabel(index);
}

void handleTouchRunScreen(int16_t tx, int16_t ty) {
  // Main Start/Pause/Resume button
  if (inRect(tx, ty, BTN_X, BTN_Y, BTN_W, BTN_H)) {
    if (!started) {
      // Start button on main cycling screen
      if (!infiniteMode && targetCycles == 0) {
        drawButton("Set target", C_RED, C_WHITE, C_WHITE);
        delay(500);
        updateMainButton();
        return;
      }
      startNewTest();
    } else {
      // Toggle global pause/resume
      if (!paused) {
        pauseAll();
      } else {
        resumeAll();
      }
    }
    return;
  }

  // Per-servo pause/resume buttons (tap the blue label area)
  if (inRect(tx, ty, SERVO_LABEL_X, SERVO1_LABEL_Y - 4, SERVO_LABEL_W, SERVO_LABEL_H)) {
    toggleServoPause(1);
    return;
  }
  if (inRect(tx, ty, SERVO_LABEL_X, SERVO2_LABEL_Y - 4, SERVO_LABEL_W, SERVO_LABEL_H)) {
    toggleServoPause(2);
    return;
  }
  if (inRect(tx, ty, SERVO_LABEL_X, SERVO3_LABEL_Y - 4, SERVO_LABEL_W, SERVO_LABEL_H)) {
    toggleServoPause(3);
    return;
  }
  if (inRect(tx, ty, SERVO_LABEL_X, SERVO4_LABEL_Y - 4, SERVO_LABEL_W, SERVO_LABEL_H)) {
    toggleServoPause(4);
    return;
  }
}

void handleTouch(int16_t tx, int16_t ty) {
  if (currentScreen == SCREEN_SET_TARGET) {
    handleTouchSetTarget(tx, ty);
  } else {
    handleTouchRunScreen(tx, ty);
  }
}

// ---------------------------------------------------------------------------
// Arduino setup / loop
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(9600);
  delay(100);

  // On power-up: move once to OPEN angles, then detach to avoid
  // jitter while waiting on user input.
  parkServo(s1, SERVO1_PIN, S1_OPEN_ANGLE);
  parkServo(s2, SERVO2_PIN, S2_OPEN_ANGLE);
  parkServo(s3, SERVO3_PIN, S3_OPEN_ANGLE);
  parkServo(s4, SERVO4_PIN, S4_OPEN_ANGLE);

  delay(800);            // allow them to fully reach and settle at OPEN
                         // before we detach, to reduce any bounce/twitch
  s1.detach();           // <--- anti-jitter before Start
  s2.detach();           // <--- anti-jitter before Start
  s3.detach();           // <--- anti-jitter before Start
  s4.detach();           // <--- anti-jitter before Start

  gfx.Init_LCD();
  gfx.Set_Rotation(3);
  pinMode(TOUCH_INT_PIN, INPUT_PULLUP);

  Wire.begin();
  resetTouch();
  if (!ctp.begin(40)) {
    gfx.Fill_Screen(C_BLACK);
    gfx.Set_Text_colour(C_WHITE);
    gfx.Set_Text_Back_colour(C_BLACK);
    gfx.Set_Text_Size(2);
    gfx.Print_String("Touch controller not found (0x38).", 10, 40);
    while (1) delay(10);
  }

  currentScreen = SCREEN_SET_TARGET;
  infiniteMode = false;
  targetCycles = 0;
  completedCycles = 0;
  drawSetTargetScreen();
}

void loop() {
  handleSerialCommands();

  unsigned long now = millis();

  // Poll the touch controller at a fixed rate instead of flat out (see TOUCH_POLL_MS)
  static unsigned long lastTouchPoll = 0;
  if (now - lastTouchPoll >= TOUCH_POLL_MS) {
    lastTouchPoll = now;
    int16_t tx, ty;
    if (getTouch(tx, ty)) {
      handleTouch(tx, ty);
    }
  }

  if (!started || paused) return;

  // Re-read the clock: handling a touch can take a while (screen redraws, delays),
  // and the state machine below compares against moveStartTime set during that time.
  now = millis();

  switch (phase) {
    case S1_CLOSING:
      // Pair (1 & 3) moving towards CLOSED. After motion + dwell, go back to OPEN.
      if (now - moveStartTime >= moveDuration + SERVO_DWELL_MS) {
        // Reverse direction for any unpaused servos in pair 1.
        unsigned long d1 = 0, d3 = 0;
        if (!servo1Paused) {
          s1.startEaseTo(S1_OPEN_ANGLE, servoSpeedDps, START_UPDATE_BY_INTERRUPT);
          d1 = estimateServoMoveTime(abs(S1_OPEN_ANGLE - S1_CLOSED_ANGLE));
        }
        if (!servo3Paused) {
          s3.startEaseTo(S3_OPEN_ANGLE, servoSpeedDps, START_UPDATE_BY_INTERRUPT);
          d3 = estimateServoMoveTime(abs(S3_OPEN_ANGLE - S3_CLOSED_ANGLE));
        }
        moveDuration = (d1 > d3) ? d1 : d3;
        moveStartTime = now;
        phase = S1_OPENING;
      }
      break;

    case S1_OPENING:
      // Pair (1 & 3) moving towards OPEN. Once motion is done, count a cycle
      // for any unpaused servo in pair 1 and then hand control to pair (2 & 4).
      if (now - moveStartTime >= moveDuration) {
        if (!servo1Paused) {
          count1++;
          updateCount1Display();
        }
        if (!servo3Paused) {
          count3++;
          updateCount3Display();
        }

        unsigned long d2 = 0, d4 = 0;
        if (!servo2Paused) {
          s2.startEaseTo(S2_CLOSED_ANGLE, servoSpeedDps, START_UPDATE_BY_INTERRUPT);
          d2 = estimateServoMoveTime(abs(S2_CLOSED_ANGLE - S2_OPEN_ANGLE));
        }
        if (!servo4Paused) {
          s4.startEaseTo(S4_CLOSED_ANGLE, servoSpeedDps, START_UPDATE_BY_INTERRUPT);
          d4 = estimateServoMoveTime(abs(S4_CLOSED_ANGLE - S4_OPEN_ANGLE));
        }
        moveDuration = (d2 > d4) ? d2 : d4;
        moveStartTime = now;
        phase = S2_CLOSING;
      }
      break;

    case S2_CLOSING:
      // Pair (2 & 4) moving towards CLOSED. After motion + dwell, go back to OPEN.
      if (now - moveStartTime >= moveDuration + SERVO_DWELL_MS) {
        unsigned long d2 = 0, d4 = 0;
        if (!servo2Paused) {
          s2.startEaseTo(S2_OPEN_ANGLE, servoSpeedDps, START_UPDATE_BY_INTERRUPT);
          d2 = estimateServoMoveTime(abs(S2_OPEN_ANGLE - S2_CLOSED_ANGLE));
        }
        if (!servo4Paused) {
          s4.startEaseTo(S4_OPEN_ANGLE, servoSpeedDps, START_UPDATE_BY_INTERRUPT);
          d4 = estimateServoMoveTime(abs(S4_OPEN_ANGLE - S4_CLOSED_ANGLE));
        }
        moveDuration = (d2 > d4) ? d2 : d4;
        moveStartTime = now;
        phase = S2_OPENING;
      }
      break;

    case S2_OPENING:
      // Pair (2 & 4) moving towards OPEN. Once motion is done, count a cycle
      // for any unpaused servo in pair 2 and then either stop (if target reached)
      // or hand control back to pair (1 & 3).
      if (now - moveStartTime >= moveDuration) {
        if (!servo2Paused) {
          count2++;
          updateCount2Display();
        }
        if (!servo4Paused) {
          count4++;
          updateCount4Display();
        }

        // One full 4-servo cycle (1&3 then 2&4) has completed.
        completedCycles++;

        if (!infiniteMode && targetCycles > 0 && completedCycles >= targetCycles) {
          // Target reached: stop the test and detach servos.
          s1.detach();
          s2.detach();
          s3.detach();
          s4.detach();
          started = false;
          phase = IDLE_WAIT_START;
          // Stay on the run screen with final counts visible
          drawUI();
        } else {
          unsigned long d1 = 0, d3 = 0;
          if (!servo1Paused) {
            s1.startEaseTo(S1_CLOSED_ANGLE, servoSpeedDps, START_UPDATE_BY_INTERRUPT);
            d1 = estimateServoMoveTime(abs(S1_CLOSED_ANGLE - S1_OPEN_ANGLE));
          }
          if (!servo3Paused) {
            s3.startEaseTo(S3_CLOSED_ANGLE, servoSpeedDps, START_UPDATE_BY_INTERRUPT);
            d3 = estimateServoMoveTime(abs(S3_CLOSED_ANGLE - S3_OPEN_ANGLE));
          }
          moveDuration = (d1 > d3) ? d1 : d3;
          moveStartTime = now;
          phase = S1_CLOSING;
        }
      }
      break;

    default:
      break;
  }
}
