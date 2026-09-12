// ============================================================================
//  Touch calibration + codec probe.
//
//  Touch is confirmed working; what is not known is how the controller's native
//  portrait coordinates (0-319 x 0-479) map onto the landscape ROT 1 screen.
//  Guessing the two signs and the axis swap is four possibilities, so this
//  measures instead: tap four targets, and the raw value at each corner
//  determines the transform outright.
//
//  Also reads the ID registers of the device at 0x18. ES8311 answers 0x83/0x11
//  at 0xFD/0xFE. If that is what it is, the speaker path needs a register init
//  sequence over I2C and audio.h's "just enable the amp" assumption is wrong.
//
//  Copyright (c) 2026 Rich Sadowsky. MIT licensed - see LICENSE.
//  Written with Claude Code (Claude Opus 5).
// ============================================================================

#include <Arduino_GFX_Library.h>
#include <Wire.h>

#include "../TalkRadio/config.h"

#define BLACK 0x0000
#define WHITE 0xFFFF
#define AMBER 0xFD20
#define GREEN 0x07E0
#define RED   0xF800

static Arduino_DataBus *bus =
    new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI, LCD_MISO);
static Arduino_GFX *gfx =
    new Arduino_ST7796(bus, LCD_RST, 1, LCD_IPS, LCD_W, LCD_H);

static bool rd8(uint8_t addr, uint8_t reg, uint8_t *buf, size_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)addr, (int)n) != (int)n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

static bool touchRaw(int16_t *x, int16_t *y) {
  uint8_t b[7];
  if (!rd8(TOUCH_I2C_ADDR, 0x00, b, 7)) return false;
  if ((b[2] & 0x0F) == 0) return false;
  *x = ((b[3] & 0x0F) << 8) | b[4];
  *y = ((b[5] & 0x0F) << 8) | b[6];
  return true;
}

// Four screen-space targets, in tap order.
static const int16_t kInset = 45;
static int s_step = 0;
static int16_t s_rawX[4], s_rawY[4], s_tgtX[4], s_tgtY[4];

static void targetFor(int i, int16_t *tx, int16_t *ty) {
  const int16_t W = gfx->width(), H = gfx->height();
  switch (i) {
    case 0: *tx = kInset;     *ty = kInset;     break;  // top-left
    case 1: *tx = W - kInset; *ty = kInset;     break;  // top-right
    case 2: *tx = W - kInset; *ty = H - kInset; break;  // bottom-right
    default:*tx = kInset;     *ty = H - kInset; break;  // bottom-left
  }
}

static const char *kName[4] = {"TOP LEFT", "TOP RIGHT", "BOTTOM RIGHT", "BOTTOM LEFT"};

static void drawTarget() {
  gfx->fillScreen(BLACK);
  if (s_step >= 4) {
    gfx->setTextColor(GREEN);
    gfx->setTextSize(3);
    gfx->setCursor(20, 30);
    gfx->print("CALIBRATED");
    gfx->setTextColor(WHITE);
    gfx->setTextSize(2);
    for (int i = 0; i < 4; i++) {
      gfx->setCursor(20, 90 + i * 28);
      gfx->printf("%-13s %3d,%3d", kName[i], s_rawX[i], s_rawY[i]);
    }
    return;
  }
  int16_t tx, ty;
  targetFor(s_step, &tx, &ty);
  s_tgtX[s_step] = tx;
  s_tgtY[s_step] = ty;

  gfx->drawCircle(tx, ty, 22, AMBER);
  gfx->drawCircle(tx, ty, 14, AMBER);
  gfx->fillCircle(tx, ty, 5, RED);
  gfx->drawFastHLine(tx - 32, ty, 64, AMBER);
  gfx->drawFastVLine(tx, ty - 32, 64, AMBER);

  gfx->setTextColor(WHITE);
  gfx->setTextSize(3);
  gfx->setCursor(20, gfx->height() / 2 - 30);
  gfx->printf("TAP %d/4", s_step + 1);
  gfx->setTextSize(2);
  gfx->setCursor(20, gfx->height() / 2 + 6);
  gfx->print(kName[s_step]);
}

void setup() {
  Serial.begin(115200);
  ledcAttach(LCD_BL, BL_PWM_FREQ_HZ, BL_PWM_BITS);
  ledcWrite(LCD_BL, BL_LEVEL_DAY);
  gfx->begin(LCD_SPI_HZ);

  pinMode(TOUCH_INT, INPUT_PULLUP);
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);
  delay(20);
  digitalWrite(TOUCH_RST, HIGH);
  delay(200);
  Wire.begin(TOUCH_SDA, TOUCH_SCL, TOUCH_I2C_HZ);

  // --- what is at 0x18? --------------------------------------------------
  uint8_t id[3] = {0};
  bool ok = rd8(0x18, 0xFD, id, 3);
  Serial.printf("[codec] 0x18 read=%d 0xFD=%02X 0xFE=%02X 0xFF=%02X  %s\n", (int)ok,
                id[0], id[1], id[2],
                (id[0] == 0x83 && id[1] == 0x11) ? "ES8311 CONFIRMED" : "not es8311");
  for (uint8_t r = 0x00; r <= 0x06; r++) {
    uint8_t v = 0;
    rd8(0x18, r, &v, 1);
    Serial.printf("[codec] reg %02X = %02X\n", r, v);
  }

  drawTarget();
}

void loop() {
  static uint32_t latch = 0;
  int16_t rx, ry;
  if (touchRaw(&rx, &ry) && millis() - latch > 600) {
    latch = millis();
    if (s_step < 4) {
      s_rawX[s_step] = rx;
      s_rawY[s_step] = ry;
      Serial.printf("[cal] %d %-13s screen=%d,%d raw=%d,%d\n", s_step,
                    kName[s_step], s_tgtX[s_step], s_tgtY[s_step], rx, ry);
      s_step++;
      if (s_step == 4) {
        Serial.println("[cal] --- summary ---");
        for (int i = 0; i < 4; i++)
          Serial.printf("[cal] %-13s screen=%3d,%3d raw=%3d,%3d\n", kName[i],
                        s_tgtX[i], s_tgtY[i], s_rawX[i], s_rawY[i]);
      }
      drawTarget();
    }
  }
  delay(15);
}
