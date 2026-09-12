#pragma once

// ============================================================================
//  Audio path: I2S out to the on-board amplifier and speaker header, plus a UDP
//  PCM receiver.
//
//  NAMED playback.h, NOT audio.h. macOS filesystems are case-insensitive, so a
//  local audio.h shadows the ESP32-audioI2S library's <Audio.h> - the library
//  header silently never loads and its contents get replaced by this file's,
//  producing a cascade of errors that point at the library and have nothing to
//  do with it.
//
//  Playback is TX-ONLY, but NOT because full duplex breaks it.
//
//  This comment used to say that initialising an RX channel alongside TX
//  silenced the output, and that the mic therefore had to wait. That was the
//  right symptom and the wrong cause, and it cost the mic a whole project.
//
//  Both handles must come from ONE i2s_new_channel(&cfg, &tx, &rx) call. That
//  is what makes the peripheral full duplex: TX and RX then share BCLK and WS
//  internally. Two separate calls create two independent channels fighting over
//  the same pins, which is what actually produced the silence.
//
//  Proven on this same hardware in cyd-sonar-esp32s3-4inch, which runs the mic
//  and the speaker simultaneously at 48 kHz with the tone test passing
//  untouched. Call-in is not blocked by anything in the audio path.
//
//  TX-only stands here simply because nothing in this sketch captures yet.
//
//  Everything upstream of playback goes through one interface, borrowed from
//  cyd-audio-spectrum-esp32s3-7inch/CydSpectrum/sources.h:
//
//      SRC_TONE  built-in sine        (no network, no hardware - proves the amp)
//      SRC_NET   16-bit PCM over UDP  (stream_to_cyd.py decodes on the Mac)
//
//  A future SRC_STREAM (on-board HTTP + MP3 decode) drops in behind the same
//  sourceRead() seam and nothing above it changes.
//
//  Copyright (c) 2026 Rich Sadowsky. MIT licensed - see LICENSE.
//  Written with Claude Code (Claude Opus 5).
// ============================================================================

#include <Arduino.h>
#include <driver/i2s_std.h>
#include <freertos/stream_buffer.h>
#include <math.h>

#if ENABLE_NET
#include <AsyncUDP.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#endif

#include "config.h"
#include "es8311.h"
#include "stations.h"

// SRC_STREAM is standalone: the board opens the station itself and the
// ESP32-audioI2S library owns the I2S peripheral for the duration. The other
// two sources use our own I2S path, so switching between them hands the
// peripheral over - see stream.h.
enum AudioSourceId : uint8_t {
  SRC_TONE = 0,
  SRC_NET = 1,
  SRC_STREAM = 2,
  SRC_COUNT = 3
};

static i2s_chan_handle_t s_txChan = nullptr;
static i2s_chan_handle_t s_rxChan = nullptr;
static float s_volume = VOLUME_DEFAULT;
static AudioSourceId s_source = SRC_TONE;
static bool s_muted = false;

// ---------------------------------------------------------------------------
//  I2S bring-up
// ---------------------------------------------------------------------------
static bool audioInit() {
  // Amp enable first and held low until I2S is actually clocking. Enabling the
  // amp while the data line is floating is what produces the startup pop.
  pinMode(AMP_EN_PIN, OUTPUT);
  digitalWrite(AMP_EN_PIN, !AMP_EN_ACTIVE);

  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chanCfg.dma_desc_num  = 6;
  chanCfg.dma_frame_num = 256;
  chanCfg.auto_clear    = true;  // emit silence on underrun rather than the
                                 // stale DMA buffer, which buzzes.

  // TX only - see the note at the top of this file before adding RX back.
  if (i2s_new_channel(&chanCfg, &s_txChan, nullptr) != ESP_OK) return false;

  i2s_std_config_t cfg = {
      .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_MONO),
      .gpio_cfg = {
#if AMP_USE_MCLK
          .mclk = (gpio_num_t)I2S_MCLK,
#else
          .mclk = I2S_GPIO_UNUSED,
#endif
          .bclk = (gpio_num_t)I2S_BCLK,
          .ws   = (gpio_num_t)I2S_LRCLK,
          .dout = (gpio_num_t)I2S_DOUT,
          .din  = I2S_GPIO_UNUSED,
          .invert_flags = {false, false, false},
      },
  };

  if (i2s_channel_init_std_mode(s_txChan, &cfg) != ESP_OK) return false;
  if (i2s_channel_enable(s_txChan) != ESP_OK) return false;

  // The codec must be configured *after* I2S is running: it needs MCLK present
  // to lock its internal clocks. Then raise the external PA enable, which sits
  // after the codec and is a separate thing from the codec's own power state.
  delay(50);
  bool codec = es8311Init();
  Serial.printf("[es8311] init %s\n", codec ? "ok" : "NOT FOUND");
  delay(10);
  digitalWrite(AMP_EN_PIN, AMP_EN_ACTIVE);
  return true;
}

// Release I2S_NUM_0 so ESP32-audioI2S can claim it. Everything else - codec
// config, PA enable - stays as it is; only the peripheral changes hands.
static void audioReleaseI2S() {
  if (!s_txChan) return;
  i2s_channel_disable(s_txChan);
  i2s_del_channel(s_txChan);
  s_txChan = nullptr;
}

static bool audioHaveI2S() { return s_txChan != nullptr; }

static void audioSetVolume(float v) { s_volume = constrain(v, 0.0f, 1.0f); }
static float audioVolume() { return s_volume; }

// Mute silences the speaker but leaves the stream running, so the spectrum
// keeps moving and the display still shows the station is live. Both the PA and
// the samples are zeroed: the PA alone can leave a faint residual, and zeroing
// samples alone still drives the amp.
static void audioSetMuted(bool mute) {
  s_muted = mute;
  digitalWrite(AMP_EN_PIN, mute ? !AMP_EN_ACTIVE : AMP_EN_ACTIVE);
}

static bool audioMuted() { return s_muted; }

// Blocking write of one hop to the speaker. Returns the RMS of what was played,
// which is what drives the VU meter - free, since we already touched every
// sample here.
static float audioPlay(const float *src, size_t frames) {
  static int16_t pcm[HOP_SIZE];
  if (frames > HOP_SIZE) frames = HOP_SIZE;

  // RMS is measured pre-mute so the spectrum stays live while silenced.
  const float gain = s_muted ? 0.0f : s_volume;
  float sumSq = 0.0f;
  for (size_t i = 0; i < frames; i++) {
    float s = src[i] * gain;
    sumSq += src[i] * src[i];
    if (s > 1.0f) s = 1.0f;
    if (s < -1.0f) s = -1.0f;
    pcm[i] = (int16_t)lrintf(s * 32767.0f);
  }

  size_t written = 0;
  i2s_channel_write(s_txChan, pcm, frames * sizeof(int16_t), &written, portMAX_DELAY);
  return sqrtf(sumSq / (float)frames);
}

// ---------------------------------------------------------------------------
//  Test tone - proves amp enable, I2S pins and the speaker with no network
// ---------------------------------------------------------------------------
static float s_tonePhase = 0.0f;

static size_t toneRead(float *dst, size_t frames) {
  const float step = 2.0f * (float)PI * TONE_HZ / (float)SAMPLE_RATE;
  for (size_t i = 0; i < frames; i++) {
    dst[i] = TONE_AMPLITUDE * sinf(s_tonePhase);
    s_tonePhase += step;
    if (s_tonePhase > 2.0f * (float)PI) s_tonePhase -= 2.0f * (float)PI;
  }
  return frames;
}

// ---------------------------------------------------------------------------
//  UDP PCM sink
//
//  Wire-compatible with the spectrum analyser's sender. UDP is deliberate: a
//  dropped packet costs one glitch, where TCP would stall playback to
//  retransmit. Packets that will not fit whole are dropped rather than written
//  partially - a half-written packet misaligns every sample after it.
// ---------------------------------------------------------------------------
#if ENABLE_NET
static AsyncUDP s_udp;
static StreamBufferHandle_t s_netBuf = nullptr;
static volatile uint32_t s_netPackets = 0;

// Address of whatever is feeding us audio. Tuning requests go back here - the
// sender is by definition the process that can change stations.
static IPAddress s_senderIp;
static bool s_haveSender = false;
static volatile uint32_t s_lastPacketMs = 0;
static bool s_netPrerolled = false;
static int16_t s_netTmp[HOP_SIZE];

static uint32_t s_lastJoinAttempt = 0;

static void netOnEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      // Reason codes are the only useful signal when association fails: 15 is a
      // bad password (4-way handshake timeout), 201 is "AP not found".
      Serial.printf("[wifi] disconnected, reason %d\n",
                    info.wifi_sta_disconnected.reason);
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[wifi] got ip %s\n", WiFi.localIP().toString().c_str());
      break;
    default:
      break;
  }
}

static bool netInit() {
  if (strlen(WIFI_SSID) == 0) return false;
  s_netBuf = xStreamBufferCreate(NET_BUFFER_BYTES, 1);
  if (!s_netBuf) return false;

  WiFi.onEvent(netOnEvent);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  // TX power is set after begin: applied before the interface is up it can be
  // silently ignored.
  WiFi.setTxPower(WIFI_TX_POWER);
  s_lastJoinAttempt = millis();
  return true;
}

// A failed join at boot used to be permanent - nothing ever retried, so a
// router that was briefly unreachable meant a radio that stayed dead until it
// was power-cycled. Call this from the main loop.
// One-shot diagnostic. "Cannot associate" has very different causes depending
// on whether the SSID is absent (AP down, or 5 GHz-only - this radio is 2.4 GHz
// only), present but weak, or present and strong (then it is credentials, or
// the AP refusing us). Scanning answers that directly.
//
// The scan stops the supplicant first: scanning while an association attempt is
// in flight fails with "sta is connecting".
static void netScanReport() {
  Serial.printf("[wifi] scanning for '%s'...\n", WIFI_SSID);

  // The supplicant needs time to actually stop. Scanning too soon returns -2
  // (WIFI_SCAN_FAILED), which reads like "no networks" but means "did not run".
  WiFi.disconnect(false, true);
  delay(600);

  int n = -2;
  for (int attempt = 0; attempt < 3 && n < 0; attempt++) {
    n = WiFi.scanNetworks(false, true);   // blocking, include hidden
    if (n < 0) {
      Serial.printf("[wifi] scan attempt %d failed (%d), retrying\n",
                    attempt + 1, n);
      delay(800);
    }
  }
  if (n < 0) {
    Serial.println("[wifi] scan could not run at all");
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    return;
  }
  bool found = false;
  for (int i = 0; i < n; i++) {
    bool match = WiFi.SSID(i) == WIFI_SSID;
    if (match) found = true;
    Serial.printf("[wifi]   %-28s ch%-3d %4d dBm %s%s\n", WiFi.SSID(i).c_str(),
                  WiFi.channel(i), WiFi.RSSI(i),
                  WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "enc",
                  match ? "   <-- TARGET" : "");
  }
  Serial.printf("[wifi] %d networks visible; target %s\n", n,
                found ? "IS in range" : "NOT FOUND");
  WiFi.scanDelete();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

static void netEnsureConnected() {
  if (strlen(WIFI_SSID) == 0) return;

  // Scan once, ~25 s in, if we still have not joined.
  static bool scanned = false;
  if (!scanned && WiFi.status() != WL_CONNECTED && millis() > 25000) {
    scanned = true;
    netScanReport();
    return;
  }

  if (WiFi.status() == WL_CONNECTED) return;
  // setAutoReconnect() is already retrying continuously; a manual reconnect on
  // top of it just races the supplicant. Retry rarely, as a backstop only.
  if (millis() - s_lastJoinAttempt < 60000) return;
  s_lastJoinAttempt = millis();

  Serial.println("[wifi] retrying join");
  // reconnect() rather than disconnect()+begin(): calling begin() while the
  // supplicant is mid-attempt logs "sta is connecting, cannot set config" and
  // achieves nothing.
  WiFi.reconnect();
}

static bool netConnected() { return WiFi.status() == WL_CONNECTED; }

static bool s_listening = false;

static bool netListen() {
  if (s_listening) return true;
  if (!s_udp.listen(NET_UDP_PORT)) return false;
  s_listening = true;
  s_udp.onPacket([](AsyncUDPPacket packet) {
    size_t len = packet.length() & ~(size_t)1;  // whole samples only
    if (!len) return;

    // Count every arrival, buffer only what we are about to play. The counter
    // is the liveness indicator, so it has to keep moving whether or not NET is
    // the selected source - and buffering while another source plays would fill
    // the jitter buffer with audio that is already seconds stale by the time
    // anyone switches over. A radio must change station instantly, not replay
    // a backlog.
    s_netPackets++;
    s_senderIp = packet.remoteIP();
    s_haveSender = true;
    s_lastPacketMs = millis();
    if (s_source != SRC_NET) return;

    if (xStreamBufferSpacesAvailable(s_netBuf) < len) return;  // drop whole
    xStreamBufferSend(s_netBuf, packet.data(), len, 0);
  });
  return true;
}

static uint32_t netPacketCount() { return s_netPackets; }

// Ask the sender to switch stations. Fire-and-forget: a dropped request costs
// one retry by the user, where waiting on an ack would stall the UI.
// Control uses plain WiFiUDP rather than AsyncUDP. AsyncUDP is right for the
// receive path - the audio callback must not block - but for a one-shot send it
// is a local object whose destructor can tear the socket down before the packet
// leaves. WiFiUDP's endPacket() is synchronous, which is exactly what a
// fire-and-forget control message wants.
static bool netControl(const char *verb, int stationIndex) {
  if (WiFi.status() != WL_CONNECTED) return false;

  char msg[24];
  int n = snprintf(msg, sizeof(msg), "%s %d", verb, stationIndex);

  // The sender's address is normally learned from the audio it sends us. But an
  // idle server sends nothing until it hears a heartbeat, and we cannot address
  // a heartbeat until it does - a deadlock on first boot and after every idle
  // period. Broadcasting until we know who to talk to breaks it, at the cost of
  // one small packet rather than a stream nobody wants.
  IPAddress dest = s_haveSender ? s_senderIp : WiFi.broadcastIP();

  WiFiUDP udp;
  if (!udp.begin(0)) return false;         // ephemeral source port
  bool ok = udp.beginPacket(dest, CONTROL_PORT) == 1;
  if (ok) {
    udp.write((const uint8_t *)msg, n);
    ok = udp.endPacket() == 1;
  }
  udp.stop();
  Serial.printf("[ctrl] %s -> %s %s\n", msg, dest.toString().c_str(),
                ok ? "sent" : "FAILED");
  return ok;
}

// Heartbeat: "still listening, still on this station". Also the wake signal -
// an idle server starts streaming again on the first one it sees.
static bool netHeartbeat(int stationIndex) {
  return netControl("ALIVE", stationIndex);
}

static bool netTune(int stationIndex) {
  bool ok = netControl("TUNE", stationIndex);
  // A station change means everything buffered is the previous station.
  if (s_netBuf) xStreamBufferReset(s_netBuf);
  s_netPrerolled = false;
  return ok;
}

static bool netHaveSender() { return s_haveSender; }

// True once audio has been absent long enough that the sender has probably
// restarted. A restarted server begins on its default station, so the board's
// displayed station and the actual stream would silently diverge - the display
// would keep claiming KQED while NPR played.
static bool netStreamStalled(uint32_t forMs) {
  return s_haveSender && (millis() - s_lastPacketMs) > forMs;
}

static size_t netRead(float *dst, size_t frames) {
  // Hold playback silent until the jitter buffer has a cushion, otherwise the
  // first seconds of every station stutter while the buffer fills.
  if (!s_netPrerolled) {
    if (xStreamBufferBytesAvailable(s_netBuf) < NET_PREROLL_BYTES) {
      memset(dst, 0, frames * sizeof(float));
      return frames;
    }
    s_netPrerolled = true;
  }

  size_t got = xStreamBufferReceive(s_netBuf, s_netTmp, frames * sizeof(int16_t),
                                    pdMS_TO_TICKS(NET_READ_TIMEOUT_MS));
  size_t samples = got / sizeof(int16_t);
  if (samples == 0) s_netPrerolled = false;  // ran dry; re-buffer before resuming
  for (size_t i = 0; i < samples; i++) dst[i] = s_netTmp[i] * (1.0f / 32768.0f);
  for (size_t i = samples; i < frames; i++) dst[i] = 0.0f;
  return frames;
}
#endif  // ENABLE_NET

// ---------------------------------------------------------------------------
//  Source dispatch
// ---------------------------------------------------------------------------
static void audioSetSource(AudioSourceId id) {
#if ENABLE_NET
  // Discard whatever accumulated before the switch, then re-preroll, so tuning
  // in starts from live audio rather than from the backlog.
  if (id == SRC_NET && s_netBuf) {
    xStreamBufferReset(s_netBuf);
    s_netPrerolled = false;
  }
#endif
  s_source = id;
}

static AudioSourceId audioSource() { return s_source; }

static size_t sourceRead(float *dst, size_t frames) {
  switch (s_source) {
#if ENABLE_NET
    case SRC_NET: return netRead(dst, frames);
#endif
    case SRC_TONE:
    default:      return toneRead(dst, frames);
  }
}
