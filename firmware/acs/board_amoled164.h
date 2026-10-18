// Waveshare ESP32-S3-Touch-AMOLED-1.64: CO5300 280x456 AMOLED over QSPI, FT3168 touch, BOOT as the only button.
// Pins and panel quirks from waveshareteam/ESP32-S3-Touch-AMOLED-1.64-v2 (Arduino/examples/06_LVGL_Test):
// column offset 0x14, brightness by DCS command 0x51 (no backlight pin), FT3168 at I2C 0x38.
//
// Every screen is drawn at panel resolution into one PSRAM canvas. Layout and touch zones are written in logical
// 196x320 coordinates (the XIAO layout, widened) and placed on the panel through NP(), which scales them 1.425x.
#pragma once
#define U8G2_USE_LARGE_FONTS                                // the full CJK Unifont below needs it
#include <U8g2lib.h>                                         // makes Arduino_GFX enable its u8g2 (CJK) fonts
#include <Arduino_GFX_Library.h>
// (Arduino_GFX includes its CJK Unifont when the U8g2 library is installed; 16 px, OFL / GPL + font exception)
#include <Wire.h>
#include "fonts/FreeSansBold12pt7b.h"
#include "fonts/FreeSansBold9pt7b.h"
#include "fonts/FreeSans9pt7b.h"
#include "fonts/FreeSansBold18pt7b.h"
#include "fonts/FreeSans12pt7b.h"

#define BOARD_NAME "waveshare-amoled-164"
static constexpr int W = 196, H = 320;                    // logical layout size: 280x456 / 1.425
static constexpr int PANEL_W = 280, PANEL_H = 456;
static constexpr uint8_t BTN_A_PIN = 0;                     // BOOT: hold = allow; short = next page / release
static constexpr uint8_t BTN_B_PIN = 0;
static constexpr bool HAS_BTN_B = false;

#ifdef AMOLED_REV1                                           // first revision: CS on 9, touch INT on 46
static constexpr int8_t QSPI_CS = 9;
#else
static constexpr int8_t QSPI_CS = 46;
#endif
static constexpr int8_t QSPI_SCK = 10, QSPI_D0 = 11, QSPI_D1 = 12, QSPI_D2 = 13, QSPI_D3 = 14, LCD_RST = 21;
static constexpr int8_t TP_SDA = 47, TP_SCL = 48;
static constexpr uint8_t TP_ADDR = 0x38, IMU_ADDR = 0x6B;   // FT3168 touch and QMI8658 IMU share the bus
static constexpr int8_t BAT_ADC = 4;                         // ADC1 channel 3, 1/3 divider

Arduino_DataBus *qspi = new Arduino_ESP32QSPI(QSPI_CS, QSPI_SCK, QSPI_D0, QSPI_D1, QSPI_D2, QSPI_D3);
Arduino_CO5300 *panel = new Arduino_CO5300(qspi, LCD_RST, 0, PANEL_W, PANEL_H, 20, 0, 0, 0);

// The subset of the Seeed_GFX API the UI uses, drawing into a full-resolution canvas.
class AmoledDisplay {
 public:
  Arduino_Canvas canvas{PANEL_W, PANEL_H, nullptr};
  bool changed = true;

  bool begin() {
    if (!canvas.begin(GFX_SKIP_OUTPUT_BEGIN)) return false;
    canvas.setTextWrap(false);
    return true;
  }
  void fillScreen(uint16_t c) { canvas.fillScreen(c); changed = true; }
  void fillRect(int x, int y, int w, int h, uint16_t c) { canvas.fillRect(x, y, w, h, c); changed = true; }
  void fillRoundRect(int x, int y, int w, int h, int r, uint16_t c) { canvas.fillRoundRect(x, y, w, h, r, c); changed = true; }
  void drawFastHLine(int x, int y, int w, uint16_t c) { canvas.drawFastHLine(x, y, w, c); changed = true; }
  void fillCircle(int x, int y, int r, uint16_t c) { canvas.fillCircle(x, y, r, c); changed = true; }
  void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) {
    canvas.fillTriangle(x0, y0, x1, y1, x2, y2, c);
    changed = true;
  }
  void pushImage(int x, int y, int w, int h, const uint16_t *d) {
    canvas.draw16bitRGBBitmap(x, y, (uint16_t *)d, w, h);
    changed = true;
  }
  void setTextSize(uint8_t s) { size_ = s; canvas.setTextSize(s); }
  void setTextColor(uint16_t fg) { canvas.setTextColor(fg); }
  void setTextColor(uint16_t fg, uint16_t bg) { canvas.setTextColor(fg, bg); }
  void setTextFont(int) { setFreeFont(nullptr); }
  // Unifont for UTF-8 text (song titles); any setFreeFont/setTextFont call leaves this mode
  void setCjk(uint8_t size) {
    cjk_ = true;
    font_ = nullptr;
    size_ = size;
    canvas.setFont(u8g2_font_unifont_h_cjk);
    canvas.setUTF8Print(true);
    canvas.setTextSize(size);
    int16_t x1, y1;
    uint16_t w, h;
    canvas.getTextBounds("国Ag", 0, 0, &x1, &y1, &w, &h);
    ascent_ = -y1 / size;
  }
  void setFreeFont(const GFXfont *f) {
    if (cjk_) {
      cjk_ = false;
      canvas.setUTF8Print(false);
    }
    font_ = f;
    canvas.setFont(f);
    ascent_ = 0;
    if (f)
      for (uint16_t c = f->first; c <= f->last; c++) ascent_ = max(ascent_, -(int)f->glyph[c - f->first].yOffset);
  }
  int textWidth(const char *s) {
    if (cjk_) {
      int16_t x1, y1;
      uint16_t w, h;
      canvas.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
      return w;
    }
    if (!font_) return strlen(s) * 6 * size_;
    int w = 0;
    for (; *s; s++)
      if ((uint8_t)*s >= font_->first && (uint8_t)*s <= font_->last) w += font_->glyph[(uint8_t)*s - font_->first].xAdvance;
    return w * size_;
  }
  // Top-left anchored, like Seeed_GFX's default datum (free fonts: the cursor is the baseline)
  void drawString(const char *s, int x, int y) {
    canvas.setCursor(x, font_ || cjk_ ? y + ascent_ * size_ : y);
    canvas.print(s);
    changed = true;
  }
  // Touch in logical coordinates, the same space the touch zones are written in
  bool getTouch(int32_t *x, int32_t *y) {
    uint8_t n = 0, b[4];
    if (!readReg(0x02, &n, 1) || !(n & 0x0F) || !readReg(0x03, b, 4)) return false;
    int nx = ((b[0] & 0x0F) << 8) | b[1], ny = ((b[2] & 0x0F) << 8) | b[3];
    *x = constrain(nx * W / PANEL_W, 0, W - 1);
    *y = constrain(ny * H / PANEL_H, 0, H - 1);
    return true;
  }

 private:
  const GFXfont *font_ = nullptr;
  bool cjk_ = false;
  int ascent_ = 0;
  uint8_t size_ = 1;
  static bool readReg(uint8_t reg, uint8_t *buf, uint8_t len) {
    Wire.beginTransmission(TP_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(TP_ADDR, len) != len) return false;
    for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
    return true;
  }
};

AmoledDisplay display;

static void imuWrite(uint8_t reg, uint8_t v) {
  Wire.beginTransmission(IMU_ADDR);
  Wire.write(reg);
  Wire.write(v);
  Wire.endTransmission();
}

// Acceleration in g. False when the IMU does not answer.
bool boardImu(float &ax, float &ay, float &az) {
  Wire.beginTransmission(IMU_ADDR);
  Wire.write(0x35);                                          // AX_L .. AZ_H, little endian
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(IMU_ADDR, (uint8_t)6) != 6) return false;
  int16_t v[3];
  for (int i = 0; i < 3; i++) v[i] = Wire.read() | (Wire.read() << 8);
  ax = v[0] / 8192.0f;                                       // 8192 LSB/g at +-4 g
  ay = v[1] / 8192.0f;
  az = v[2] / 8192.0f;
  return true;
}

bool boardBegin() {
  analogReadResolution(12);
  if (!panel->begin()) return false;
  panel->fillScreen(0x0000);
  if (!display.begin()) return false;
  Wire.begin(TP_SDA, TP_SCL, 300000);
  Wire.beginTransmission(TP_ADDR);                           // FT3168: normal operating mode
  Wire.write(0x00);
  Wire.write(0x00);
  Wire.endTransmission();
  imuWrite(0x02, 0x40);                                      // CTRL1: register address auto-increment
  imuWrite(0x03, 0x17);                                      // CTRL2: accelerometer +-4 g, 62.5 Hz
  imuWrite(0x08, 0x01);                                      // CTRL7: accelerometer on
  return true;
}

void setBacklight(uint8_t level) { panel->setBrightness(level); }

// Main screen ("rail") geometry at panel resolution
void useNativeRail(bool) {}                                   // every screen is native here
static constexpr int RW = PANEL_W, RH = PANEL_H;
static constexpr int RAIL_D = 128, RAIL_TOP = 10, RAIL_STEP = 186, RAIL_LINE_DY = 34, RAIL_STATUS_Y = RH - 74,
                     RAIL_CAT_SCALE = 3, RAIL_TEXT_X = 72, RAIL_FOOT_Y = RH - 24, RAIL_PCT_H = 30;
#define RAIL_PCT_FONT (&FreeSansBold18pt7b)
#define RAIL_SMALL_FONT (&FreeSans9pt7b)
#define RAIL_STATE_FONT (&FreeSansBold9pt7b)
#define RAIL_LARGE_ICONS 1
#define HAS_CJK 1                                            // song titles in any script (MP3 page)
static constexpr int CJK_SIZE = 2;                            // 16 px Unifont at 2x = 32 px

// Permission screen and other pages: the logical layout x1.425 (NS), so touch zones are unchanged
struct TextStyle { const GFXfont *font; uint8_t size; };   // font == nullptr: GLCD 5x7 at `size`
static constexpr float NS = (float)PANEL_H / H;
static const TextStyle P_LABEL{&FreeSansBold9pt7b, 1}, P_SMALL{&FreeSans9pt7b, 1}, P_TITLE{&FreeSansBold12pt7b, 1},
                       P_BTN{&FreeSansBold12pt7b, 1}, P_DENY{&FreeSansBold12pt7b, 1};
static constexpr int P_CMD_BIG = 3, P_CMD_SMALL = 2, P_CAT_SCALE = 4;
static constexpr const char *BTN_A_NAME = "BOOT", *BTN_B_NAME = "BOOT";
bool nativeNow() { return true; }
// Other pages: titles, body text, the cat (one size up from the XIAO)
static const TextStyle S_TITLE{&FreeSansBold18pt7b, 1}, S_HEAD{&FreeSansBold12pt7b, 1}, S_BODY{&FreeSans12pt7b, 1},
                       S_NOTE{&FreeSans9pt7b, 1};
static constexpr int CAT_SCALE = 9;

// Burn-in protection: the whole image moves by a few pixels every 3 minutes, so static UI (ring tracks, labels)
// does not wear the same OLED pixels for hours. Offsets are even because CO5300 windows start on even pixels.
static const int8_t SHIFT[][2] = {{0, 0}, {2, 0}, {2, 2}, {0, 2}, {-2, 2}, {-2, 0}, {-2, -2}, {0, -2}, {2, -2}, {4, 0}, {0, 4}, {-4, 0}, {0, -4}};
static constexpr uint32_t SHIFT_EVERY_MS = 180000;
static uint16_t *shifted = nullptr;

// Send the canvas to the panel when anything was drawn since the last frame (or the shift moved on).
void boardPresent() {
  static int lastShift = -1;
  int k = (millis() / SHIFT_EVERY_MS) % (sizeof(SHIFT) / sizeof(SHIFT[0]));
  if (!display.changed && k == lastShift) return;
  display.changed = false;
  lastShift = k;
  uint16_t *src = display.canvas.getFramebuffer();
  const int dx = SHIFT[k][0], dy = SHIFT[k][1];
  if (dx || dy) {
    if (!shifted) shifted = (uint16_t *)ps_malloc(PANEL_W * PANEL_H * 2);
    if (shifted) {
      for (int y = 0; y < PANEL_H; y++) {
        uint16_t *out = shifted + y * PANEL_W;
        int sy = y - dy;
        if (sy < 0 || sy >= PANEL_H) { memset(out, 0, PANEL_W * 2); continue; }
        const uint16_t *in = src + sy * PANEL_W;
        if (dx >= 0) { memset(out, 0, dx * 2); memcpy(out + dx, in, (PANEL_W - dx) * 2); }
        else { memcpy(out, in - dx, (PANEL_W + dx) * 2); memset(out + PANEL_W + dx, 0, -dx * 2); }
      }
      src = shifted;
    }
  }
  panel->draw16bitRGBBitmap(0, 0, src, PANEL_W, PANEL_H);
}

bool boardBattery(float &vbat, bool &charging) {
  uint32_t mv = 0;
  for (int i = 0; i < 16; i++) mv += analogReadMilliVolts(BAT_ADC);
  vbat = mv / 16.0f * 3 / 1000.0f;
  charging = false;                                          // no charge-status pin on this board
  return vbat > 3.0f && vbat < 4.3f;                         // above that is the USB rail, not a cell
}
