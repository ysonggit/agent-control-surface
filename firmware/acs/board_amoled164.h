// Waveshare ESP32-S3-Touch-AMOLED-1.64: CO5300 280x456 AMOLED over QSPI, FT3168 touch, BOOT as the only button.
// Pins and panel quirks from waveshareteam/ESP32-S3-Touch-AMOLED-1.64-v2 (Arduino/examples/06_LVGL_Test):
// column offset 0x14, brightness by DCS command 0x51 (no backlight pin), FT3168 at I2C 0x38.
//
// The UI is drawn on a 196x320 logical canvas (the XIAO layout, widened) and scaled 1.425x with bilinear
// filtering to the full 280x456 panel, so every screen works on both boards and circles stay round.
#pragma once
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include "fonts/FreeSansBold12pt7b.h"
#include "fonts/FreeSansBold9pt7b.h"
#include "fonts/FreeSans9pt7b.h"
#include "fonts/FreeSansBold18pt7b.h"

#define BOARD_NAME "waveshare-amoled-164"
static constexpr int W = 196, H = 320;                    // logical UI size: 280x456 / 1.425, so it fills the panel
static constexpr int PANEL_W = 280, PANEL_H = 456;
static constexpr int OUT_X = 0, OUT_W = PANEL_W;            // CO5300 windows must start on even x and be even wide
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
static constexpr uint8_t TP_ADDR = 0x38;
static constexpr int8_t BAT_ADC = 4;                         // ADC1 channel 3, 1/3 divider

Arduino_DataBus *qspi = new Arduino_ESP32QSPI(QSPI_CS, QSPI_SCK, QSPI_D0, QSPI_D1, QSPI_D2, QSPI_D3);
Arduino_CO5300 *panel = new Arduino_CO5300(qspi, LCD_RST, 0, PANEL_W, PANEL_H, 20, 0, 0, 0);

// The subset of the Seeed_GFX API the UI uses, drawing into the logical canvas.
// Two drawing targets: the logical canvas (scaled to the panel) and a native 280x456 canvas used by the main screen.
class AmoledDisplay {
 public:
  Arduino_Canvas canvas{W, H, nullptr};
  Arduino_Canvas ncanvas{PANEL_W, PANEL_H, nullptr};
  Arduino_GFX *g = &canvas;
  bool native = false, changed = true;

  bool begin() {
    if (!canvas.begin(GFX_SKIP_OUTPUT_BEGIN) || !ncanvas.begin(GFX_SKIP_OUTPUT_BEGIN)) return false;
    canvas.setTextWrap(false);
    ncanvas.setTextWrap(false);
    return true;
  }
  void useNative(bool on) {
    if (on != native) changed = true;
    native = on;
    g = on ? (Arduino_GFX *)&ncanvas : (Arduino_GFX *)&canvas;
  }
  void fillScreen(uint16_t c) { g->fillScreen(c); changed = true; }
  void fillRect(int x, int y, int w, int h, uint16_t c) { g->fillRect(x, y, w, h, c); changed = true; }
  void fillRoundRect(int x, int y, int w, int h, int r, uint16_t c) { g->fillRoundRect(x, y, w, h, r, c); changed = true; }
  void drawFastHLine(int x, int y, int w, uint16_t c) { g->drawFastHLine(x, y, w, c); changed = true; }
  void pushImage(int x, int y, int w, int h, const uint16_t *d) {
    g->draw16bitRGBBitmap(x, y, (uint16_t *)d, w, h);
    changed = true;
  }
  // text state is kept identical on both targets
  void setTextSize(uint8_t s) { size_ = s; canvas.setTextSize(s); ncanvas.setTextSize(s); }
  void setTextColor(uint16_t fg) { canvas.setTextColor(fg); ncanvas.setTextColor(fg); }
  void setTextColor(uint16_t fg, uint16_t bg) { canvas.setTextColor(fg, bg); ncanvas.setTextColor(fg, bg); }
  void setTextFont(int) { setFreeFont(nullptr); }
  void setFreeFont(const GFXfont *f) {
    font_ = f;
    canvas.setFont(f);
    ncanvas.setFont(f);
    ascent_ = 0;
    if (f)
      for (uint16_t c = f->first; c <= f->last; c++) ascent_ = max(ascent_, -(int)f->glyph[c - f->first].yOffset);
  }
  int textWidth(const char *s) {
    if (!font_) return strlen(s) * 6 * size_;
    int w = 0;
    for (; *s; s++)
      if ((uint8_t)*s >= font_->first && (uint8_t)*s <= font_->last) w += font_->glyph[(uint8_t)*s - font_->first].xAdvance;
    return w * size_;
  }
  // Top-left anchored, like Seeed_GFX's default datum (free fonts: the cursor is the baseline)
  void drawString(const char *s, int x, int y) {
    g->setCursor(x, font_ ? y + ascent_ * size_ : y);
    g->print(s);
    changed = true;
  }
  bool getTouch(int32_t *x, int32_t *y) {
    uint8_t n = 0, b[4];
    if (!readReg(0x02, &n, 1) || !(n & 0x0F) || !readReg(0x03, b, 4)) return false;
    int nx = ((b[0] & 0x0F) << 8) | b[1], ny = ((b[2] & 0x0F) << 8) | b[3];
    *x = constrain((nx - OUT_X) * W / OUT_W, 0, W - 1);
    *y = constrain(ny * H / PANEL_H, 0, H - 1);
    return true;
  }

 private:
  const GFXfont *font_ = nullptr;
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
static uint16_t *frame = nullptr;                            // scaled output, OUT_W x PANEL_H, in PSRAM
static uint16_t sxIdx[OUT_W];
static uint8_t sxFrac[OUT_W];

bool boardBegin() {
  analogReadResolution(12);
  if (!panel->begin()) return false;
  panel->fillScreen(0x0000);
  if (!display.begin()) return false;
  frame = (uint16_t *)ps_malloc(OUT_W * PANEL_H * 2);
  if (!frame) return false;
  for (int x = 0; x < OUT_W; x++) {                          // horizontal sample positions, 8.8 fixed point
    uint32_t f = (uint32_t)x * ((W - 1) << 8) / (OUT_W - 1);
    sxIdx[x] = min<uint32_t>(f >> 8, W - 2);
    sxFrac[x] = f >> 8 >= W - 1 ? 255 : f & 0xFF;
  }
  Wire.begin(TP_SDA, TP_SCL, 300000);
  Wire.beginTransmission(TP_ADDR);                           // FT3168: normal operating mode
  Wire.write(0x00);
  Wire.write(0x00);
  Wire.endTransmission();
  return true;
}

void setBacklight(uint8_t level) { panel->setBrightness(level); }

// Main screen ("rail") at native resolution
void useNativeRail(bool on) { display.useNative(on); }
static constexpr int RW = PANEL_W, RH = PANEL_H;
static constexpr int RAIL_D = 128, RAIL_TOP = 10, RAIL_STEP = 186, RAIL_LINE_DY = 34, RAIL_STATUS_Y = RH - 74,
                     RAIL_CAT_SCALE = 3, RAIL_TEXT_X = 72, RAIL_FOOT_Y = RH - 24, RAIL_PCT_H = 30;
#define RAIL_PCT_FONT (&FreeSansBold18pt7b)
#define RAIL_SMALL_FONT (&FreeSans9pt7b)
#define RAIL_STATE_FONT (&FreeSansBold9pt7b)
#define RAIL_LARGE_ICONS 1

// Permission screen: the logical layout x1.425 at native resolution, so touch zones are unchanged
struct TextStyle { const GFXfont *font; uint8_t size; };   // font == nullptr: GLCD 5x7 at `size`
static constexpr float NS = (float)PANEL_H / H;
static const TextStyle P_LABEL{&FreeSansBold9pt7b, 1}, P_SMALL{&FreeSans9pt7b, 1}, P_TITLE{&FreeSansBold12pt7b, 1},
                       P_BTN{&FreeSansBold12pt7b, 1}, P_DENY{&FreeSansBold12pt7b, 1};
static constexpr int P_CMD_BIG = 3, P_CMD_SMALL = 2, P_CAT_SCALE = 4;
static constexpr const char *BTN_A_NAME = "BOOT", *BTN_B_NAME = "BOOT";
bool nativeNow() { return display.native; }

static inline uint16_t lerp565(uint16_t a, uint16_t b, uint8_t t) {
  if (a == b || t == 0) return a;
  if (t == 255) return b;
  int r = ((a >> 11) * (255 - t) + (b >> 11) * t) / 255;
  int g = (((a >> 5) & 63) * (255 - t) + ((b >> 5) & 63) * t) / 255;
  int bl = ((a & 31) * (255 - t) + (b & 31) * t) / 255;
  return (r << 11) | (g << 5) | bl;
}

// Scale the logical canvas to the panel when anything was drawn since the last frame.
void boardPresent() {
  if (!display.changed || !frame) return;
  display.changed = false;
  if (display.native) {                                      // main screen: already at panel resolution
    panel->draw16bitRGBBitmap(0, 0, display.ncanvas.getFramebuffer(), PANEL_W, PANEL_H);
    return;
  }
  const uint16_t *src = display.canvas.getFramebuffer();
  for (int y = 0; y < PANEL_H; y++) {
    uint32_t fy = (uint32_t)y * ((H - 1) << 8) / (PANEL_H - 1);
    int y0 = min<uint32_t>(fy >> 8, H - 2);
    uint8_t ty = fy >> 8 >= H - 1 ? 255 : fy & 0xFF;
    const uint16_t *r0 = src + y0 * W, *r1 = r0 + W;
    uint16_t *out = frame + y * OUT_W;
    for (int x = 0; x < OUT_W; x++) {
      int i = sxIdx[x];
      uint8_t tx = sxFrac[x];
      out[x] = lerp565(lerp565(r0[i], r0[i + 1], tx), lerp565(r1[i], r1[i + 1], tx), ty);
    }
  }
  panel->draw16bitRGBBitmap(OUT_X, 0, frame, OUT_W, PANEL_H);
}

bool boardBattery(float &vbat, bool &charging) {
  uint32_t mv = 0;
  for (int i = 0; i < 16; i++) mv += analogReadMilliVolts(BAT_ADC);
  vbat = mv / 16.0f * 3 / 1000.0f;
  charging = false;                                          // no charge-status pin on this board
  return vbat > 3.0f && vbat < 4.3f;                         // above that is the USB rail, not a cell
}
