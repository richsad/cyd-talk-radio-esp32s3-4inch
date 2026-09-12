#pragma once

// ============================================================================
//  Board / project configuration for the Hosyond ESP32-S3 4.0" touchscreen
//  module (240x320 IPS, capacitive touch, on-board I2S amp + speaker header).
//
//  Copyright (c) 2026 Rich Sadowsky. MIT licensed - see LICENSE.
//  Written with Claude Code (Claude Opus 5).
// ============================================================================

// ---------------------------------------------------------------------------
//  !! UNVERIFIED PINOUT !!
//
//  Every pin below comes from LCD wiki's "2.8inch ESP32-S3 Display", which is
//  the board Hosyond rebrands. The whole Hosyond ESP32-S3 line (2.8/3.5/4.0")
//  is 240x320, so the 4.0" is almost certainly the same design with a larger
//  panel - but nothing here has been confirmed against the actual hardware.
//
//  The bring-up sketch exists to prove these. Run it and fix what it reports;
//  everything is defined here so a wrong guess is a one-line edit.
//
//  Two known unknowns:
//    - Panel driver may be ST7796 rather than ILI9341V on the 4.0". If the
//      screen stays black or the image is mirrored/offset, try the other.
//    - MCLK on IO4 alongside a mic input hints this may be a codec (ES8311?)
//      rather than a plain amp. If the test tone is silent with AMP_USE_MCLK 0,
//      try 1 - and if it is still silent, the part needs I2C init, not just I2S.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
//  LCD - ST7796 over 4-line SPI, 320x480 IPS
//
//  NOT 240x320, despite what the Hosyond listing title says. Driving it as
//  240x320 fills exactly 3/4 of the width and 2/3 of the height, which is how
//  this was caught. Their 3.5" board carries the same wrong 240x320 in its
//  title, so the whole ESP32-S3 line appears to have the 2.8" spec pasted in.
//
//  ILI9341 and ST7796 overlap enough in their command sets that the wrong init
//  still draws - it just draws into the wrong window. A partial, mirrored image
//  is the signature of the wrong controller here, not of wiring trouble.
// ---------------------------------------------------------------------------
#define LCD_W 320
#define LCD_H 480

#define LCD_CS    10
#define LCD_DC    46
#define LCD_SCK   12
#define LCD_MOSI  11
#define LCD_MISO  13
#define LCD_RST   -1  // not broken out on this family; -1 = tie to EN
#define LCD_BL    45  // backlight, PWM-dimmable

// Confirmed on hardware: ROT 1 is upright landscape. LCD_W/LCD_H above are the
// panel's NATIVE portrait dimensions, which the driver needs; at this rotation
// the drawable surface is 480x320. Use SCREEN_W/SCREEN_H for layout.
#define LCD_ROTATION 1
#define SCREEN_W 480
#define SCREEN_H 320

// IPS panels almost always need colour inversion on. PanelTest confirms it.
#define LCD_IPS true

// SPI clock. 40 MHz is the usual safe ceiling for ILI9341 on flying leads;
// this is a rigid PCB so 80 MHz generally works. Drop it if you see tearing
// or corruption.
#define LCD_SPI_HZ 40000000

// Backlight PWM. Night mode dims rather than blanking - a bedside radio that
// goes fully dark is harder to find in the dark than one at 5%.
// ESP32 core 3.x addresses ledc by pin, not by channel.
#define BL_PWM_FREQ_HZ 5000
#define BL_PWM_BITS       8
#define BL_LEVEL_DAY    255
#define BL_LEVEL_NIGHT   12

// ---------------------------------------------------------------------------
//  Touch - FT6336G capacitive, I2C
//
//  Note this is *not* the GT911 most CYD boards use, so GT911 drivers and
//  most copy-pasted CYD touch code will not work here.
// ---------------------------------------------------------------------------
#define TOUCH_SDA 16
#define TOUCH_SCL 15
#define TOUCH_RST 18
#define TOUCH_INT 17

#define TOUCH_I2C_HZ   400000
#define TOUCH_I2C_ADDR 0x38  // confirmed: chip 0x64, vendor 0x11 (FocalTech)

// Measured with the four-corner calibration, not derived from the datasheet.
// The controller reports native portrait coordinates; at ROT 1 the long axis
// maps straight through and the short axis is inverted.
//     screen_x = raw_y
//     screen_y = (SCREEN_H - 1) - raw_x
#define TOUCH_SWAP_XY   1
#define TOUCH_INVERT_Y  1

// ---------------------------------------------------------------------------
//  Audio
//
//  Speaker output goes to the on-board amplifier and out the JST speaker
//  header. The ESP32-S3 has NO DAC (unlike the classic ESP32), so this path is
//  I2S-only - there is no PWM/analog fallback to fall back on.
//
//  Mic and amp share BCLK and LRCLK on one I2S bus, so capture and playback
//  must run as a single full-duplex channel pair rather than two independent
//  channels. See audio.h.
// ---------------------------------------------------------------------------
#define I2S_BCLK   5
#define I2S_LRCLK  7
#define I2S_DOUT   8  // -> amplifier -> speaker header
#define I2S_DIN    6  // <- on-board microphone
#define I2S_MCLK   4

// Amplifier enable: GPIO1, ACTIVE LOW.
//
// Deduced from the sweep rather than the datasheet. AmpTest drove exactly one
// candidate HIGH and every other candidate LOW, so GPIO1 sat LOW in four of the
// five steps - and sound played in "several" of them, including the GPIO2 step.
// An active-HIGH enable on any single pin cannot produce that pattern; an
// active-LOW enable on GPIO1 produces it exactly, silent only during GPIO1's
// own step.
//
// The vendor doc's "IO1: output enable" had the right pin. The polarity was my
// assumption, and leaving IO1 floating is why the bring-up sketch stayed mute
// while every sample count and codec register read perfectly healthy.
#define AMP_EN_PIN     1
#define AMP_EN_ACTIVE  LOW

// Required. The board carries an ES8311 codec (I2C 0x18, reg 0xFD = 0x83), not
// a plain I2S amp, and the codec derives its internal clocks from MCLK.
#define AMP_USE_MCLK 1

// 32 kHz mono matches CydSpectrum's pipeline and stream_to_cyd.py's default,
// so the existing sender works unchanged. Speech needs nothing more.
#define SAMPLE_RATE 32000
#define HOP_SIZE    1024

// Output volume, 0.0 - 1.0, applied in software before the I2S write. This is
// a LINEAR gain on the samples, unlike CODEC_VOLUME below.
#define VOLUME_DEFAULT 0.60f

// ES8311 DAC volume. This register is LOGARITHMIC, not a 0-255 linear scale:
//
//     gain_dB = -95.5 + 0.5 * value
//
// so 0xBF (191) is unity, 0xFF (255) is +32 dB, and 0x90 (144) is -23.5 dB.
// Treating it as "a bit quieter than 0xBF" cost an evening: 0x90 together with
// a 0.20 software volume put the output roughly 77 dB below the level that was
// uncomfortably loud, which is simply inaudible.
//
// Unity is the right reference. Adjust loudness with VOLUME_DEFAULT and the
// on-screen VOL buttons, which are linear and predictable, not with this.
#define CODEC_VOLUME 0xBF

// ---------------------------------------------------------------------------
//  Network PCM source
//
//  Wire-compatible with cyd-audio-spectrum-esp32s3-7inch/tools/stream_to_cyd.py:
//  mono 16-bit little-endian PCM at SAMPLE_RATE, UDP, packets of 512 samples.
//  That sender already decodes internet radio via ffmpeg, which is why v1 does
//  no decoding on the board at all.
//
//      tools/stream_to_cyd.py <board-ip> --input https://stream.example/radio.mp3
// ---------------------------------------------------------------------------
#define ENABLE_NET 1

#include "secrets.h"

#define NET_UDP_PORT 3333

// Jitter buffer. Bigger = more resilient to Wi-Fi hiccups, at the cost of
// latency. For radio, latency is irrelevant and dropouts are not, so this is
// deliberately generous compared to the spectrum analyser's 16 KB.
#define NET_BUFFER_BYTES    65536  // ~1.0 s at 32 kHz mono 16-bit
#define NET_PREROLL_BYTES   16384  // wait for this much before starting playback
#define NET_READ_TIMEOUT_MS   250

// Full power. This was WIFI_POWER_13dBm, inherited from CydSpectrum where it
// guarded a 7" backlight against browning out a marginal USB supply - a concern
// that does not apply to this smaller board, and we have seen no brownouts.
//
// It also never actually took effect there: setTxPower() before WiFi.begin() is
// silently ignored, so the radio ran at full power regardless. Applying it
// correctly for the first time dropped the board to 13 dBm, which was too weak
// to associate - reason 2, WIFI_REASON_AUTH_EXPIRE, on every attempt. If you
// ever lower this, verify association afterwards rather than trusting it.
#define WIFI_TX_POWER WIFI_POWER_19_5dBm

// ---------------------------------------------------------------------------
//  Test tone (proves the speaker path with no network at all)
// ---------------------------------------------------------------------------
#define TONE_HZ        440.0f

// Audible but well short of the sweep test's 0.61, which at codec +32 dB was
// loud enough to be alarming in a quiet room.
#define TONE_AMPLITUDE   0.50f

// ---------------------------------------------------------------------------
//  Spectrum display
//
//  SPEC_FFT_SIZE equals HOP_SIZE so the analyser runs on exactly the block that was
//  just played - no extra buffering, no second copy, and 31 spectra a second at
//  32 kHz, which is well past what the eye resolves.
//
//  The band range is chosen for speech rather than music. Talk radio has
//  essentially nothing above 8 kHz and a 40 Hz bottom end just shows rumble, so
//  spending bars there wastes the display.
// ---------------------------------------------------------------------------
// Prefixed SPEC_ deliberately. ESP32-audioI2S declares its own FFT_SIZE and
// NUM_BANDS as struct members, and an unprefixed #define here rewrites those
// declarations into nonsense - "const uint16_t 1024 = 512;" - with the error
// reported against this file rather than the library. Macros in a sketch have
// no namespace, so anything generic enough to collide needs a prefix.
#define SPEC_FFT_SIZE    HOP_SIZE
#define SPEC_BANDS       30

#define BAND_F_LOW    90.0f
#define BAND_F_HIGH 7000.0f

// Speech rolls off steeply with frequency; without a tilt the right-hand bars
// barely move and the display looks broken rather than quiet.
#define TILT_DB_PER_OCTAVE 3.5f
#define DISPLAY_RANGE_DB  46.0f

// The top of the scale tracks the loudest band, so a quiet passage still shows
// detail instead of flatlining.
#define AGC_ATTACK      0.25f
#define AGC_RELEASE     0.010f
#define AGC_MIN_REF_DB -58.0f
#define AGC_MAX_REF_DB   0.0f

// Bar ballistics: fast attack so consonants snap, slow decay so the display
// reads as motion rather than flicker.
#define ATTACK_TAU     0.015f
#define DECAY_TAU      0.240f
#define PEAK_HOLD_S    0.80f
#define PEAK_FALL_RATE 0.55f
