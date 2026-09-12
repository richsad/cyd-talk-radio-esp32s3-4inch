#pragma once

// ============================================================================
//  Standalone streaming: the board opens the station itself.
//
//  This is the untethered path. ESP32-audioI2S does the HTTP(S), the MP3/AAC
//  decode and the I2S output, so the Mac is not involved at all.
//
//  The library claims I2S_NUM_0 for itself, which is the same peripheral
//  audio.h drives. Only one of them may hold it, so entering stream mode
//  releases ours and leaving it takes the peripheral back. Everything else -
//  the ES8311 configuration, the PA enable - is untouched by the handover.
//
//  Two weak hooks in the library make this fit the existing architecture:
//
//    audio_process_i2s()   PCM after volume, which feeds the same spectrum
//                          analyser the tethered path uses. Without it the
//                          display would go dead in standalone mode.
//    audio_info_callback   ICY metadata, so the screen can show what is
//                          actually playing rather than a fixed description.
//
//  NAMING: macOS filesystems are case-insensitive, so a header here silently
//  shadows any core or library header with the same name in a different case.
//  This file was stream.h until Arduino's own Udp.h did #include <Stream.h> and
//  got this instead - which derailed Arduino.h itself, long before Serial was
//  defined, and produced dozens of errors pointing at the core. audio.h shadowed
//  the library's <Audio.h> the same way. Avoid Stream, Audio, Client, Server,
//  Print, WiFi, Update and friends as filenames in a sketch directory.
//
//  Copyright (c) 2026 Rich Sadowsky. MIT licensed - see LICENSE.
//  Written with Claude Code (Claude Opus 5).
// ============================================================================

#include <Arduino.h>
#include <Audio.h>

#include "playback.h"
#include "config.h"
#include "spectrum.h"
#include "stations.h"

static Audio *s_audio = nullptr;
static char s_streamTitle[96] = "";
static char s_streamStation[64] = "";
static volatile bool s_streamActive = false;
static volatile bool s_connectOk = false;
static volatile float s_streamRms = 0.0f;
static uint32_t s_streamRate = 0;

static const char *streamTitle()   { return s_streamTitle; }
static const char *streamStation() { return s_streamStation; }
static bool streamActive()         { return s_streamActive; }

// The library calls this from its own context with interleaved L/R after volume
// and EQ. Downmix to mono, normalise, and hand it to the same analyser the
// tethered path uses so the display behaves identically in both modes.
void audio_process_i2s(int32_t *outBuff, int16_t validSamples, bool *continueI2S) {
  *continueI2S = true;                      // still send it to the speaker
  if (!s_streamActive || validSamples <= 0) return;

  static float mono[SPEC_FFT_SIZE];
  static size_t fill = 0;

  const float kScale = 1.0f / 2147483648.0f;  // int32 full scale
  for (int i = 0; i < validSamples; i++) {
    // Interleaved stereo: average the pair into one mono sample.
    float l = (float)outBuff[i * 2] * kScale;
    float r = (float)outBuff[i * 2 + 1] * kScale;
    mono[fill++] = 0.5f * (l + r);
    if (fill >= SPEC_FFT_SIZE) {
      float sum = 0.0f;
      for (size_t k = 0; k < SPEC_FFT_SIZE; k++) sum += mono[k] * mono[k];
      s_streamRms = sqrtf(sum / SPEC_FFT_SIZE);
      spectrumProcess(mono, (float)SPEC_FFT_SIZE / (float)(s_streamRate ? s_streamRate
                                                                   : SAMPLE_RATE));
      fill = 0;
    }
  }
}

static void streamOnInfo(Audio::msg_t m) {
  switch (m.e) {
    case Audio::evt_streamtitle:
      snprintf(s_streamTitle, sizeof(s_streamTitle), "%s", m.msg);
      Serial.printf("[stream] title: %s\n", s_streamTitle);
      break;
    case Audio::evt_name:
      snprintf(s_streamStation, sizeof(s_streamStation), "%s", m.msg);
      break;
    case Audio::evt_bitrate:
      Serial.printf("[stream] bitrate %s\n", m.msg);
      break;
    default:
      break;
  }
}

// Take the I2S peripheral and start the station. Returns false if the connect
// fails, leaving the caller to fall back to the tethered path.
// Must be called from the audio task, not the UI thread: streamLoop() runs
// inside the library on core 0, and reconfiguring or deleting the object from
// core 1 while it is executing there is a use-after-free.
static bool streamBegin(int stationIndex, float volume) {
  if (stationIndex < 0 || stationIndex >= kStationCount) return false;

  s_streamTitle[0] = '\0';
  s_streamStation[0] = '\0';

  if (!s_audio) {
    // One-time setup. setPinout() registers an I2S event callback, which the
    // driver rejects with ESP_ERR_INVALID_STATE once the channel is enabled -
    // and the library turns that into abort(). Calling it on every tune
    // rebooted the board on the second station change.
    audioReleaseI2S();
    s_audio = new Audio(I2S_NUM_0);
    Audio::audio_info_callback = streamOnInfo;
    s_audio->setPinout(I2S_BCLK, I2S_LRCLK, I2S_DOUT, I2S_MCLK);
    s_audio->forceMono(true);
  } else {
    s_audio->stopSong();      // retune an existing, already-configured engine
  }
  // 0..21 in this library; our UI volume is 0..1.
  s_audio->setVolume((uint8_t)(constrain(volume, 0.0f, 1.0f) * 21.0f + 0.5f));

  s_streamActive = true;
  bool ok = s_audio->connecttohost(kStations[stationIndex].url);
  Serial.printf("[stream] connect %s -> %s\n", kStations[stationIndex].name,
                ok ? "ok" : "FAILED");
  s_connectOk = ok;
  if (!ok) s_streamActive = false;
  return ok;
}

static void streamSetVolume(float v) {
  if (s_audio) s_audio->setVolume((uint8_t)(constrain(v, 0.0f, 1.0f) * 21.0f + 0.5f));
}

// Stop playing, but do NOT destroy the engine.
//
// The library runs its own internal FreeRTOS task, and its destructor deletes
// the I2S channel and both mutexes *before* stopping that task:
//
//     stopSong(); i2s_channel_disable(); i2s_del_channel();
//     stopAudioTask(); vSemaphoreDelete(mutex_audioTask);
//
// so the decode task can still be running against freed objects. Deleting the
// Audio instance corrupted the heap ("CORRUPT HEAP: Bad head ... expected
// 0xabba1234") and panicked in multi_heap_free.
//
// Since the engine cannot be safely torn down, it also cannot hand the I2S
// peripheral back - which is why switching to HOST mode restarts the board
// instead. See applyPersistedMode() in the sketch.
static void streamStop() {
  if (!s_streamActive) return;
  s_streamActive = false;
  if (s_audio) s_audio->stopSong();
}

// Must be called often - this is where the library does its streaming work.
static void streamLoop() {
  if (!s_audio || !s_streamActive) return;
  s_audio->loop();

  // Band edges depend on the sample rate, which is only known once the stream
  // is decoding and differs per station (44.1 kHz is typical, ours is 32 kHz).
  uint32_t rate = s_audio->getSampleRate();
  if (rate && rate != s_streamRate) {
    s_streamRate = rate;
    spectrumSetRate(rate);
    Serial.printf("[stream] sample rate %lu Hz\n", (unsigned long)rate);
  }
}

static float streamRms() { return s_streamRms; }

// Whether the last connect attempt succeeded. A failed connect should be
// visible on screen rather than leaving the dial confidently claiming a station
// that is not playing.
static bool streamConnectOk() { return s_connectOk; }
