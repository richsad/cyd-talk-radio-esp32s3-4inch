# ESP32-S3 Talk Radio — design notes

Working notes. v1 is built and running; see [Status](#status).

## The hardware, as measured

Hosyond ESP32-S3 Touchscreen Module, 4.0", capacitive touch, speaker header.

**The vendor listing is wrong in three places.** Everything below was measured on
the board, and each correction is recorded in `TalkRadio/config.h` alongside the
evidence that overturned it.

| Part | Listing claims | Actually |
|---|---|---|
| Panel | 240×320 | **320×480** (ROT 1 → 480×320 landscape) |
| Driver | ILI9341 | **ST7796** |
| Audio | I2S amp, `IO1` output enable | **ES8311 codec** @ 0x18, PA enable `IO1` **active LOW** |

| Function | Pins |
|---|---|
| MCU | ESP32-S3, 8 MB octal PSRAM, 16 MB flash |
| LCD (SPI) | CS 10, DC 46, CLK 12, MOSI 11, MISO 13, backlight 45 (PWM) |
| Touch (FT6336, I2C 0x38) | SDA 16, SCL 15, RST 18, INT 17 |
| I2S | BCLK 5, LRCLK 7, DOUT 8, DIN 6, **MCLK 4 (required)** |
| PA enable | **GPIO1, active LOW** |
| SD | CLK 38, CMD 40, D0–D3 39/41/48/47 |

### How the panel error was caught

Driving a 320×480 panel as 240×320 fills exactly ¾ of the width and ⅔ of the
height. The "two thirds" observation was the whole diagnosis — 320/480 is
exactly ⅔. ILI9341 and ST7796 share enough command set that the wrong init still
draws; it just draws into the wrong window.

### How the amp polarity was caught

A GPIO sweep played a tone with one candidate pin HIGH and every other candidate
LOW. Sound came out on *several* steps. No active-HIGH enable on a single pin can
produce that; an active-LOW enable on GPIO1 produces it exactly, silent only
during GPIO1's own step.

## Architecture

The board **never opens a stream**. Most stations are HTTPS now, and TLS plus an
AAC decode on an S3 also driving a 480×320 panel is a bad trade. `ffmpeg` on the
Mac decodes and resamples; the board receives mono 16-bit PCM at 32 kHz over UDP
and hands it to the codec.

UDP is deliberate: a dropped packet costs one glitch, where TCP would stall
playback to retransmit. Packets that will not fit whole in the jitter buffer are
dropped rather than written partially, since a half-written packet misaligns
every sample after it.

**Tuning needs no configuration.** The board records whichever address is feeding
it audio and sends `TUNE <index>` back on port 3334. The process sending audio is
by definition the one that can retune it.

The trade-off is that v1 is **tethered** — it needs the Mac running.

## Hard-won details

- **The ESP32-S3 has no DAC.** All audio is I2S to the codec; no analog fallback.
- **The ES8311 needs MCLK** and a register init over I2C. Clocking I2S and
  raising an enable pin produces silence, with every sample count healthy.
- **I2S must be TX-only.** Creating an RX channel alongside TX — with a config
  naming both `dout` and `din` — disturbs shared clock and GPIO state and mutes
  the output. The mic is deferred to call-in for this reason.
- **`ES8311 0x32` is logarithmic**: `−95.5 + 0.5 × value` dB. `0xBF` is unity.
  Treating it as a linear 0–255 scale put the output ~77 dB down: inaudible.
- **Never poll touch in a tight loop.** Reading the FT6336 every loop iteration
  is thousands of I2C transactions a second; it corrupts reads (`TD_STATUS` of
  6 touches) and eventually wedges the controller. 50 Hz is plenty.
- **Never full-repaint on a timer.** On this SPI panel it reads as flicker. Every
  element draws incrementally.
- **GPIO19/20 are USB D−/D+.** Driving them as outputs kills the USB connection
  and the board can only be recovered through ROM download mode.
- **`setTxPower()` before `WiFi.begin()` is silently ignored.** Applying it
  correctly for the first time dropped the board to 13 dBm and it could no
  longer associate. If you lower it, verify association afterwards.
- **launchd runs agents at background QoS.** Timer coalescing turned a 16 ms
  sleep into ~37 ms, halving the packet rate with no error anywhere. Needs
  `ProcessType: Interactive`.
- **launchd's `PATH` excludes Homebrew**, so ffmpeg appears missing.
- **A pacing loop that resyncs on overrun hides the overrun.** It silently caps
  throughput at whatever the loop achieves. Catch up instead, and log it when
  you cannot.
- **macOS header shadowing.** A sketch header shadows any core/library header
  differing only in case. `audio.h` shadowed `<Audio.h>`; `stream.h` shadowed
  `<Stream.h>` and broke `Arduino.h` itself.
- **Sketch macros have no namespace.** `FFT_SIZE` and `NUM_BANDS` collided with
  struct members inside ESP32-audioI2S, rewriting its declarations into garbage.
- **The ESP32-audioI2S `Audio` object cannot be destroyed safely.** It runs its
  own FreeRTOS task, and its destructor deletes the I2S channel and both mutexes
  *before* stopping that task, so the decode task runs against freed objects.
  `delete` corrupted the heap and panicked in `multi_heap_free`. It is therefore
  created once and never destroyed - which is why it can never hand I2S back,
  and why a DIRECT/HOST switch restarts the board.
- **`RTC_DATA_ATTR` does not survive `ESP.restart()`.** Those variables live in
  an initialised section that a normal boot reloads. `RTC_NOINIT_ATTR` survives,
  but is never initialised - so it needs a magic value to detect garbage after a
  cold power-on.

## Status

**Built and working:** ST7796 panel, calibrated FT6336 touch, ES8311 audio out,
Wi-Fi, UDP PCM playback, 30-band FFT spectrum, tuning dial with 5 stations,
tap-to-set volume, and `tools/radio_server.py` with station switching.

Also working: mute, Wi-Fi reconnect with scan diagnostics, zero-config address
discovery in both directions, host-side idle/resume, and a LaunchAgent.

**Verified stations:** all five. C-SPAN was replaced by LBC after failing TLS
certificate verification; the rest each return exactly 3 s of audio in 3 s.

## Standalone mode

DIRECT mode uses ESP32-audioI2S for HTTPS, decode and I2S output. The library
claims `I2S_NUM_0`, the same peripheral `playback.h` drives, so only one may
hold it: entering DIRECT releases ours, leaving it takes the peripheral back.
The codec configuration and PA enable are untouched by the handover.

Two weak hooks keep it inside the existing architecture rather than beside it:

- `audio_process_i2s()` — PCM after volume, feeding the same spectrum analyser
  the tethered path uses. Without it the display would go dead in DIRECT.
- `audio_info_callback` — ICY metadata, so the subtitle shows what is actually
  playing instead of a fixed description.

Band edges depend on sample rate, which differs per station (44.1 kHz is common,
ours is 32 kHz), so `spectrumSetRate()` retunes them when the stream reports its
rate.

Costs: ~2 MB of flash, needing `PartitionScheme=huge_app`.

## Planned

1. **AI station** — Claude writes a call-in show, TTS voices it, streamed like
   any other station. It is a server feature, not firmware complexity.
2. **Call-in** — the on-board mic. No longer blocked: full-duplex I2S works on
   this board, proven in
   [cyd-sonar-esp32s3-4inch](../cyd-sonar-esp32s3-4inch), which runs mic and
   speaker together at 48 kHz. Both channel handles have to come from a single
   `i2s_new_channel()` call — two separate calls are what silenced the speaker
   during bring-up here and got the mic shelved. Re-add RX with the tone test
   as a regression check.
3. Sleep timer, alarm, night dimming via the backlight PWM, ICY stream metadata.
