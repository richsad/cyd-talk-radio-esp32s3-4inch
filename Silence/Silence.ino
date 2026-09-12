// ============================================================================
//  Silence. Makes no sound, ever.
//
//  Exists because AmpTest plays a loud tone from boot, which makes it a hostile
//  thing to leave resident on the board. Flash this to park the hardware in a
//  quiet state: no I2S peripheral is created at all, and the codec DAC is
//  explicitly muted rather than merely turned down.
//
//  Copyright (c) 2026 Rich Sadowsky. MIT licensed - see LICENSE.
//  Written with Claude Code (Claude Opus 5).
// ============================================================================

#include <Arduino_GFX_Library.h>
#include <Wire.h>

#include "../TalkRadio/config.h"
#include "../TalkRadio/es8311.h"

static Arduino_DataBus *bus =
    new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI, LCD_MISO);
static Arduino_GFX *gfx =
    new Arduino_ST7796(bus, LCD_RST, LCD_ROTATION, LCD_IPS, LCD_W, LCD_H);

void setup() {
  Serial.begin(115200);

  // Hold the PA enable inactive before anything else can drive the bus.
  pinMode(AMP_EN_PIN, OUTPUT);
  digitalWrite(AMP_EN_PIN, !AMP_EN_ACTIVE);

  Wire.begin(TOUCH_SDA, TOUCH_SCL, TOUCH_I2C_HZ);
  if (es8311Present()) {
    es8311SetVolume(0x00);   // DAC volume to zero
    es8311Write(0x31, 0x60); // and hard-mute the DAC as well
  }

  ledcAttach(LCD_BL, BL_PWM_FREQ_HZ, BL_PWM_BITS);
  ledcWrite(LCD_BL, BL_LEVEL_DAY);
  gfx->begin(LCD_SPI_HZ);
  gfx->fillScreen(0x0000);
  gfx->setTextColor(0xFD20);
  gfx->setTextSize(4);
  gfx->setCursor(20, 100);
  gfx->print("SILENT");
  gfx->setTextSize(2);
  gfx->setTextColor(0xFFFF);
  gfx->setCursor(20, 160);
  gfx->print("no audio, safe to leave on");
  Serial.println("[silence] muted");
}

void loop() { delay(1000); }
