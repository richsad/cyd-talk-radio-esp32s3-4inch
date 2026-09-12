#pragma once

// ============================================================================
//  ES8311 audio codec.
//
//  The board does not have a dumb I2S amplifier. It has an ES8311 codec at I2C
//  0x18 (register 0xFD reads 0x83), which is why clocking I2S and raising the
//  amp-enable pin produces silence: the codec comes up powered down and routes
//  nothing until its registers are written.
//
//  Two consequences for config.h:
//    - AMP_USE_MCLK must be 1. The codec derives its internal clocks from MCLK
//      and will not run without it.
//    - AMP_EN_PIN is the external PA enable *after* the codec, not the codec
//      itself. Both are needed.
//
//  The register values below are the standard 256*fs coefficient set. Read back
//  is per-register: the ES8311 does not auto-increment across a multi-byte
//  read, which is why a 2-byte read of 0xFD/0xFE returns 0x83 then 0xFF.
//
//  Copyright (c) 2026 Rich Sadowsky. MIT licensed - see LICENSE.
//  Written with Claude Code (Claude Opus 5).
// ============================================================================

#include <Arduino.h>
#include <Wire.h>

#define ES8311_ADDR 0x18

static bool es8311Write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

// One register per transaction - see the note above about auto-increment.
static uint8_t es8311Read(uint8_t reg) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0xFF;
  if (Wire.requestFrom((int)ES8311_ADDR, 1) != 1) return 0xFF;
  return Wire.read();
}

static bool es8311Present() { return es8311Read(0xFD) == 0x83; }

// DAC volume, 0x00 = mute .. 0xFF = max. 0xBF is a sensible listening level.
static void es8311SetVolume(uint8_t v) { es8311Write(0x32, v); }

static bool es8311Init() {
  if (!es8311Present()) return false;

  es8311Write(0x00, 0x1F);  // reset
  delay(20);
  es8311Write(0x00, 0x00);
  es8311Write(0x00, 0x80);  // slave mode, power up

  // Clock manager. 0x01 turns on every internal clock; 0x02-0x08 are the
  // standard divider set for MCLK = 256 * fs, which is what the ESP32 I2S
  // driver emits by default.
  es8311Write(0x01, 0x3F);
  es8311Write(0x02, 0x00);
  es8311Write(0x03, 0x10);
  es8311Write(0x04, 0x10);
  es8311Write(0x05, 0x00);
  es8311Write(0x06, 0x03);
  es8311Write(0x07, 0x00);
  es8311Write(0x08, 0xFF);

  // Serial data format: I2S, 16-bit, both directions.
  // bits[1:0] = format (00 = I2S), bits[4:2] = word length (011 = 16 bit).
  es8311Write(0x09, 0x0C);  // SDP in  (to DAC)
  es8311Write(0x0A, 0x0C);  // SDP out (from ADC)

  // System power-up and routing.
  es8311Write(0x0B, 0x00);
  es8311Write(0x0C, 0x00);
  es8311Write(0x0D, 0x01);
  es8311Write(0x0E, 0x02);
  es8311Write(0x0F, 0x44);
  es8311Write(0x10, 0x03);
  es8311Write(0x11, 0x7F);
  es8311Write(0x12, 0x00);  // DAC powered up
  es8311Write(0x13, 0x10);
  es8311Write(0x14, 0x1A);  // ADC / mic path
  es8311Write(0x15, 0x00);
  es8311Write(0x16, 0x24);
  es8311Write(0x17, 0xBF);
  es8311Write(0x18, 0x00);

  es8311Write(0x31, 0x00);  // DAC unmuted
  es8311SetVolume(CODEC_VOLUME);
  es8311Write(0x37, 0x08);
  es8311Write(0x44, 0x00);  // no ADC->DAC loopback
  es8311Write(0x45, 0x00);

  return true;
}
