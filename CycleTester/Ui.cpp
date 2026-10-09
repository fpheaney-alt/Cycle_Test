#include <LCDWIKI_SPI.h>
#include <lcd_spi_registers.h>
#include <mcu_spi_magic.h>
#include <Wire.h>
#include <LCDWIKI_GUI.h>
#include <Adafruit_FT6206.h>

#include "Ui.h"
#include "Config.h"
#include "UiFonts.h"

// ===========================================================================
// HARDWARE
// ===========================================================================
static LCDWIKI_SPI gfx(ST7796S, LCD_CS_PIN, LCD_DC_PIN, LCD_RST_PIN, LCD_LED_PIN);
static Adafruit_FT6206 ctp;

// ===========================================================================
// LOOK AND LAYOUT - everything you would change to restyle the screens is in this section
// ===========================================================================
// The look follows Apple's dark mode: black background, grouped "cards", soft gray buttons, system blue for
// the main action, and the Inter typeface (a close relative of Apple's San Francisco).
const int16_t SCREEN_W = 480;      // landscape, after Set_Rotation(3)
const int16_t SCREEN_H = 320;

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
const uint16_t COL_BG      = rgb565(0, 0, 0);          // screen
const uint16_t COL_CARD    = rgb565(28, 28, 30);       // grouped cards
const uint16_t COL_FILL    = rgb565(58, 58, 62);       // gray buttons, empty progress tracks
const uint16_t COL_SEP     = rgb565(56, 56, 58);       // hairlines between rows
const uint16_t COL_TEXT    = rgb565(255, 255, 255);    // primary label
const uint16_t COL_TEXT2   = rgb565(152, 152, 159);    // secondary label
const uint16_t COL_TEXT3   = rgb565(99, 99, 104);      // tertiary label, disabled
const uint16_t COL_BLUE    = rgb565(10, 132, 255);
const uint16_t COL_GREEN   = rgb565(48, 209, 88);
const uint16_t COL_ORANGE  = rgb565(255, 159, 10);
const uint16_t COL_RED     = rgb565(255, 69, 58);
const uint16_t COL_BLUE_TINT   = rgb565(24, 48, 75);   // the same colours at 20 % over a card, for "tinted" buttons
const uint16_t COL_GREEN_TINT  = rgb565(32, 64, 42);
const uint16_t COL_ORANGE_TINT = rgb565(73, 54, 26);

const int16_t BUTTON_RADIUS = 12;
const int16_t CARD_RADIUS   = 14;
const int16_t MARGIN        = 12;       // space between the screen edge and the cards

// --- Setup screen: one card holding four rows ---
const int16_t SETUP_CARD_Y    = 44;
const int16_t SETUP_ROW_H     = 46;
const int16_t SETUP_BTN_H     = 38;     // buttons are a little shorter than their row
const int16_t SETUP_LABEL_X   = 28;
const int16_t SETUP_X0        = 100;    // left edge of the first (-1000) button
const int16_t SETUP_BIG_W     = 60;     // width of the -1000 / +1000 buttons
const int16_t SETUP_SMALL_W   = 52;     // width of the -100 / +100 buttons
const int16_t SETUP_VALUE_W   = 108;    // width of the number between them (six digits fit)
const int16_t SETUP_GAP       = 6;
const int16_t SETUP_EST_Y     = 236;    // line showing the estimated run time
const int16_t SETUP_BOTTOM_Y  = 262;
const int16_t SETUP_BOTTOM_H  = 48;

// --- Run screen: one card per servo ---
const int16_t RUN_ROW_Y0   = 42;
const int16_t RUN_ROW_STEP = 56;
const int16_t RUN_ROW_H    = 52;
const int16_t RUN_BOTTOM_Y = 270;
const int16_t RUN_BOTTOM_H = 44;

// ===========================================================================
// SMALL DRAWING TOOLKIT
// ===========================================================================
// Text is anti-aliased: each character is blended from its 4-bit coverage bitmap (UiFonts.h) between the text
// colour and the colour behind it, and streamed to the display as one block. Because the background is a known
// flat colour, no read-back from the display is needed. No String objects, no heap use: it runs for days.

struct Rect { int16_t x, y, w, h; };

static void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  if (w > 0 && h > 0) gfx.Fill_Rect(x, y, w, h, color);
}

// Colour between bg and fg; a = 0 gives bg, 255 gives fg.
static uint16_t mix565(uint16_t bg, uint16_t fg, uint8_t a) {
  int16_t br = bg >> 11, bgc = (bg >> 5) & 63, bb = bg & 31;
  int16_t fr = fg >> 11, fgc = (fg >> 5) & 63, fb = fg & 31;
  int16_t r = br + (int16_t)(((fr - br) * (int16_t)a + (fr >= br ? 127 : -127)) / 255);
  int16_t g = bgc + (int16_t)(((fgc - bgc) * (int16_t)a + (fgc >= bgc ? 127 : -127)) / 255);
  int16_t b = bb + (int16_t)(((fb - bb) * (int16_t)a + (fb >= bb ? 127 : -127)) / 255);
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static void loadGlyph(const UiFont& font, char c, UiGlyph& g) {
  uint8_t u = (uint8_t)c;
  if (u < UI_FIRST_CHAR || u > UI_LAST_CHAR) u = '?';
  memcpy_P(&g, &font.glyphs[u - UI_FIRST_CHAR], sizeof(UiGlyph));
}

const uint8_t MAX_CELL_W = 32;     // widest character cell in any font (checked when the fonts are generated)

// One character: a cell `advance` wide and `font.height` tall, painted completely (no separate clear needed).
static void drawGlyph(int16_t x, int16_t y, char c, const UiFont& font, uint16_t fg, uint16_t bg) {
  UiGlyph g;
  loadGlyph(font, c, g);
  uint8_t cellW = g.adv > MAX_CELL_W ? MAX_CELL_W : g.adv;
  if (cellW == 0) return;
  uint16_t row[MAX_CELL_W];
  gfx.Set_Addr_Window(x, y, x + cellW - 1, y + font.height - 1);
  const uint8_t* bits = font.bitmaps + g.offset;
  for (uint8_t ry = 0; ry < font.height; ry++) {
    int16_t gy = (int16_t)ry - (int16_t)font.ascent - g.yoff;      // row within the glyph bitmap
    if (gy < 0 || gy >= g.h) {
      for (uint8_t cx = 0; cx < cellW; cx++) row[cx] = bg;
    } else {
      for (uint8_t cx = 0; cx < cellW; cx++) {
        int16_t gx = (int16_t)cx - g.xoff;
        uint16_t color = bg;
        if (gx >= 0 && gx < g.w) {
          uint16_t index = (uint16_t)gy * g.w + gx;
          uint8_t byte = pgm_read_byte(bits + (index >> 1));
          uint8_t a = (index & 1) ? (byte & 0x0F) : (byte >> 4);
          if (a == 15) color = fg;
          else if (a) color = mix565(bg, fg, (uint8_t)(a * 17));
        }
        row[cx] = color;
      }
    }
    gfx.Push_Any_Color(row, cellW, ry == 0, 0);
  }
}

static int16_t textWidth(const char* s, const UiFont& font) {
  int16_t w = 0;
  UiGlyph g;
  for (; *s; s++) { loadGlyph(font, *s, g); w += g.adv; }
  return w;
}

// x, y is the top-left of the first character cell.
static void drawText(int16_t x, int16_t y, const char* s, const UiFont& font, uint16_t fg, uint16_t bg) {
  UiGlyph g;
  for (; *s; s++) {
    drawGlyph(x, y, *s, font, fg, bg);
    loadGlyph(font, *s, g);
    x += g.adv;
  }
}

// Top of a text cell so that the text's baseline lands on `baseline`, or so that capital letters are centred on `centreY`.
static int16_t topForBaseline(const UiFont& font, int16_t baseline) { return baseline - font.ascent; }
static int16_t topForCentre(const UiFont& font, int16_t centreY)    { return centreY + font.capHeight / 2 - font.ascent; }

static void drawTextCentered(const Rect& r, const char* s, const UiFont& font, uint16_t fg, uint16_t bg) {
  drawText(r.x + (r.w - textWidth(s, font)) / 2, topForCentre(font, r.y + r.h / 2), s, font, fg, bg);
}

// A piece of text that remembers what is on screen. Changing it repaints only the character cells of the
// new text, and clears whatever the old text covered that the new one does not.
struct Field { char shown[24]; uint16_t fg; int16_t x0, x1; };

static void resetField(Field& f) { memset(f.shown, 0, sizeof(f.shown)); f.fg = 0; f.x0 = f.x1 = 0; }

enum Align : uint8_t { ALIGN_LEFT, ALIGN_RIGHT, ALIGN_CENTER };

// The text is placed inside [x, x + width). y is the top of the character cells.
static void drawField(Field& f, int16_t x, int16_t y, int16_t width, const UiFont& font, uint16_t fg, uint16_t bg,
                      const char* text, Align align) {
  if (fg == f.fg && !strncmp(f.shown, text, sizeof(f.shown) - 1)) return;
  int16_t w = textWidth(text, font);
  if (w > width) w = width;                                    // (never happens with the layouts used)
  int16_t nx0 = x + (align == ALIGN_RIGHT ? width - w : align == ALIGN_CENTER ? (width - w) / 2 : 0);
  int16_t nx1 = nx0 + w;
  drawText(nx0, y, text, font, fg, bg);
  if (f.x1 > f.x0) {                                           // clear what the old text covered and the new one does not
    if (f.x0 < nx0) fillRect(f.x0, y, (f.x1 < nx0 ? f.x1 : nx0) - f.x0, font.height, bg);
    if (f.x1 > nx1) fillRect(f.x0 > nx1 ? f.x0 : nx1, y, f.x1 - (f.x0 > nx1 ? f.x0 : nx1), font.height, bg);
  }
  strncpy(f.shown, text, sizeof(f.shown) - 1);
  f.shown[sizeof(f.shown) - 1] = 0;
  f.fg = fg;
  f.x0 = nx0;
  f.x1 = nx1;
}

// A rounded rectangle with smooth (anti-aliased) corners. `behind` is the colour the corners blend into.
// Straight edges are plain rectangles; only the corner pixels are blended, using the usual
// "distance to the arc" estimate. Radius 24 at most.
static void fillRound(const Rect& r, int16_t rad, uint16_t color, uint16_t behind) {
  if (r.w <= 0 || r.h <= 0) return;
  if (rad > r.w / 2) rad = r.w / 2;
  if (rad > r.h / 2) rad = r.h / 2;
  if (rad > 24) rad = 24;
  if (rad <= 0) { fillRect(r.x, r.y, r.w, r.h, color); return; }
  fillRect(r.x + rad, r.y, r.w - 2 * rad, r.h, color);
  fillRect(r.x, r.y + rad, rad, r.h - 2 * rad, color);
  fillRect(r.x + r.w - rad, r.y + rad, rad, r.h - 2 * rad, color);

  const long R4 = 4L * rad * rad;
  uint8_t cover[24];
  for (int16_t j = 0; j < rad; j++) {                          // j: rows in from the outer edge; the other corners mirror this one
    int16_t full = rad;                                        // first column (counting in from the outer edge) that is fully covered
    for (int16_t i = 0; i < rad; i++) {
      long dx2 = 2L * rad - 2 * i - 1, dy2 = 2L * rad - 2 * j - 1;
      long a = 128 + (255L * (R4 - (dx2 * dx2 + dy2 * dy2))) / (8L * rad);
      cover[i] = (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a);
      if (cover[i] == 255 && full == rad) full = i;
    }
    for (uint8_t corner = 0; corner < 4; corner++) {
      bool right = corner & 1, bottom = corner & 2;
      int16_t py = bottom ? r.y + r.h - 1 - j : r.y + j;
      for (int16_t i = 0; i < full; i++) {
        if (cover[i] == 0) continue;
        int16_t px = right ? r.x + r.w - 1 - i : r.x + i;
        gfx.Set_Draw_color(mix565(behind, color, cover[i]));
        gfx.Draw_Pixel(px, py);
      }
      if (full < rad) fillRect(right ? r.x + r.w - rad : r.x + full, py, rad - full, 1, color);
    }
  }
}

static void drawButton(const Rect& r, const char* label, const UiFont& font, uint16_t fill, uint16_t textColor, uint16_t behind) {
  fillRound(r, BUTTON_RADIUS, fill, behind);
  drawTextCentered(r, label, font, textColor, fill);
}

static bool inRect(const Rect& r, int16_t px, int16_t py) {
  return px >= r.x - TOUCH_SLOP_PX && px < r.x + r.w + TOUCH_SLOP_PX &&
         py >= r.y - TOUCH_SLOP_PX && py < r.y + r.h + TOUCH_SLOP_PX;
}

static void drawTitle() {
  drawText(16, topForBaseline(UI_FONT_TITLE, 31), "Cycle Tester", UI_FONT_TITLE, COL_TEXT, COL_BG);
}

// ===========================================================================
// STATE
// ===========================================================================
enum Screen : uint8_t { SCREEN_SETUP, SCREEN_RUN };

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
  ACT_SETUP  = 26        // back to the setup screen
};

// --- Setup screen state ---
static Field setupValueField[NUM_SERVOS];
static Field setupEstimateField;
static uint32_t setupMessageUntil = 0;          // while non-zero, the estimate line shows a warning

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
  int16_t y = SETUP_CARD_Y + row * SETUP_ROW_H + (SETUP_ROW_H - SETUP_BTN_H) / 2;
  int16_t x = SETUP_X0;
  static const int16_t widths[5] = { SETUP_BIG_W, SETUP_SMALL_W, SETUP_VALUE_W, SETUP_SMALL_W, SETUP_BIG_W };
  for (uint8_t p = 0; p < part; p++) x += widths[p] + SETUP_GAP;
  return Rect{ x, y, widths[part], SETUP_BTN_H };
}

static const Rect COPY_BTN  = { MARGIN, SETUP_BOTTOM_Y, 150, SETUP_BOTTOM_H };
static const Rect START_BTN = { 170, SETUP_BOTTOM_Y, 298, SETUP_BOTTOM_H };

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
  uint16_t color = COL_TEXT2;
  if (setupMessageUntil != 0) {
    snprintf(text, sizeof(text), "Set a target first");
    color = COL_RED;
  } else if (longest == 0) {
    snprintf(text, sizeof(text), "No servos selected");
  } else {
    uint32_t sweepMs = (uint32_t)SWEEP_DEG * 1000UL / SERVO_SPEED_DEG_PER_SEC;
    uint32_t cycleMs = 2 * sweepMs + DWELL_AT_END_MS + DWELL_AT_START_MS;
    char dur[16];
    formatDuration((uint64_t)longest * cycleMs / 1000, dur, sizeof(dur));
    snprintf(text, sizeof(text), "Estimated run time %s", dur);
  }
  drawField(setupEstimateField, SETUP_LABEL_X, SETUP_EST_Y, 300, UI_FONT_CAPTION, color, COL_BG, text, ALIGN_LEFT);
}

static void drawSetupValue(uint8_t row) {
  char text[12];
  uint16_t color;
  if (targets[row] == 0) {
    snprintf(text, sizeof(text), "Off");
    color = COL_TEXT3;
  } else {
    snprintf(text, sizeof(text), "%lu", (unsigned long)targets[row]);
    color = COL_TEXT;
  }
  Rect box = setupRect(row, P_VALUE);
  drawField(setupValueField[row], box.x, topForCentre(UI_FONT_NUM, box.y + box.h / 2), box.w, UI_FONT_NUM, color, COL_CARD, text, ALIGN_CENTER);
}

static void drawSetupScreen() {
  screen = SCREEN_SETUP;
  gfx.Fill_Screen(COL_BG);
  drawTitle();
  const char* sub = "Cycles per servo";
  drawText(SCREEN_W - 16 - textWidth(sub, UI_FONT_CAPTION), topForBaseline(UI_FONT_CAPTION, 31), sub, UI_FONT_CAPTION, COL_TEXT2, COL_BG);

  for (uint8_t i = 0; i < NUM_SERVOS; i++) { resetField(setupValueField[i]); }
  resetField(setupEstimateField);

  fillRound(Rect{ MARGIN, SETUP_CARD_Y, SCREEN_W - 2 * MARGIN, NUM_SERVOS * SETUP_ROW_H }, CARD_RADIUS, COL_CARD, COL_BG);

  char label[12], step[8];
  for (uint8_t row = 0; row < NUM_SERVOS; row++) {
    int16_t rowY = SETUP_CARD_Y + row * SETUP_ROW_H;
    if (row > 0) fillRect(SETUP_LABEL_X, rowY, SCREEN_W - MARGIN - SETUP_LABEL_X, 1, COL_SEP);
    snprintf(label, sizeof(label), "Servo %u", (unsigned)(row + 1));
    drawText(SETUP_LABEL_X, topForCentre(UI_FONT_BODY, rowY + SETUP_ROW_H / 2), label, UI_FONT_BODY, COL_TEXT, COL_CARD);

    const SetupPart parts[4] = { P_MINUS_L, P_MINUS_S, P_PLUS_S, P_PLUS_L };
    for (uint8_t k = 0; k < 4; k++) {
      uint32_t amount = (parts[k] == P_MINUS_L || parts[k] == P_PLUS_L) ? STEP_LARGE : STEP_SMALL;
      bool minus = (parts[k] == P_MINUS_L || parts[k] == P_MINUS_S);
      snprintf(step, sizeof(step), "%c%lu", minus ? '-' : '+', (unsigned long)amount);
      drawButton(setupRect(row, parts[k]), step, UI_FONT_BODY, COL_FILL, COL_TEXT, COL_CARD);
    }
    drawSetupValue(row);
  }
  drawEstimate();
  drawButton(COPY_BTN, "Copy S1 to all", UI_FONT_BODY, COL_FILL, COL_BLUE, COL_BG);
  drawButton(START_BTN, "Start", UI_FONT_BODY, COL_BLUE, COL_TEXT, COL_BG);
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

const int16_t RUN_BADGE_D  = 36;       // the round servo number
const int16_t RUN_COUNT_X  = 72;
const int16_t RUN_STATUS_X = 278;

static Rect runPanelRect(uint8_t row)  { return Rect{ MARGIN, runRowY(row), SCREEN_W - 2 * MARGIN, RUN_ROW_H }; }
static Rect runBadgeRect(uint8_t row)  { return Rect{ 24, (int16_t)(runRowY(row) + (RUN_ROW_H - RUN_BADGE_D) / 2), RUN_BADGE_D, RUN_BADGE_D }; }
static Rect runToggleRect(uint8_t row) { return Rect{ 366, (int16_t)(runRowY(row) + 7), 90, 38 }; }
static Rect runBarRect(uint8_t row)    { return Rect{ RUN_COUNT_X, (int16_t)(runRowY(row) + 42), 196, 6 }; }
static const Rect PAUSE_ALL_BTN  = { MARGIN, RUN_BOTTOM_Y, 148, RUN_BOTTOM_H };
static const Rect RESUME_ALL_BTN = { 166,    RUN_BOTTOM_Y, 148, RUN_BOTTOM_H };
static const Rect SETUP_BTN      = { 320,    RUN_BOTTOM_Y, 148, RUN_BOTTOM_H };

static uint16_t statusColor(ChannelStatus s) {
  switch (s) {
    case CH_RUNNING: return COL_BLUE;
    case CH_PAUSED:  return COL_ORANGE;
    case CH_DONE:    return COL_GREEN;
    case CH_FAULT:   return COL_RED;
    default:         return COL_TEXT3;
  }
}

static const char* statusText(ChannelStatus s) {
  switch (s) {
    case CH_RUNNING: return "Running";
    case CH_PAUSED:  return "Paused";
    case CH_DONE:    return "Done";
    case CH_FAULT:   return "Fault";
    default:         return "Off";
  }
}

static void drawRunCount(uint8_t row, uint32_t count) {
  if (runCountChars[row] == 0) return;             // a servo that is OFF shows no counter
  char text[12];
  snprintf(text, sizeof(text), "%lu", (unsigned long)count);
  UiGlyph zero;
  loadGlyph(UI_FONT_NUM, '0', zero);               // all digits are this wide
  drawField(runCountField[row], RUN_COUNT_X, topForBaseline(UI_FONT_NUM, runRowY(row) + 32), runCountChars[row] * zero.adv,
            UI_FONT_NUM, COL_TEXT, COL_CARD, text, ALIGN_LEFT);
}

// The progress bar is a pill: gray track, blue (green when finished) fill. It is repainted whole when it changes.
static void drawRunBar(uint8_t row, const CycleChannel& ch, bool force) {
  if (runCountChars[row] == 0) return;
  Rect bar = runBarRect(row);
  int16_t fill = 0;
  if (ch.target() > 0) fill = (int16_t)((uint64_t)bar.w * ch.count() / ch.target());
  if (fill > bar.w) fill = bar.w;
  if (fill == shownBarFill[row] && !force) return;
  shownBarFill[row] = fill;
  fillRound(bar, bar.h / 2, COL_FILL, COL_CARD);
  if (fill > 0) fillRound(Rect{ bar.x, bar.y, fill, bar.h }, bar.h / 2, ch.status() == CH_DONE ? COL_GREEN : COL_BLUE, COL_FILL);
}

static void drawRunToggle(uint8_t row, ChannelStatus s) {
  Rect r = runToggleRect(row);
  if (s == CH_RUNNING)     drawButton(r, "Pause",  UI_FONT_BODY, COL_ORANGE_TINT, COL_ORANGE, COL_CARD);
  else if (s == CH_PAUSED) drawButton(r, "Resume", UI_FONT_BODY, COL_GREEN_TINT,  COL_GREEN,  COL_CARD);
  else                     fillRect(r.x, r.y, r.w, r.h, COL_CARD);           // nothing to press
}

static void drawRunStatus(uint8_t row, ChannelStatus s) {
  if (s == CH_OFF) return;                      // a servo that is not used shows "Not used" instead
  uint16_t color = statusColor(s);
  int16_t cy = runRowY(row) + RUN_ROW_H / 2;
  fillRound(Rect{ RUN_STATUS_X, (int16_t)(cy - 4), 8, 8 }, 4, color, COL_CARD);                   // status dot
  drawField(runStatusField[row], RUN_STATUS_X + 14, topForCentre(UI_FONT_BODY, cy), 66, UI_FONT_BODY, color, COL_CARD, statusText(s), ALIGN_LEFT);
}

static void drawRunStatic() {
  screen = SCREEN_RUN;
  gfx.Fill_Screen(COL_BG);
  drawTitle();
  resetField(runOverallField);

  for (uint8_t row = 0; row < NUM_SERVOS; row++) {
    resetField(runCountField[row]);
    resetField(runStatusField[row]);
    shownCount[row] = 0xFFFFFFFFUL;           // impossible values, so the first refresh paints everything
    shownStatus[row] = 255;
    shownBarFill[row] = -1;

    fillRound(runPanelRect(row), CARD_RADIUS, COL_CARD, COL_BG);
    Rect badge = runBadgeRect(row);
    bool on = targets[row] > 0;
    fillRound(badge, RUN_BADGE_D / 2, on ? COL_BLUE_TINT : COL_FILL, COL_CARD);
    char label[4];
    snprintf(label, sizeof(label), "%u", (unsigned)(row + 1));
    drawTextCentered(badge, label, UI_FONT_BODY, on ? COL_BLUE : COL_TEXT3, on ? COL_BLUE_TINT : COL_FILL);

    uint8_t digits = 0;                                 // how many digits this row's target has
    for (uint32_t t = targets[row]; t > 0; t /= 10) digits++;
    runCountChars[row] = digits;
    if (digits > 0) {
      char of[20];
      snprintf(of, sizeof(of), "of %lu", (unsigned long)targets[row]);
      Rect bar = runBarRect(row);                       // "of 1000" sits at the right-hand end of the progress bar
      drawText(bar.x + bar.w - textWidth(of, UI_FONT_CAPTION), topForBaseline(UI_FONT_CAPTION, runRowY(row) + 32), of, UI_FONT_CAPTION, COL_TEXT2, COL_CARD);
    } else {
      drawText(RUN_COUNT_X, topForCentre(UI_FONT_BODY, runRowY(row) + RUN_ROW_H / 2), "Not used", UI_FONT_BODY, COL_TEXT3, COL_CARD);
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
// It is called once per pass of loop(), so a big change such as Pause All is painted over a handful of
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
      drawRunStatus(row, st);
      drawRunToggle(row, st);
    }
    drawRunBar(row, ch, statusChanged);         // also repainted when the status changes, so a finished bar turns green
    return;
  }

  OverallState ov = overallState();
  if ((uint8_t)ov != shownOverall) {
    shownOverall = (uint8_t)ov;
    const char* text = (ov == OV_RUNNING) ? "Running" : (ov == OV_PAUSED) ? "Paused" : (ov == OV_COMPLETE) ? "Complete" : "Fault";
    uint16_t color = (ov == OV_RUNNING) ? COL_BLUE : (ov == OV_PAUSED) ? COL_ORANGE : (ov == OV_COMPLETE) ? COL_GREEN : COL_RED;
    drawField(runOverallField, SCREEN_W - 16 - 120, topForBaseline(UI_FONT_BODY, 31), 120, UI_FONT_BODY, color, COL_BG, text, ALIGN_RIGHT);
    return;
  }

  // Pause All is live while anything is running, Resume All while anything is paused.
  bool canPause  = ctl->countWithStatus(CH_RUNNING) > 0;
  bool canResume = ctl->countWithStatus(CH_PAUSED) > 0;
  if (shownPauseAll != canPause) {
    shownPauseAll = canPause;
    drawButton(PAUSE_ALL_BTN, "Pause All", UI_FONT_BODY, canPause ? COL_ORANGE_TINT : COL_CARD, canPause ? COL_ORANGE : COL_TEXT3, COL_BG);
    return;
  }
  if (shownResumeAll != canResume) {
    shownResumeAll = canResume;
    drawButton(RESUME_ALL_BTN, "Resume All", UI_FONT_BODY, canResume ? COL_GREEN_TINT : COL_CARD, canResume ? COL_GREEN : COL_TEXT3, COL_BG);
    return;
  }

  SetupBtnState sb = confirmPending ? SB_CONFIRM : (ctl->allFinished() ? SB_NEW_TEST : SB_SETUP);
  if ((uint8_t)sb != shownSetupBtn) {
    shownSetupBtn = (uint8_t)sb;
    if (sb == SB_CONFIRM)       drawButton(SETUP_BTN, "Sure?",    UI_FONT_BODY, COL_RED,  COL_TEXT, COL_BG);
    else if (sb == SB_NEW_TEST) drawButton(SETUP_BTN, "New Test", UI_FONT_BODY, COL_BLUE, COL_TEXT, COL_BG);
    else                        drawButton(SETUP_BTN, "Setup",    UI_FONT_BODY, COL_FILL, COL_TEXT, COL_BG);
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
    if (inRect(COPY_BTN, x, y))  return ACT_COPY;
    if (inRect(START_BTN, x, y)) return ACT_START;
  } else {
    for (uint8_t row = 0; row < NUM_SERVOS; row++)
      if (inRect(runToggleRect(row), x, y)) return (Action)(ACT_TOGGLE + row);
    if (inRect(PAUSE_ALL_BTN, x, y))  return ACT_PAUSE_ALL;
    if (inRect(RESUME_ALL_BTN, x, y)) return ACT_RESUME_ALL;
    if (inRect(SETUP_BTN, x, y))      return ACT_SETUP;
  }
  return ACT_NONE;
}

static bool isRepeating(Action a) { return a >= ACT_ADJUST && a < ACT_COPY; }

static void enterSetup() {
  drawButton(SETUP_BTN, "Wait...", UI_FONT_BODY, COL_FILL, COL_TEXT2, COL_BG);   // instant feedback: the full redraw takes a couple of seconds
  ctl->abort();
  drawSetupScreen();
}

static void perform(Action a, uint32_t now) {
  if (a != ACT_SETUP) confirmPending = false;      // tapping anything else cancels a pending "SURE?"

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
      setupMessageUntil = now + 2000;
      drawEstimate();
      return;
    }
    drawButton(START_BTN, "Starting...", UI_FONT_BODY, COL_FILL, COL_TEXT2, COL_BG);   // instant feedback: the full redraw takes a couple of seconds
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
    drawText(16, 40, "Touch controller not found", UI_FONT_TITLE, COL_RED, COL_BG);
    drawText(16, 84, "Check SDA, SCL and the CTP_INT", UI_FONT_BODY, COL_TEXT, COL_BG);
    drawText(16, 108, "wire (pin 2), then wait here.", UI_FONT_BODY, COL_TEXT, COL_BG);
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
  } else {
    refreshRunStep(now);
  }
}
