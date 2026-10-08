#include <LCDWIKI_SPI.h>
#include <lcd_spi_registers.h>
#include <mcu_spi_magic.h>
#include <Wire.h>
#include <LCDWIKI_GUI.h>
#include <Adafruit_FT6206.h>

#include "Ui.h"
#include "Config.h"
#include "Font5x7.h"

// ===========================================================================
// HARDWARE
// ===========================================================================
static LCDWIKI_SPI gfx(ST7796S, LCD_CS_PIN, LCD_DC_PIN, LCD_RST_PIN, LCD_LED_PIN);
static Adafruit_FT6206 ctp;

// ===========================================================================
// LOOK AND LAYOUT - everything you would change to restyle the screens is in this section
// ===========================================================================
const int16_t SCREEN_W = 480;      // landscape, after Set_Rotation(3)
const int16_t SCREEN_H = 320;
const int16_t HEADER_H = 32;

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
const uint16_t COL_BG       = rgb565(8, 10, 18);
const uint16_t COL_PANEL    = rgb565(24, 30, 46);
const uint16_t COL_BORDER   = rgb565(70, 82, 112);
const uint16_t COL_BUTTON   = rgb565(48, 60, 92);
const uint16_t COL_TEXT     = rgb565(255, 255, 255);
const uint16_t COL_DIM      = rgb565(140, 150, 175);
const uint16_t COL_ACCENT   = rgb565(0, 200, 255);
const uint16_t COL_GOOD     = rgb565(40, 205, 95);
const uint16_t COL_WARN     = rgb565(255, 175, 0);
const uint16_t COL_BAD      = rgb565(235, 65, 65);
const uint16_t COL_ON_BRIGHT = rgb565(8, 10, 18);   // dark text drawn on the bright green / amber / red buttons

const int16_t BUTTON_RADIUS = 8;

// --- Setup screen ---
const int16_t SETUP_ROW_Y0    = 40;     // top of the first servo row
const int16_t SETUP_ROW_STEP  = 50;
const int16_t SETUP_ROW_H     = 44;
const int16_t SETUP_LABEL_X   = 8;
const int16_t SETUP_X0        = 42;     // left edge of the first (-1000) button
const int16_t SETUP_BIG_W     = 80;     // width of the -1000 / +1000 buttons
const int16_t SETUP_SMALL_W   = 66;     // width of the -100 / +100 buttons
const int16_t SETUP_VALUE_W   = 112;    // width of the number box
const int16_t SETUP_GAP       = 6;
const int16_t SETUP_EST_Y     = 243;    // line showing the estimated run time
const int16_t SETUP_BOTTOM_Y  = 262;
const int16_t SETUP_BOTTOM_H  = 50;

// --- Run screen ---
const int16_t RUN_ROW_Y0   = 38;
const int16_t RUN_ROW_STEP = 54;
const int16_t RUN_ROW_H    = 50;
const int16_t RUN_BOTTOM_Y = 258;
const int16_t RUN_BOTTOM_H = 54;

// --- Calibration screen ---
const int16_t CALSCR_TAB_Y    = 34;     // servo tabs S1..S4
const int16_t CALSCR_TAB_H    = 32;
const int16_t CALSCR_TAB_W    = 108;
const int16_t CALSCR_TAB_GAP  = 6;
const int16_t CALSCR_ROW_Y0   = 70;     // the START row; the END row is one step below
const int16_t CALSCR_ROW_STEP = 46;
const int16_t CALSCR_ROW_H    = 42;
const int16_t CALSCR_BOX_X    = 4;      // the value box at the left of each row
const int16_t CALSCR_BOX_W    = 118;
const int16_t CALSCR_JOG_X0   = 126;    // the six jog buttons
const int16_t CALSCR_JOG_W    = 55;
const int16_t CALSCR_JOG_GAP  = 3;
const int16_t CALSCR_INFO_Y   = 162;    // three lines of text under the rows
const int16_t CALSCR_INFO_STEP = 18;
const int16_t CALSCR_BTN1_Y   = 220;    // GO START / GO END / DEFAULT
const int16_t CALSCR_BTN1_H   = 36;
const int16_t CALSCR_BTN2_Y   = 262;    // SAVE / BACK
const int16_t CALSCR_BTN2_H   = 48;

// ===========================================================================
// SMALL DRAWING TOOLKIT
// ===========================================================================
// Text is drawn by the few lines below instead of the display library's Print_String(). Reasons:
// it only needs Fill_Rect(); it makes no temporary String objects (no heap use over a multi-day
// run); and it paints roughly 4x fewer rectangles per character, so redraws stall the main loop less.

struct Rect { int16_t x, y, w, h; };

static void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  if (w > 0 && h > 0) gfx.Fill_Rect(x, y, w, h, color);
}

// One character cell is 6x8 "font pixels" (5x7 glyph plus spacing), each font pixel scale x scale screen pixels.
static void drawGlyph(int16_t x, int16_t y, char c, uint8_t scale, uint16_t fg, uint16_t bg) {
  fillRect(x, y, 6 * scale, 8 * scale, bg);
  if (c <= ' ' || (uint8_t)c > FONT_LAST_CHAR) return;
  const uint8_t* glyph = FONT5X7 + (uint8_t)(c - FONT_FIRST_CHAR) * 5;
  for (uint8_t col = 0; col < 5; col++) {
    uint8_t bits = pgm_read_byte(glyph + col);
    uint8_t row = 0;
    while (row < 8) {                                         // 8 rows: the tails of g, j, p, q, y and the comma use the last one
      if (bits & (1 << row)) {
        uint8_t first = row;
        while (row < 8 && (bits & (1 << row))) row++;         // merge a vertical run into one rectangle
        fillRect(x + col * scale, y + first * scale, scale, (row - first) * scale, fg);
      } else {
        row++;
      }
    }
  }
}

static void drawText(int16_t x, int16_t y, const char* s, uint8_t scale, uint16_t fg, uint16_t bg) {
  for (; *s; s++, x += 6 * scale) drawGlyph(x, y, *s, scale, fg, bg);
}

static int16_t textWidth(const char* s, uint8_t scale) { return (int16_t)strlen(s) * 6 * scale - scale; }

static void drawTextCentered(const Rect& r, const char* s, uint8_t scale, uint16_t fg, uint16_t bg) {
  int16_t x = r.x + (r.w - textWidth(s, scale)) / 2;
  int16_t y = r.y + (r.h - 7 * scale) / 2;
  drawText(x, y, s, scale, fg, bg);
}

// A piece of text that remembers what is on screen, and repaints only the characters that changed.
// Updating a counter from 1234 to 1235 repaints one digit instead of the whole number.
struct Field { char shown[32]; uint16_t fg; };

static void resetField(Field& f) { memset(f.shown, 0, sizeof(f.shown)); f.fg = 0; }

enum Align : uint8_t { ALIGN_LEFT, ALIGN_RIGHT, ALIGN_CENTER };

static void drawField(Field& f, int16_t x, int16_t y, uint8_t scale, uint16_t fg, uint16_t bg,
                      const char* text, uint8_t width, Align align) {
  if (width > sizeof(f.shown) - 1) width = sizeof(f.shown) - 1;
  char line[sizeof(f.shown)];
  memset(line, ' ', width);
  uint8_t len = strlen(text);
  if (len > width) {                                   // too long: keep what fits
    if (align == ALIGN_RIGHT) text += len - width;
    len = width;
  }
  memcpy(line + (align == ALIGN_RIGHT ? width - len : align == ALIGN_CENTER ? (width - len) / 2 : 0), text, len);

  if (fg != f.fg) {                                    // colour changed: repaint everything
    memset(f.shown, 0, sizeof(f.shown));
    f.fg = fg;
  }
  for (uint8_t i = 0; i < width; i++) {
    if (line[i] != f.shown[i]) {
      drawGlyph(x + i * 6 * scale, y, line[i], scale, fg, bg);
      f.shown[i] = line[i];
    }
  }
}

static void drawBox(const Rect& r, uint16_t fill, uint16_t border) {
  gfx.Set_Draw_color(fill);
  gfx.Fill_Round_Rectangle(r.x, r.y, r.x + r.w - 1, r.y + r.h - 1, BUTTON_RADIUS);
  gfx.Set_Draw_color(border);
  gfx.Draw_Round_Rectangle(r.x, r.y, r.x + r.w - 1, r.y + r.h - 1, BUTTON_RADIUS);
}

static void drawButton(const Rect& r, const char* label, uint8_t scale, uint16_t fill, uint16_t textColor) {
  drawBox(r, fill, COL_BORDER);
  drawTextCentered(r, label, scale, textColor, fill);
}

static bool inRect(const Rect& r, int16_t px, int16_t py) {
  return px >= r.x - TOUCH_SLOP_PX && px < r.x + r.w + TOUCH_SLOP_PX &&
         py >= r.y - TOUCH_SLOP_PX && py < r.y + r.h + TOUCH_SLOP_PX;
}

static void drawHeader(const char* title) {
  fillRect(0, 0, SCREEN_W, HEADER_H, COL_BG);
  drawText(10, 9, title, 2, COL_TEXT, COL_BG);
  fillRect(0, HEADER_H - 1, SCREEN_W, 1, COL_BORDER);
}

// ===========================================================================
// STATE
// ===========================================================================
enum Screen : uint8_t { SCREEN_SETUP, SCREEN_RUN, SCREEN_CAL };

static CycleController* ctl = nullptr;
static Screen screen = SCREEN_SETUP;
static uint32_t targets[NUM_SERVOS];            // chosen on the setup screen; kept when you return to it
static uint32_t inputBlockedUntil = 0;          // ignore touches briefly after a screen change

// What every touch can do.
enum Action : int8_t {
  ACT_NONE   = -1,
  ACT_ADJUST = 0,        // 0..15: row * 4 + part, part = 0:-large 1:-small 2:+small 3:+large
  ACT_COPY   = 16,       // copy servo 1's target to all
  ACT_START  = 17,
  ACT_TOGGLE = 20,       // 20..23: pause/resume servo (action - 20)
  ACT_PAUSE_ALL  = 24,
  ACT_RESUME_ALL = 25,
  ACT_SETUP  = 26,       // back to the setup screen
  ACT_CAL_ENTER = 30,    // open the calibration screen
  ACT_CAL_TAB   = 31,    // 31..34: pick servo
  ACT_CAL_GO_START = 35,
  ACT_CAL_GO_END   = 36,
  ACT_CAL_DEFAULT  = 37,
  ACT_CAL_SAVE     = 38,
  ACT_CAL_BACK     = 39,
  ACT_CAL_JOG      = 40  // 40..51: row * 6 + k   (row 0 = START, 1 = END; k 0..2 = minus large..small, 3..5 = plus small..large)
};

// --- Setup screen state ---
static Field setupValueField[NUM_SERVOS];
static Field setupEstimateField;
static uint32_t setupMessageUntil = 0;          // while non-zero, the estimate line shows a warning
static bool     setupMessageBusy = false;       // which warning: servos still moving (true) or no target chosen (false)

// --- Run screen state: what is currently drawn, so only changes are repainted ---
enum OverallState : uint8_t { OV_RUNNING, OV_PAUSED, OV_COMPLETE, OV_FAULT, OV_UNKNOWN = 255 };
enum SetupBtnState : uint8_t { SB_SETUP, SB_CONFIRM, SB_NEW_TEST, SB_UNKNOWN = 255 };

static Field runCountField[NUM_SERVOS];
static uint8_t runCountChars[NUM_SERVOS];       // digits in this row's target (0 = servo is OFF)
static Field runStatusField[NUM_SERVOS];
static Field runOverallField;
static uint32_t shownCount[NUM_SERVOS];
static uint8_t  shownStatus[NUM_SERVOS];
static int16_t  shownBarFill[NUM_SERVOS];
static uint8_t  shownOverall, shownPauseAll, shownResumeAll, shownSetupBtn;
static bool     confirmPending = false;
static uint32_t confirmUntil = 0;

// ===========================================================================
// SETUP SCREEN
// ===========================================================================
enum SetupPart : uint8_t { P_MINUS_L, P_MINUS_S, P_VALUE, P_PLUS_S, P_PLUS_L };

static Rect setupRect(uint8_t row, SetupPart part) {
  int16_t y = SETUP_ROW_Y0 + row * SETUP_ROW_STEP;
  int16_t x = SETUP_X0;
  static const int16_t widths[5] = { SETUP_BIG_W, SETUP_SMALL_W, SETUP_VALUE_W, SETUP_SMALL_W, SETUP_BIG_W };
  for (uint8_t p = 0; p < part; p++) x += widths[p] + SETUP_GAP;
  return Rect{ x, y, widths[part], SETUP_ROW_H };
}

static const Rect CAL_BTN   = { 8,   SETUP_BOTTOM_Y, 120, SETUP_BOTTOM_H };
static const Rect COPY_BTN  = { 134, SETUP_BOTTOM_Y, 120, SETUP_BOTTOM_H };
static const Rect START_BTN = { 260, SETUP_BOTTOM_Y, 210, SETUP_BOTTOM_H };

static void formatDuration(uint64_t seconds, char* out, size_t n) {
  unsigned long d = (unsigned long)(seconds / 86400UL);
  unsigned long h = (unsigned long)((seconds % 86400UL) / 3600UL);
  unsigned long m = (unsigned long)((seconds % 3600UL) / 60UL);
  unsigned long s = (unsigned long)(seconds % 60UL);
  if (d > 0)      snprintf(out, n, "%lud %02luh", d, h);
  else if (h > 0) snprintf(out, n, "%luh %02lum", h, m);
  else if (m > 0) snprintf(out, n, "%lum %02lus", m, s);
  else            snprintf(out, n, "%lus", s);
}

// Time for the slowest servo to finish its target, from the speed and dwell settings.
static void drawEstimate() {
  uint32_t longest = 0;
  for (uint8_t i = 0; i < NUM_SERVOS; i++) if (targets[i] > longest) longest = targets[i];

  char text[40];
  uint16_t color = COL_DIM;
  if (setupMessageUntil != 0) {
    snprintf(text, sizeof(text), setupMessageBusy ? "Servos still moving, wait" : "Set a target first");
    color = COL_BAD;
  } else if (longest == 0) {
    snprintf(text, sizeof(text), "No servos selected");
  } else {
    uint32_t sweepMs = (uint32_t)SWEEP_DEG * 1000UL / SERVO_SPEED_DEG_PER_SEC;
    uint32_t cycleMs = 2 * sweepMs + DWELL_AT_END_MS + DWELL_AT_START_MS;
    char dur[16];
    formatDuration((uint64_t)longest * cycleMs / 1000, dur, sizeof(dur));
    snprintf(text, sizeof(text), "Est. run time %s", dur);
  }
  drawField(setupEstimateField, 8, SETUP_EST_Y, 2, color, COL_BG, text, 31, ALIGN_LEFT);
}

static void drawSetupValue(uint8_t row) {
  char text[12];
  uint16_t color;
  if (targets[row] == 0) {
    snprintf(text, sizeof(text), "OFF");
    color = COL_DIM;
  } else {
    snprintf(text, sizeof(text), "%lu", (unsigned long)targets[row]);
    color = COL_TEXT;
  }
  Rect box = setupRect(row, P_VALUE);
  drawField(setupValueField[row], box.x + 2, box.y + (box.h - 21) / 2, 3, color, COL_PANEL, text, 6, ALIGN_CENTER);
}

static void drawSetupScreen() {
  screen = SCREEN_SETUP;
  gfx.Fill_Screen(COL_BG);
  drawHeader("CYCLE TESTER");
  const char* sub = "SET CYCLES";
  drawText(SCREEN_W - 10 - textWidth(sub, 2), 9, sub, 2, COL_ACCENT, COL_BG);

  for (uint8_t i = 0; i < NUM_SERVOS; i++) { resetField(setupValueField[i]); }
  resetField(setupEstimateField);

  char label[8], step[8];
  for (uint8_t row = 0; row < NUM_SERVOS; row++) {
    Rect labelBox = { SETUP_LABEL_X, (int16_t)(SETUP_ROW_Y0 + row * SETUP_ROW_STEP), 30, SETUP_ROW_H };
    snprintf(label, sizeof(label), "S%u", (unsigned)(row + 1));
    drawTextCentered(labelBox, label, 2, COL_TEXT, COL_BG);

    const SetupPart parts[4] = { P_MINUS_L, P_MINUS_S, P_PLUS_S, P_PLUS_L };
    for (uint8_t k = 0; k < 4; k++) {
      uint32_t amount = (parts[k] == P_MINUS_L || parts[k] == P_PLUS_L) ? STEP_LARGE : STEP_SMALL;
      bool minus = (parts[k] == P_MINUS_L || parts[k] == P_MINUS_S);
      snprintf(step, sizeof(step), "%c%lu", minus ? '-' : '+', (unsigned long)amount);
      drawButton(setupRect(row, parts[k]), step, 2, COL_BUTTON, COL_TEXT);
    }
    drawBox(setupRect(row, P_VALUE), COL_PANEL, COL_BORDER);
    drawSetupValue(row);
  }
  drawEstimate();
  drawButton(CAL_BTN, "CALIBRATE", 2, COL_BUTTON, COL_TEXT);
  drawButton(COPY_BTN, "ALL = S1", 2, COL_BUTTON, COL_TEXT);
  drawButton(START_BTN, "START", 3, COL_GOOD, COL_ON_BRIGHT);
  inputBlockedUntil = millis() + 400;
}

static void adjustTarget(uint8_t row, uint8_t part) {
  uint32_t step = (part == 0 || part == 3) ? STEP_LARGE : STEP_SMALL;
  uint32_t v = targets[row];
  if (part <= 1) v = (v > step) ? v - step : 0;
  else           v = (v + step > MAX_TARGET_CYCLES) ? MAX_TARGET_CYCLES : v + step;
  if (v != targets[row]) {
    targets[row] = v;
    drawSetupValue(row);
    drawEstimate();
  }
}

// ===========================================================================
// RUN SCREEN
// ===========================================================================
static int16_t runRowY(uint8_t row) { return RUN_ROW_Y0 + row * RUN_ROW_STEP; }

static Rect runPanelRect(uint8_t row)  { return Rect{ 6, runRowY(row), 468, RUN_ROW_H }; }
static Rect runToggleRect(uint8_t row) { return Rect{ 368, (int16_t)(runRowY(row) + 3), 100, 44 }; }
static Rect runBarRect(uint8_t row)    { return Rect{ 60, (int16_t)(runRowY(row) + 36), 208, 8 }; }
static const Rect PAUSE_ALL_BTN  = { 6,   RUN_BOTTOM_Y, 150, RUN_BOTTOM_H };
static const Rect RESUME_ALL_BTN = { 162, RUN_BOTTOM_Y, 150, RUN_BOTTOM_H };
static const Rect SETUP_BTN      = { 318, RUN_BOTTOM_Y, 156, RUN_BOTTOM_H };

static uint16_t statusColor(ChannelStatus s) {
  switch (s) {
    case CH_RUNNING: return COL_GOOD;
    case CH_PAUSED:  return COL_WARN;
    case CH_DONE:    return COL_ACCENT;
    case CH_FAULT:   return COL_BAD;
    default:         return COL_DIM;
  }
}

static const char* statusText(ChannelStatus s) {
  switch (s) {
    case CH_RUNNING: return "RUNNING";
    case CH_PAUSED:  return "PAUSED";
    case CH_DONE:    return "DONE";
    case CH_FAULT:   return "FAULT";
    default:         return "OFF";
  }
}

static const int16_t RUN_COUNT_X = 60;

static void drawRunCount(uint8_t row, uint32_t count) {
  if (runCountChars[row] == 0) return;             // a servo that is OFF shows no counter
  char text[12];
  snprintf(text, sizeof(text), "%lu", (unsigned long)count);
  drawField(runCountField[row], RUN_COUNT_X, runRowY(row) + 6, 3, COL_TEXT, COL_PANEL, text, runCountChars[row], ALIGN_RIGHT);
}

static void drawRunBar(uint8_t row, const CycleChannel& ch) {
  if (runCountChars[row] == 0) return;
  Rect bar = runBarRect(row);
  int16_t fill = 0;
  if (ch.target() > 0) fill = (int16_t)((uint64_t)(bar.w - 2) * ch.count() / ch.target());
  if (fill > bar.w - 2) fill = bar.w - 2;
  if (fill == shownBarFill[row]) return;
  if (fill < shownBarFill[row]) {                       // shrank (should not happen): repaint the whole track
    fillRect(bar.x + 1, bar.y + 1, bar.w - 2, bar.h - 2, COL_BG);
    shownBarFill[row] = 0;
  }
  fillRect(bar.x + 1 + shownBarFill[row], bar.y + 1, fill - shownBarFill[row], bar.h - 2, COL_ACCENT);
  shownBarFill[row] = fill;
}

static void drawRunToggle(uint8_t row, ChannelStatus s) {
  Rect r = runToggleRect(row);
  if (s == CH_RUNNING)     drawButton(r, "PAUSE",  2, COL_WARN, COL_ON_BRIGHT);
  else if (s == CH_PAUSED) drawButton(r, "RESUME", 2, COL_GOOD, COL_ON_BRIGHT);
  else                     drawButton(r, "-",      2, COL_PANEL, COL_DIM);
}

static void drawRunStatic() {
  screen = SCREEN_RUN;
  gfx.Fill_Screen(COL_BG);
  drawHeader("CYCLE TESTER");
  resetField(runOverallField);

  for (uint8_t row = 0; row < NUM_SERVOS; row++) {
    resetField(runCountField[row]);
    resetField(runStatusField[row]);
    shownCount[row] = 0xFFFFFFFFUL;           // impossible values, so the first refresh paints everything
    shownStatus[row] = 255;
    shownBarFill[row] = 0;

    drawBox(runPanelRect(row), COL_PANEL, COL_BORDER);
    char label[8];
    snprintf(label, sizeof(label), "S%u", (unsigned)(row + 1));
    drawText(14, runRowY(row) + 14, label, 3, targets[row] ? COL_TEXT : COL_DIM, COL_PANEL);

    uint8_t digits = 0;                                 // how many digits this row's target has
    for (uint32_t t = targets[row]; t > 0; t /= 10) digits++;
    runCountChars[row] = digits;
    if (digits > 0) {
      char of[16];
      snprintf(of, sizeof(of), "/ %lu", (unsigned long)targets[row]);
      drawText(RUN_COUNT_X + digits * 18 + 4, runRowY(row) + 13, of, 2, COL_DIM, COL_PANEL);

      Rect bar = runBarRect(row);                       // empty progress track
      fillRect(bar.x, bar.y, bar.w, bar.h, COL_BORDER);
      fillRect(bar.x + 1, bar.y + 1, bar.w - 2, bar.h - 2, COL_BG);
    }
  }
  shownOverall = OV_UNKNOWN;
  shownPauseAll = shownResumeAll = 255;
  shownSetupBtn = SB_UNKNOWN;
  confirmPending = false;
  inputBlockedUntil = millis() + 400;
}

static OverallState overallState() {
  if (ctl->countWithStatus(CH_FAULT) > 0) return OV_FAULT;
  if (ctl->allFinished())                 return OV_COMPLETE;
  if (ctl->countWithStatus(CH_RUNNING) > 0) return OV_RUNNING;
  return OV_PAUSED;
}

// Repaints ONE thing that is out of date (a servo row, the header status or a bottom button), then returns.
// It is called once per pass of loop(), so a big change such as PAUSE ALL is painted over a handful of
// passes instead of in one long stall, and the cycle logic and the touch screen keep being serviced in between.
static void refreshRunStep(uint32_t now) {
  if (confirmPending && (int32_t)(now - confirmUntil) >= 0) confirmPending = false;

  for (uint8_t row = 0; row < NUM_SERVOS; row++) {
    const CycleChannel& ch = ctl->channel(row);
    ChannelStatus st = ch.status();
    bool countChanged  = ch.count() != shownCount[row];
    bool statusChanged = (uint8_t)st != shownStatus[row];
    if (!countChanged && !statusChanged) continue;

    if (countChanged) {
      shownCount[row] = ch.count();
      drawRunCount(row, ch.count());
    }
    if (statusChanged) {
      shownStatus[row] = (uint8_t)st;
      drawField(runStatusField[row], 278, runRowY(row) + 17, 2, statusColor(st), COL_PANEL, statusText(st), 7, ALIGN_LEFT);
      drawRunToggle(row, st);
    }
    drawRunBar(row, ch);         // also repainted when a servo finishes, so its bar ends up full
    return;
  }

  OverallState ov = overallState();
  if ((uint8_t)ov != shownOverall) {
    shownOverall = (uint8_t)ov;
    const char* text = (ov == OV_RUNNING) ? "RUNNING" : (ov == OV_PAUSED) ? "PAUSED" : (ov == OV_COMPLETE) ? "COMPLETE" : "FAULT";
    uint16_t color = (ov == OV_RUNNING) ? COL_GOOD : (ov == OV_PAUSED) ? COL_WARN : (ov == OV_COMPLETE) ? COL_ACCENT : COL_BAD;
    drawField(runOverallField, SCREEN_W - 10 - 8 * 12, 9, 2, color, COL_BG, text, 8, ALIGN_RIGHT);
    return;
  }

  // PAUSE ALL is live while anything is running, RESUME ALL while anything is paused.
  bool canPause  = ctl->countWithStatus(CH_RUNNING) > 0;
  bool canResume = ctl->countWithStatus(CH_PAUSED) > 0;
  if (shownPauseAll != canPause) {
    shownPauseAll = canPause;
    drawButton(PAUSE_ALL_BTN, "PAUSE ALL", 2, canPause ? COL_WARN : COL_PANEL, canPause ? COL_ON_BRIGHT : COL_DIM);
    return;
  }
  if (shownResumeAll != canResume) {
    shownResumeAll = canResume;
    drawButton(RESUME_ALL_BTN, "RESUME ALL", 2, canResume ? COL_GOOD : COL_PANEL, canResume ? COL_ON_BRIGHT : COL_DIM);
    return;
  }

  SetupBtnState sb = confirmPending ? SB_CONFIRM : (ctl->allFinished() ? SB_NEW_TEST : SB_SETUP);
  if ((uint8_t)sb != shownSetupBtn) {
    shownSetupBtn = (uint8_t)sb;
    if (sb == SB_CONFIRM)       drawButton(SETUP_BTN, "SURE?",    2, COL_BAD, COL_TEXT);
    else if (sb == SB_NEW_TEST) drawButton(SETUP_BTN, "NEW TEST", 2, COL_ACCENT, COL_ON_BRIGHT);
    else                        drawButton(SETUP_BTN, "SETUP",    2, COL_BUTTON, COL_TEXT);
  }
}

// ===========================================================================
// CALIBRATION SCREEN
// ===========================================================================
// Pick a servo, then jog START (moves the whole 180 degree sweep) and END (moves only the end) in small
// steps. Only the servo you are adjusting is switched on, and the arm follows every press, so you can watch it
// and measure. SAVE keeps the values in EEPROM. Everything here is in microseconds of pulse width; the
// degrees shown are the nominal conversion from Config.h.

static uint8_t     calServo = 0;                 // the servo being adjusted
static Field       calStartField, calEndField, calInfoField1, calInfoField2, calStatusField;
static bool        calDiscardPending = false;    // BACK was tapped with unsaved changes: waiting for a second tap
static uint32_t    calDiscardUntil = 0;
static const char* calMessage = nullptr;         // a short-lived line of status text (always a string literal)
static uint16_t    calMessageColor = 0;
static uint32_t    calMessageUntil = 0;

static Rect calTabRect(uint8_t i) {
  return Rect{ (int16_t)(12 + i * (CALSCR_TAB_W + CALSCR_TAB_GAP)), CALSCR_TAB_Y, CALSCR_TAB_W, CALSCR_TAB_H };
}
static Rect calBoxRect(uint8_t row) {
  return Rect{ CALSCR_BOX_X, (int16_t)(CALSCR_ROW_Y0 + row * CALSCR_ROW_STEP), CALSCR_BOX_W, CALSCR_ROW_H };
}
static Rect calJogRect(uint8_t row, uint8_t k) {
  return Rect{ (int16_t)(CALSCR_JOG_X0 + k * (CALSCR_JOG_W + CALSCR_JOG_GAP)), (int16_t)(CALSCR_ROW_Y0 + row * CALSCR_ROW_STEP), CALSCR_JOG_W, CALSCR_ROW_H };
}
static const Rect CAL_GO_START_BTN = { 4,   CALSCR_BTN1_Y, 152, CALSCR_BTN1_H };
static const Rect CAL_GO_END_BTN   = { 162, CALSCR_BTN1_Y, 152, CALSCR_BTN1_H };
static const Rect CAL_DEFAULT_BTN  = { 320, CALSCR_BTN1_Y, 152, CALSCR_BTN1_H };
static const Rect CAL_SAVE_BTN     = { 4,   CALSCR_BTN2_Y, 232, CALSCR_BTN2_H };
static const Rect CAL_BACK_BTN     = { 242, CALSCR_BTN2_Y, 230, CALSCR_BTN2_H };

// Pulse width -> degrees, using the nominal figures from Config.h. The real servo can differ a little;
// that is exactly what calibrating by eye is for.
static const long US_PER_DEG_X1000 = (long)(SERVO_PULSE_MAX_US - SERVO_PULSE_MIN_US) * 1000L / SERVO_FULL_TRAVEL_DEG;
static long tenthsOfDegree(long us) { return (us * 10000L + US_PER_DEG_X1000 / 2) / US_PER_DEG_X1000; }

static int calJogDelta(uint8_t k) { return (k < 3) ? -(int)CAL_STEP_US[2 - k] : (int)CAL_STEP_US[k - 3]; }

static void drawCalTab(uint8_t i) {
  char label[8];
  snprintf(label, sizeof(label), "S%u", (unsigned)(i + 1));
  bool on = (i == calServo);
  drawButton(calTabRect(i), label, 2, on ? COL_ACCENT : COL_BUTTON, on ? COL_ON_BRIGHT : COL_TEXT);
}

static void drawCalValues() {
  const CycleChannel& c = ctl->channel(calServo);
  char text[64];
  Rect startBox = calBoxRect(0), endBox = calBoxRect(1);
  snprintf(text, sizeof(text), "%d", c.startUs());
  drawField(calStartField, startBox.x + 8, startBox.y + 16, 3, COL_TEXT, COL_PANEL, text, 4, ALIGN_RIGHT);
  snprintf(text, sizeof(text), "%d", c.endUs());
  drawField(calEndField, endBox.x + 8, endBox.y + 16, 3, COL_TEXT, COL_PANEL, text, 4, ALIGN_RIGHT);

  int span = abs(c.endUs() - c.startUs());
  long tenths = tenthsOfDegree(span);
  snprintf(text, sizeof(text), "Sweep %d us = %ld.%ld deg%s", span, tenths / 10, tenths % 10, c.endUs() < c.startUs() ? " (REV)" : "");
  drawField(calInfoField1, 8, CALSCR_INFO_Y, 2, COL_TEXT, COL_BG, text, 31, ALIGN_LEFT);
}

static void drawCalStatus(uint32_t now) {
  const char* text;
  uint16_t color;
  if (calMessage != nullptr && (int32_t)(now - calMessageUntil) < 0) { text = calMessage; color = calMessageColor; }
  else if (calDiscardPending)                                       { text = "Tap BACK again to discard"; color = COL_BAD; }
  else if (ctl->calDirty())                                         { text = "UNSAVED - tap SAVE"; color = COL_WARN; }
  else                                                              { text = "START moves both, END moves one"; color = COL_DIM; }
  drawField(calStatusField, 8, CALSCR_INFO_Y + 2 * CALSCR_INFO_STEP, 2, color, COL_BG, text, 31, ALIGN_LEFT);
}

static void drawCalBack() {
  if (calDiscardPending) drawButton(CAL_BACK_BTN, "DISCARD?", 3, COL_BAD, COL_TEXT);
  else                   drawButton(CAL_BACK_BTN, "BACK", 3, COL_BUTTON, COL_TEXT);
}

static void setCalMessage(const char* text, uint16_t color, uint32_t now, uint32_t forMs) {
  calMessage = text;
  calMessageColor = color;
  calMessageUntil = now + forMs;
  drawCalStatus(now);
}

static void drawCalScreen() {
  screen = SCREEN_CAL;
  calDiscardPending = false;
  calMessage = nullptr;
  gfx.Fill_Screen(COL_BG);
  drawHeader("CALIBRATE");
  const char* sub = "one servo on at a time";
  drawText(SCREEN_W - 10 - textWidth(sub, 1), 12, sub, 1, COL_DIM, COL_BG);

  resetField(calStartField);
  resetField(calEndField);
  resetField(calInfoField1);
  resetField(calInfoField2);
  resetField(calStatusField);

  for (uint8_t i = 0; i < NUM_SERVOS; i++) drawCalTab(i);

  for (uint8_t row = 0; row < 2; row++) {
    Rect box = calBoxRect(row);
    drawBox(box, COL_PANEL, COL_BORDER);
    drawText(box.x + 8, box.y + 5, row == 0 ? "START" : "END", 1, COL_DIM, COL_PANEL);
    drawText(box.x + 8 + 4 * 18 + 6, box.y + 28, "us", 1, COL_DIM, COL_PANEL);
    for (uint8_t k = 0; k < 6; k++) {
      int d = calJogDelta(k);
      char label[8];
      snprintf(label, sizeof(label), "%c%d", d < 0 ? '-' : '+', d < 0 ? -d : d);
      drawButton(calJogRect(row, k), label, 2, COL_BUTTON, COL_TEXT);
    }
  }
  drawCalValues();

  char text[96];
  long t0 = tenthsOfDegree(CAL_STEP_US[0]), t1 = tenthsOfDegree(CAL_STEP_US[1]), t2 = tenthsOfDegree(CAL_STEP_US[2]);
  snprintf(text, sizeof(text), "%u/%u/%u us = %ld.%ld/%ld.%ld/%ld.%ld deg", (unsigned)CAL_STEP_US[0], (unsigned)CAL_STEP_US[1], (unsigned)CAL_STEP_US[2],
           t0 / 10, t0 % 10, t1 / 10, t1 % 10, t2 / 10, t2 % 10);
  drawField(calInfoField2, 8, CALSCR_INFO_Y + CALSCR_INFO_STEP, 2, COL_DIM, COL_BG, text, 31, ALIGN_LEFT);

  drawButton(CAL_GO_START_BTN, "GO START", 2, COL_BUTTON, COL_TEXT);
  drawButton(CAL_GO_END_BTN,   "GO END",   2, COL_BUTTON, COL_TEXT);
  drawButton(CAL_DEFAULT_BTN,  "DEFAULT",  2, COL_BUTTON, COL_TEXT);
  drawButton(CAL_SAVE_BTN,     "SAVE",     3, COL_GOOD, COL_ON_BRIGHT);
  drawCalBack();

  uint32_t now = millis();        // taken after the (slow) redraw, so the message is on screen for its full time
  CycleController::CalSource src = ctl->calSource();
  if (src == CycleController::CAL_SAVED_IGNORED)    setCalMessage("Config changed, saved cal reset", COL_WARN, now, 8000);
  else if (src == CycleController::CAL_SAVED_BAD)   setCalMessage("Saved cal damaged, factory used", COL_BAD, now, 8000);
  else if (src == CycleController::CAL_FROM_SAVED)  setCalMessage("Loaded saved calibration", COL_GOOD, now, 4000);
  else                                              drawCalStatus(now);
  inputBlockedUntil = millis() + 400;
}

// Called every pass while the calibration screen is up: lets timed messages and the discard question expire.
static void updateCal(uint32_t now) {
  if (calDiscardPending && (int32_t)(now - calDiscardUntil) >= 0) {
    calDiscardPending = false;
    drawCalBack();
    drawCalStatus(now);
  }
  if (calMessage != nullptr && (int32_t)(now - calMessageUntil) >= 0) {
    calMessage = nullptr;
    drawCalStatus(now);
  }
}

// ===========================================================================
// TOUCH
// ===========================================================================
static Action hitTest(int16_t x, int16_t y) {
  if (screen == SCREEN_SETUP) {
    for (uint8_t row = 0; row < NUM_SERVOS; row++) {
      const SetupPart parts[4] = { P_MINUS_L, P_MINUS_S, P_PLUS_S, P_PLUS_L };
      for (uint8_t k = 0; k < 4; k++)
        if (inRect(setupRect(row, parts[k]), x, y)) return (Action)(ACT_ADJUST + row * 4 + k);
    }
    if (inRect(CAL_BTN, x, y))   return ACT_CAL_ENTER;
    if (inRect(COPY_BTN, x, y))  return ACT_COPY;
    if (inRect(START_BTN, x, y)) return ACT_START;
  } else if (screen == SCREEN_CAL) {
    for (uint8_t i = 0; i < NUM_SERVOS; i++)
      if (inRect(calTabRect(i), x, y)) return (Action)(ACT_CAL_TAB + i);
    for (uint8_t row = 0; row < 2; row++)
      for (uint8_t k = 0; k < 6; k++)
        if (inRect(calJogRect(row, k), x, y)) return (Action)(ACT_CAL_JOG + row * 6 + k);
    if (inRect(CAL_GO_START_BTN, x, y)) return ACT_CAL_GO_START;
    if (inRect(CAL_GO_END_BTN, x, y))   return ACT_CAL_GO_END;
    if (inRect(CAL_DEFAULT_BTN, x, y))  return ACT_CAL_DEFAULT;
    if (inRect(CAL_SAVE_BTN, x, y))     return ACT_CAL_SAVE;
    if (inRect(CAL_BACK_BTN, x, y))     return ACT_CAL_BACK;
  } else {
    for (uint8_t row = 0; row < NUM_SERVOS; row++)
      if (inRect(runToggleRect(row), x, y)) return (Action)(ACT_TOGGLE + row);
    if (inRect(PAUSE_ALL_BTN, x, y))  return ACT_PAUSE_ALL;
    if (inRect(RESUME_ALL_BTN, x, y)) return ACT_RESUME_ALL;
    if (inRect(SETUP_BTN, x, y))      return ACT_SETUP;
  }
  return ACT_NONE;
}

static bool isRepeating(Action a) {      // buttons that keep stepping while held
  return (a >= ACT_ADJUST && a < ACT_COPY) || (a >= ACT_CAL_JOG && a < ACT_CAL_JOG + 12);
}

static void enterSetup() {
  drawButton(SETUP_BTN, "WAIT...", 2, COL_WARN, COL_ON_BRIGHT);   // instant feedback: the full redraw takes a couple of seconds
  ctl->abort();
  drawSetupScreen();
}

static void perform(Action a, uint32_t now) {
  if (a != ACT_SETUP) confirmPending = false;      // tapping anything else cancels a pending "SURE?"
  if (a != ACT_CAL_BACK && calDiscardPending) {    // ...and a pending "DISCARD?" on the calibration screen
    calDiscardPending = false;
    if (screen == SCREEN_CAL) { drawCalBack(); drawCalStatus(now); }
  }

  if (a >= ACT_ADJUST && a < ACT_COPY) {
    adjustTarget((a - ACT_ADJUST) / 4, (a - ACT_ADJUST) % 4);

  } else if (a == ACT_COPY) {
    for (uint8_t i = 1; i < NUM_SERVOS; i++) {
      if (targets[i] != targets[0]) { targets[i] = targets[0]; drawSetupValue(i); }
    }
    drawEstimate();

  } else if (a == ACT_START) {
    bool any = false;
    for (uint8_t i = 0; i < NUM_SERVOS; i++) if (targets[i] > 0) any = true;
    if (!any) {
      setupMessageBusy = false;
      setupMessageUntil = now + 2000;
      drawEstimate();
      return;
    }
    drawButton(START_BTN, "STARTING...", 3, COL_WARN, COL_ON_BRIGHT);   // instant feedback: the full redraw takes a couple of seconds
    drawRunStatic();
    ctl->begin(targets, millis());   // start the servos only once the screen is up; the run screen then fills in over the next few passes

  } else if (a >= ACT_TOGGLE && a < ACT_TOGGLE + NUM_SERVOS) {
    ctl->toggle(a - ACT_TOGGLE, now);

  } else if (a == ACT_PAUSE_ALL) {
    ctl->pauseAll(now);

  } else if (a == ACT_RESUME_ALL) {
    ctl->resumeAll(now);

  } else if (a == ACT_SETUP) {
    if (ctl->allFinished() || (confirmPending && (int32_t)(now - confirmUntil) < 0)) {
      enterSetup();
    } else {
      confirmPending = true;                        // first tap: ask for a second one
      confirmUntil = now + CONFIRM_WINDOW_MS;
    }

  } else if (a == ACT_CAL_ENTER) {
    if (!ctl->calibrationAllowed()) {               // a servo is still homing after the last test
      setupMessageBusy = true;
      setupMessageUntil = now + 2500;
      drawEstimate();
      return;
    }
    drawButton(CAL_BTN, "WAIT...", 2, COL_WARN, COL_ON_BRIGHT);   // instant feedback: the full redraw takes a couple of seconds
    drawCalScreen();

  } else if (a >= ACT_CAL_TAB && a < ACT_CAL_TAB + NUM_SERVOS) {
    uint8_t picked = a - ACT_CAL_TAB;
    if (picked != calServo) {
      uint8_t previous = calServo;
      calServo = picked;
      ctl->calSelect(picked);                       // the previous servo goes limp; the new one stays off until you move it
      calMessage = nullptr;
      drawCalTab(previous);
      drawCalTab(picked);
      drawCalValues();
      drawCalStatus(now);
    }

  } else if (a >= ACT_CAL_JOG && a < ACT_CAL_JOG + 12) {
    uint8_t row = (a - ACT_CAL_JOG) / 6, k = (a - ACT_CAL_JOG) % 6;
    if (ctl->calJog(calServo, row == 1, calJogDelta(k), now)) {
      calMessage = nullptr;
      drawCalValues();
      drawCalStatus(now);
    } else {
      setCalMessage("At the limit", COL_WARN, now, 1200);
    }

  } else if (a == ACT_CAL_GO_START || a == ACT_CAL_GO_END) {
    ctl->calGo(calServo, a == ACT_CAL_GO_END, now);

  } else if (a == ACT_CAL_DEFAULT) {
    ctl->calSetDefaults(calServo, now);
    calMessage = nullptr;
    drawCalValues();
    drawCalStatus(now);

  } else if (a == ACT_CAL_SAVE) {
    if (!ctl->calSaveEnabled())  setCalMessage("Saving is off in Config.h", COL_BAD, now, 4000);
    else if (ctl->calSave())     setCalMessage("Saved", COL_GOOD, now, 2500);
    else                         setCalMessage("SAVE FAILED", COL_BAD, now, 4000);

  } else if (a == ACT_CAL_BACK) {
    if (ctl->calDirty() && !(calDiscardPending && (int32_t)(now - calDiscardUntil) < 0)) {
      calDiscardPending = true;                     // unsaved changes: ask before throwing them away
      calDiscardUntil = now + CONFIRM_WINDOW_MS;
      drawCalBack();
      drawCalStatus(now);
    } else {
      calDiscardPending = false;
      if (ctl->calDirty()) ctl->calDiscard();
      ctl->calFinish(now);                          // a switched-on servo glides to its start position and is released there
      drawButton(CAL_BACK_BTN, "WAIT...", 3, COL_WARN, COL_ON_BRIGHT);
      drawSetupScreen();
    }
  }
}

// Reads the touch controller. Returns true while a finger is down, with the position in screen pixels.
static bool readTouch(int16_t& x, int16_t& y) {
  TS_Point p = ctp.getPoint();         // one I2C read; z is 0 when nothing is touching
  if (p.z == 0) return false;
  if (p.x < 0 || p.x >= SCREEN_H || p.y < 0 || p.y > SCREEN_W) return false;   // outside the panel: ignore noise
  x = SCREEN_W - p.y;                  // the panel is mounted rotated relative to the display (rotation 3)
  y = p.x;
  if (x >= SCREEN_W) x = SCREEN_W - 1;
  return true;
}

static void pollTouch(uint32_t now) {
  static uint32_t lastPoll = 0;
  static bool     pressed = false;       // a press has been recognised and the finger has not lifted yet
  static Action   pressAction = ACT_NONE;
  static uint8_t  candidateHits = 0;     // consecutive polls that saw a touch near candidate position
  static int16_t  candX = 0, candY = 0;
  static uint8_t  missedPolls = 0;
  static uint32_t nextRepeat = 0;
  static uint16_t repeats = 0;

  if (now - lastPoll < TOUCH_POLL_MS) return;
  lastPoll = now;

  int16_t x, y;
  if (readTouch(x, y)) {
    missedPolls = 0;
    if (!pressed) {
      // A real tap stays put for a couple of polls. A glitchy I2C read that returns garbage
      // does not, so require two agreeing readings before believing it.
      if (candidateHits > 0 && abs(x - candX) < 40 && abs(y - candY) < 40) candidateHits++;
      else { candX = x; candY = y; candidateHits = 1; }

      if (candidateHits >= 2 && (int32_t)(now - inputBlockedUntil) >= 0) {
        pressed = true;
        pressAction = hitTest(x, y);
        repeats = 0;
        nextRepeat = now + REPEAT_DELAY_MS;
        if (TOUCH_DEBUG) { Serial.print(F("touch ")); Serial.print(x); Serial.print(','); Serial.print(y); Serial.print(F(" action ")); Serial.println((int)pressAction); }
        if (pressAction != ACT_NONE) perform(pressAction, now);
      }
    } else if (isRepeating(pressAction) && (int32_t)(now - nextRepeat) >= 0 && hitTest(x, y) == pressAction) {
      perform(pressAction, now);       // held +/- button: keep stepping, speeding up the longer it is held
      repeats++;
      nextRepeat = now + (repeats >= REPEAT_FAST_AFTER ? REPEAT_FAST_INTERVAL_MS : REPEAT_INTERVAL_MS);
    }
  } else {
    candidateHits = 0;
    // A single empty read in the middle of a long press can happen (a missed I2C read, for
    // instance), so only call it a release after two in a row.
    if (pressed && ++missedPolls >= 2) { pressed = false; missedPolls = 0; }
  }
}

// ===========================================================================
// PUBLIC
// ===========================================================================
void Ui::begin(CycleController& controller) {
  ctl = &controller;
  for (uint8_t i = 0; i < NUM_SERVOS; i++) targets[i] = DEFAULT_TARGET_CYCLES;

  gfx.Init_LCD();
  gfx.Set_Rotation(3);
  pinMode(TOUCH_INT_PIN, INPUT_PULLUP);

  Wire.begin();
#if defined(WIRE_HAS_TIMEOUT)
  Wire.setWireTimeout(5000, true);     // never let a stuck I2C bus freeze the program (and the cycle counting with it)
#endif

  // The touch controller needs a moment after power-up; keep trying rather than giving up at once.
  bool found = false;
  for (uint8_t attempt = 0; attempt < 20 && !found; attempt++) {
    found = ctp.begin(TOUCH_THRESHOLD);
    if (!found) delay(100);
  }
  if (!found) {
    gfx.Fill_Screen(COL_BG);
    drawText(10, 40, "Touch controller not found", 2, COL_BAD, COL_BG);
    drawText(10, 72, "Check SDA, SCL and the CTP_INT", 2, COL_TEXT, COL_BG);
    drawText(10, 96, "wire (pin 2), then wait here.", 2, COL_TEXT, COL_BG);
    while (!ctp.begin(TOUCH_THRESHOLD)) delay(500);   // carries on by itself once the wire is fixed
  }

  drawSetupScreen();
}

void Ui::update(uint32_t now) {
  pollTouch(now);

  if (screen == SCREEN_SETUP) {
    if (setupMessageUntil != 0 && (int32_t)(now - setupMessageUntil) >= 0) {
      setupMessageUntil = 0;
      drawEstimate();
    }
  } else if (screen == SCREEN_CAL) {
    updateCal(now);
  } else {
    refreshRunStep(now);
  }
}
