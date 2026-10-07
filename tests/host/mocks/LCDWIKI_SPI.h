// Fake display for the PC build: draws into a 480x320 RGB565 frame buffer so screens can be saved as
// pictures, and "charges" simulated time for every drawing call using the real library's costs:
//   hardware SPI at 4 MHz, two SPI.transfer() calls per pixel  ->  about 7 us per pixel,
//   plus about 45 us to set the address window for every rectangle.
// The rounded-rectangle routines are ported from LCDWIKI_GUI.cpp so shapes match the real thing.
#pragma once
#include "Arduino.h"
#include <vector>
#include <string>

#define ST7796S 14

class LCDWIKI_SPI {
 public:
  static const int W = 480, H = 320;
  static constexpr double US_PER_PIXEL = 7.0;
  static constexpr double US_PER_WINDOW = 45.0;

  LCDWIKI_SPI(uint16_t, int8_t, int8_t, int8_t, int8_t) : fb_(W * H, 0), drawColor_(0) {}
  void Init_LCD() {}
  void Set_Rotation(uint8_t) {}
  void Set_Draw_color(uint16_t c) { drawColor_ = c; }

  void Fill_Rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (w < 0) { w = -w; x -= w; }
    if (h < 0) { h = -h; y -= h; }
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > W ? W : x + w, y1 = y + h > H ? H : y + h;
    if (x1 <= x0 || y1 <= y0) return;
    for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) fb_[yy * W + xx] = color;
    charge(1, (double)(x1 - x0) * (y1 - y0));
  }
  void Fill_Screen(uint16_t color) { Fill_Rect(0, 0, W, H, color); }

  void Draw_Fast_HLine(int16_t x, int16_t y, int16_t w) { Fill_Rect(x, y, w, 1, drawColor_); }
  void Draw_Fast_VLine(int16_t x, int16_t y, int16_t h) { Fill_Rect(x, y, 1, h, drawColor_); }
  void Draw_Pixel(int16_t x, int16_t y) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    fb_[y * W + x] = drawColor_;
    charge(1, 1);
  }

  void Fill_Round_Rectangle(int16_t x1, int16_t y1, int16_t x2, int16_t y2, int16_t radius) {
    int w = x2 - x1 + 1, h = y2 - y1 + 1;
    if (w < 0) { x1 = x2; w = -w; }
    if (h < 0) { y1 = y2; h = -h; }
    Fill_Rect(x1 + radius, y1, w - 2 * radius, h, drawColor_);
    fillCircleHelper(x1 + w - radius - 1, y1 + radius, radius, 1, h - 2 * radius - 1);
    fillCircleHelper(x1 + radius, y1 + radius, radius, 2, h - 2 * radius - 1);
  }
  void Draw_Round_Rectangle(int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint8_t radius) {
    int w = x2 - x1 + 1, h = y2 - y1 + 1;
    if (w < 0) { x1 = x2; w = -w; }
    if (h < 0) { y1 = y2; h = -h; }
    Draw_Fast_HLine(x1 + radius, y1, w - 2 * radius);
    Draw_Fast_HLine(x1 + radius, y1 + h - 1, w - 2 * radius);
    Draw_Fast_VLine(x1, y1 + radius, h - 2 * radius);
    Draw_Fast_VLine(x1 + w - 1, y1 + radius, h - 2 * radius);
    drawCircleHelper(x1 + radius, y1 + radius, radius, 1);
    drawCircleHelper(x1 + w - radius - 1, y1 + radius, radius, 2);
    drawCircleHelper(x1 + w - radius - 1, y1 + h - radius - 1, radius, 4);
    drawCircleHelper(x1 + radius, y1 + h - radius - 1, radius, 8);
  }

  // ---- test side ----
  uint16_t pixel(int x, int y) const { return fb_[y * W + x]; }
  double totalDrawUs = 0;
  unsigned long totalRects = 0;
  bool savePpm(const char* path) const {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
      uint16_t c = fb_[i];
      uint8_t rgb[3] = { (uint8_t)(((c >> 11) & 0x1F) * 255 / 31), (uint8_t)(((c >> 5) & 0x3F) * 255 / 63), (uint8_t)((c & 0x1F) * 255 / 31) };
      fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return true;
  }

 private:
  void charge(int windows, double pixels) {
    double us = windows * US_PER_WINDOW + pixels * US_PER_PIXEL;
    totalDrawUs += us;
    totalRects++;
    carry_ += us;
    uint64_t whole = (uint64_t)carry_;
    if (whole) { carry_ -= whole; simAdvanceUs(whole); }
  }
  void fillCircleHelper(int16_t x0, int16_t y0, int16_t r, uint8_t corner, int16_t delta) {
    int16_t f = 1 - r, ddF_x = 1, ddF_y = -2 * r, x = 0, y = r;
    while (x < y) {
      if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
      x++; ddF_x += 2; f += ddF_x;
      if (corner & 0x1) { Draw_Fast_VLine(x0 + x, y0 - y, 2 * y + 1 + delta); Draw_Fast_VLine(x0 + y, y0 - x, 2 * x + 1 + delta); }
      if (corner & 0x2) { Draw_Fast_VLine(x0 - x, y0 - y, 2 * y + 1 + delta); Draw_Fast_VLine(x0 - y, y0 - x, 2 * x + 1 + delta); }
    }
  }
  void drawCircleHelper(int16_t x0, int16_t y0, int16_t r, uint8_t corner) {
    int16_t f = 1 - r, ddF_x = 1, ddF_y = -2 * r, x = 0, y = r;
    while (x < y) {
      if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
      x++; ddF_x += 2; f += ddF_x;
      if (corner & 0x4) { Draw_Pixel(x0 + x, y0 + y); Draw_Pixel(x0 + y, y0 + x); }
      if (corner & 0x2) { Draw_Pixel(x0 + x, y0 - y); Draw_Pixel(x0 + y, y0 - x); }
      if (corner & 0x8) { Draw_Pixel(x0 - y, y0 + x); Draw_Pixel(x0 - x, y0 + y); }
      if (corner & 0x1) { Draw_Pixel(x0 - y, y0 - x); Draw_Pixel(x0 - x, y0 - y); }
    }
  }
  std::vector<uint16_t> fb_;
  uint16_t drawColor_;
  double carry_ = 0;
};
