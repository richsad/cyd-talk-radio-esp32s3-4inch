// ============================================================================
//  Power-amplifier enable pin sweep.
//
//  Everything upstream is confirmed: samples reach I2S, and every ES8311
//  register reads back exactly as written (00=80 01=3F 09/0A=0C 12=00 31=00
//  32=BF). So the codec is configured and the silence is after it - the PA
//  stage that drives the speaker header.
//
//  IO1 is what the vendor doc claims, but that same doc said this was a
//  240x320 ILI9341 and it is a 320x480 ST7796, so it is not evidence. This
//  plays a loud tone and walks one candidate GPIO high at a time, announcing
//  each on screen, so the pin can be found by ear.
//
//  Strapping and in-use pins are excluded: 0/3/45/46 strap, 5/6/7/8/4 are I2S,
//  10/11/12/13/46 the panel, 15/16/17/18 touch, 38-41/47/48 the SD slot - and
//  19/20, which are the S3's native USB D-/D+. Driving those as outputs drops
//  the USB connection mid-sweep, which is exactly what happened the first time.
//
//  Step 0 is the control: every candidate left low. If the tone is audible
//  there, no enable pin is involved at all and the original silence was simply
//  level - TONE_AMPLITUDE 0.25 at 60% volume is a quarter of what this plays.
//
//  Copyright (c) 2026 Rich Sadowsky. MIT licensed - see LICENSE.
//  Written with Claude Code (Claude Opus 5).
// ============================================================================

#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <driver/i2s_std.h>

#include "../TalkRadio/config.h"
#include "../TalkRadio/es8311.h"
#include "../TalkRadio/touch.h"

#define BLACK 0x0000
#define WHITE 0xFFFF
#define AMBER 0xFD20
#define GREEN 0x07E0

static Arduino_DataBus *bus =
    new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI, LCD_MISO);
static Arduino_GFX *gfx =
    new Arduino_ST7796(bus, LCD_RST, LCD_ROTATION, LCD_IPS, LCD_W, LCD_H);

static const int kCandidates[] = {1, 2, 9, 14, 21};
static const int kN = sizeof(kCandidates) / sizeof(kCandidates[0]);
static int s_idx = -1;  // -1 = all-low control step
static bool s_frozen = false;

// Starts MUTED. This sketch previously played a full-amplitude tone from boot,
// which - combined with driving the USB pins and losing the serial connection -
// left the board loud and unreachable. A diagnostic that can only be stopped by
// pulling power is a bad diagnostic.
static bool s_sounding = false;
static bool s_activeHigh = true;

static i2s_chan_handle_t s_tx = nullptr;
static float s_phase = 0.0f;

static void i2sInit() {
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.dma_desc_num = 6;
  cc.dma_frame_num = 256;
  cc.auto_clear = true;
  i2s_new_channel(&cc, &s_tx, nullptr);

  i2s_std_config_t cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_MONO),
      .gpio_cfg = {.mclk = (gpio_num_t)I2S_MCLK,
                   .bclk = (gpio_num_t)I2S_BCLK,
                   .ws = (gpio_num_t)I2S_LRCLK,
                   .dout = (gpio_num_t)I2S_DOUT,
                   .din = I2S_GPIO_UNUSED,
                   .invert_flags = {false, false, false}},
  };
  i2s_channel_init_std_mode(s_tx, &cfg);
  i2s_channel_enable(s_tx);
}

// Loud - this test is about audibility, not fidelity.
static void pushTone() {
  static int16_t pcm[512];
  if (!s_sounding) {
    memset(pcm, 0, sizeof(pcm));
    size_t w = 0;
    i2s_channel_write(s_tx, pcm, sizeof(pcm), &w, 100);
    return;
  }
  const float step = 2.0f * (float)PI * 440.0f / (float)SAMPLE_RATE;
  for (int i = 0; i < 512; i++) {
    pcm[i] = (int16_t)(sinf(s_phase) * 20000.0f);
    s_phase += step;
    if (s_phase > 2.0f * (float)PI) s_phase -= 2.0f * (float)PI;
  }
  size_t w = 0;
  i2s_channel_write(s_tx, pcm, sizeof(pcm), &w, 100);
}

static void applyPins() {
  for (int i = 0; i < kN; i++) {
    pinMode(kCandidates[i], OUTPUT);
    bool on = (s_idx >= 0 && i == s_idx);
    digitalWrite(kCandidates[i], s_activeHigh ? (on ? HIGH : LOW) : (on ? LOW : HIGH));
  }
  gfx->fillScreen(BLACK);
  gfx->setTextColor(AMBER);
  gfx->setTextSize(4);
  gfx->setCursor(20, 40);
  if (s_idx < 0) gfx->print("ALL LOW");
  else gfx->printf("GPIO %d", kCandidates[s_idx]);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(3);
  gfx->setCursor(20, 110);
  gfx->print(s_activeHigh ? "driven HIGH" : "driven LOW");
  gfx->setTextSize(2);
  gfx->setTextColor(GREEN);
  gfx->setCursor(20, 170);
  gfx->print("440 Hz tone playing");
  gfx->setCursor(20, 200);
  gfx->printf("step %d of %d", s_idx + 1 + (s_activeHigh ? 0 : kN), kN * 2);
  gfx->setCursor(20, 235);
  gfx->setTextColor(WHITE);
  if (!s_sounding) gfx->print("TAP TO START TONE");
  else gfx->print(s_frozen ? "HELD - tap to resume" : "TAP WHEN YOU HEAR IT");
  if (s_idx < 0) Serial.printf("[amp] ALL LOW (control) frozen=%d\n", (int)s_frozen);
  else Serial.printf("[amp] GPIO %d %s frozen=%d\n", kCandidates[s_idx],
                     s_activeHigh ? "HIGH" : "LOW", (int)s_frozen);
}

void setup() {
  Serial.begin(115200);
  ledcAttach(LCD_BL, BL_PWM_FREQ_HZ, BL_PWM_BITS);
  ledcWrite(LCD_BL, BL_LEVEL_DAY);
  gfx->begin(LCD_SPI_HZ);
  Wire.begin(TOUCH_SDA, TOUCH_SCL, TOUCH_I2C_HZ);

  touchInit();
  i2sInit();
  delay(50);
  bool ok = es8311Init();
  es8311SetVolume(0xFF);
  Serial.printf("[amp] es8311 init=%d id=%02X\n", (int)ok, es8311Read(0xFD));
  applyPins();
}

void loop() {
  static uint32_t last = 0, latch = 0;
  pushTone();

  // Tap freezes the sweep on the current step so the pin can be read off at
  // leisure - and reported over serial - instead of caught in a 4s window.
  TouchPoint t = touchRead();
  if (t.pressed && millis() - latch > 500) {
    latch = millis();
    if (!s_sounding) s_sounding = true;   // first tap starts the tone
    else s_frozen = !s_frozen;
    applyPins();
  }

  if (!s_frozen && millis() - last > 4000) {
    last = millis();
    s_idx++;
    if (s_idx >= kN) {
      s_idx = -1;
      s_activeHigh = !s_activeHigh;
    }
    applyPins();
  }
}
