#pragma once

// ============================================================================
//  Real-time spectrum analysis.
//
//  The FFT, band mapping and tilt are ported from CydSpectrum
//  (cyd-audio-spectrum-esp32s3-7inch), which already proved them on an
//  ESP32-S3. Two things differ here:
//
//    - SPEC_FFT_SIZE == HOP_SIZE, so the analyser runs on exactly the block that was
//      just written to the speaker. CydSpectrum overlapped by 50% because it
//      was analysing a live microphone and wanted the extra time resolution;
//      playback hands us a natural block boundary for free.
//    - The band range is speech-shaped (90 Hz - 7 kHz), not full-range.
//
//  Analysis runs on core 0 inside the audio task, immediately after playback,
//  and publishes results under a spinlock for the render loop on core 1.
//
//  Copyright (c) 2026 Rich Sadowsky. MIT licensed - see LICENSE.
//  Written with Claude Code (Claude Opus 5).
// ============================================================================

#include <Arduino.h>
#include <math.h>

#include "config.h"

static float s_window[SPEC_FFT_SIZE];
static float s_twRe[SPEC_FFT_SIZE / 2];
static float s_twIm[SPEC_FFT_SIZE / 2];
static float s_re[SPEC_FFT_SIZE];
static float s_im[SPEC_FFT_SIZE];

static uint16_t s_binLo[SPEC_BANDS];
static uint16_t s_binHi[SPEC_BANDS];
static float    s_tiltDb[SPEC_BANDS];

// Published to the renderer. 0..1 per band, already scaled to DISPLAY_RANGE_DB.
static float s_level[SPEC_BANDS];
static float s_peak[SPEC_BANDS];
static float s_peakAge[SPEC_BANDS];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static const float kMagScale = 4.0f / (float)SPEC_FFT_SIZE;  // Hann coherent gain

// Band edges depend on the sample rate. The tethered path is always 32 kHz, but
// a directly-streamed station is usually 44.1 kHz, and using the wrong rate
// shifts every bar's centre frequency.
static void spectrumSetRate(uint32_t rate) {
  const float binHz = (float)rate / SPEC_FFT_SIZE;
  const float ratio = powf(BAND_F_HIGH / BAND_F_LOW, 1.0f / (SPEC_BANDS - 1));
  for (int b = 0; b < SPEC_BANDS; b++) {
    float centre = BAND_F_LOW * powf(ratio, b);
    int kLo = (int)floorf((centre / sqrtf(ratio)) / binHz + 0.5f);
    int kHi = (int)floorf((centre * sqrtf(ratio)) / binHz + 0.5f);
    if (kLo < 1) kLo = 1;
    if (kHi < kLo) kHi = kLo;
    if (kHi > SPEC_FFT_SIZE / 2 - 1) kHi = SPEC_FFT_SIZE / 2 - 1;
    if (kLo > kHi) kLo = kHi;
    s_binLo[b] = kLo;
    s_binHi[b] = kHi;
    s_tiltDb[b] = TILT_DB_PER_OCTAVE * log2f(centre / 1000.0f);
  }
}

static void spectrumInit() {
  for (int i = 0; i < SPEC_FFT_SIZE; i++)
    s_window[i] = 0.5f * (1.0f - cosf(2.0f * (float)PI * i / (SPEC_FFT_SIZE - 1)));
  for (int i = 0; i < SPEC_FFT_SIZE / 2; i++) {
    s_twRe[i] = cosf(-2.0f * (float)PI * i / SPEC_FFT_SIZE);
    s_twIm[i] = sinf(-2.0f * (float)PI * i / SPEC_FFT_SIZE);
  }

  spectrumSetRate(SAMPLE_RATE);
}

// In-place iterative radix-2 Cooley-Tukey.
static void fftRun(float *re, float *im) {
  for (unsigned i = 1, j = 0; i < SPEC_FFT_SIZE; i++) {
    unsigned bit = SPEC_FFT_SIZE >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      float t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (unsigned len = 2; len <= SPEC_FFT_SIZE; len <<= 1) {
    unsigned half = len >> 1, step = SPEC_FFT_SIZE / len;
    for (unsigned i = 0; i < SPEC_FFT_SIZE; i += len) {
      for (unsigned k = 0; k < half; k++) {
        float wr = s_twRe[k * step], wi = s_twIm[k * step];
        unsigned a = i + k, b = a + half;
        float xr = re[b] * wr - im[b] * wi;
        float xi = re[b] * wi + im[b] * wr;
        re[b] = re[a] - xr;
        im[b] = im[a] - xi;
        re[a] += xr;
        im[a] += xi;
      }
    }
  }
}

// Called on core 0 with the block that was just played.
static void spectrumProcess(const float *samples, float dt) {
  for (int i = 0; i < SPEC_FFT_SIZE; i++) {
    s_re[i] = samples[i] * s_window[i];
    s_im[i] = 0.0f;
  }
  fftRun(s_re, s_im);

  static float ref = AGC_MIN_REF_DB;
  static float lvl[SPEC_BANDS], pk[SPEC_BANDS], age[SPEC_BANDS];

  float raw[SPEC_BANDS], loudest = -120.0f;
  for (int b = 0; b < SPEC_BANDS; b++) {
    float acc = 0.0f;
    for (int k = s_binLo[b]; k <= s_binHi[b]; k++) {
      float re = s_re[k], im = s_im[k];
      acc += re * re + im * im;
    }
    float mag = sqrtf(acc) * kMagScale;
    float db = 20.0f * log10f(mag + 1e-9f) + s_tiltDb[b];
    raw[b] = db;
    if (db > loudest) loudest = db;
  }

  // AGC: fast to follow a rise, slow to release, so speech does not pump.
  float target = constrain(loudest, AGC_MIN_REF_DB, AGC_MAX_REF_DB);
  ref += (target > ref ? AGC_ATTACK : AGC_RELEASE) * (target - ref);

  const float aAtt = 1.0f - expf(-dt / ATTACK_TAU);
  const float aDec = 1.0f - expf(-dt / DECAY_TAU);

  for (int b = 0; b < SPEC_BANDS; b++) {
    float norm = constrain((raw[b] - (ref - DISPLAY_RANGE_DB)) / DISPLAY_RANGE_DB,
                           0.0f, 1.0f);
    lvl[b] += (norm > lvl[b] ? aAtt : aDec) * (norm - lvl[b]);

    if (lvl[b] >= pk[b]) {
      pk[b] = lvl[b];
      age[b] = 0.0f;
    } else {
      age[b] += dt;
      if (age[b] > PEAK_HOLD_S) pk[b] -= PEAK_FALL_RATE * dt;
      if (pk[b] < lvl[b]) pk[b] = lvl[b];
      if (pk[b] < 0.0f) pk[b] = 0.0f;
    }
  }

  portENTER_CRITICAL(&s_mux);
  memcpy(s_level, lvl, sizeof(lvl));
  memcpy(s_peak, pk, sizeof(pk));
  portEXIT_CRITICAL(&s_mux);
}

// Snapshot for the renderer on core 1.
static void spectrumSnapshot(float *level, float *peak) {
  portENTER_CRITICAL(&s_mux);
  memcpy(level, s_level, sizeof(s_level));
  memcpy(peak, s_peak, sizeof(s_peak));
  portEXIT_CRITICAL(&s_mux);
}
