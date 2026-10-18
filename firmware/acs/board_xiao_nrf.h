// Seeed XIAO nRF52840 Plus + 1.47" JD9853A/AXS5106L touch display (172x320).
// Bring-up follows Seeed_GFX2's "1.47 inch Touch Display/nRF52840 Plus/Touch" example.
#pragma once
#include <Seeed_GFX.h>
#include "board/boards/XIAO_LCD_Board.h"
#include "driver/tft/Driver_JD9853A.h"
#include "panel/Panel_TFT.h"
#include "touch/Touch_AXS5106L.h"
#include <nrf.h>
#include <nrf_gpio.h>
#include <font/GFXFF/FreeSansBold12pt7b.h>
#include <font/GFXFF/FreeSansBold9pt7b.h>
#include <font/GFXFF/FreeSans9pt7b.h>

#define BOARD_NAME "xiao-nrf52840-147"
static constexpr int W = 172, H = 320;
static constexpr uint8_t BTN_A_PIN = D19;   // USR1: hold = allow
static constexpr uint8_t BTN_B_PIN = D15;   // USR2: short = deny, hold = deny + stop
static constexpr bool HAS_BTN_B = true;

static constexpr int8_t LCD_RST_PIN = 38, LCD_BL_PIN = 37;
static constexpr uint8_t READ_BAT_P0_PIN = 14, CHG_P0_PIN = 17;
static constexpr float BAT_DIVIDER = (1000.0f + 499.0f) / 499.0f;

Seeed_GFX display;
Touch_AXS5106L touch(-1, D7, Wire, W, H);   // RST shared with LCD, handled by display.begin

bool boardBegin() {
  nrf_gpio_cfg_input(CHG_P0_PIN, NRF_GPIO_PIN_PULLUP);
  analogReadResolution(12);
  if (!display.begin<Board_XIAO_1inch47_Touch_Display<LCD_RST_PIN, LCD_BL_PIN>,
                     Config_Seeed_1inch47_Touch_JD9853A>())
    return false;
  display.attachTouch(touch, display.panel().driver().bus());
  pinMode(LCD_BL_PIN, OUTPUT);
  return true;
}

void setBacklight(uint8_t level) { analogWrite(LCD_BL_PIN, level); }

// Main screen ("rail"): same coordinates as every other screen
void useNativeRail(bool) {}
static constexpr int RW = W, RH = H;
static constexpr int RAIL_D = 96, RAIL_TOP = 6, RAIL_STEP = 136, RAIL_LINE_DY = 22, RAIL_STATUS_Y = H - 46,
                     RAIL_CAT_SCALE = 2, RAIL_TEXT_X = 50, RAIL_FOOT_Y = H - 13, RAIL_PCT_H = 20;
#define RAIL_PCT_FONT (&FreeSansBold12pt7b)
#define RAIL_SMALL_FONT ((const GFXfont *)nullptr)   // 5x7 GLCD
#define RAIL_STATE_FONT ((const GFXfont *)nullptr)
#define RAIL_LARGE_ICONS 0

// Permission screen: drawn in logical coordinates (scale 1)
struct TextStyle { const GFXfont *font; uint8_t size; };   // font == nullptr: GLCD 5x7 at `size`
static constexpr float NS = 1.0f;
static const TextStyle P_LABEL{nullptr, 1}, P_SMALL{nullptr, 1}, P_TITLE{nullptr, 2}, P_BTN{&FreeSansBold9pt7b, 1},
                       P_DENY{nullptr, 2};
static constexpr int P_CMD_BIG = 2, P_CMD_SMALL = 1, P_CAT_SCALE = 3;
static constexpr const char *BTN_A_NAME = "USR1", *BTN_B_NAME = "USR2";
bool nativeNow() { return false; }

void boardPresent() {}   // draws go straight to the panel

// Battery voltage (V) and charging; returns false when no cell is present (charger node floats and reads noisy)
bool boardBattery(float &vbat, bool &charging) {
  NRF_P0->OUTCLR = (1UL << READ_BAT_P0_PIN);   // enable the divider (active-low sink)
  NRF_P0->DIRSET = (1UL << READ_BAT_P0_PIN);
  delay(20);
  int lo = 4095, hi = 0;
  long sum = 0;
  for (int i = 0; i < 16; i++) {
    int r = analogRead(PIN_VBAT);
    lo = min(lo, r);
    hi = max(hi, r);
    sum += r;
    delay(2);
  }
  NRF_P0->DIRCLR = (1UL << READ_BAT_P0_PIN);   // divider off
  vbat = (sum / 16.0f) * 3.6f / 4095.0f * BAT_DIVIDER;
  charging = (NRF_P0->IN & (1UL << CHG_P0_PIN)) == 0;
  return !(hi - lo > 60 || vbat < 3.0f);
}
