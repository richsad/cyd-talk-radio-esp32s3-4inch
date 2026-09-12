// ============================================================================
//  CYD Talk Radio.
//
//  A backlit-dial radio for the Hosyond ESP32-S3 4.0" module. Stations arrive
//  as decoded PCM over UDP from tools/radio_server.py, so the board never
//  touches HTTPS or an MP3 decoder; tuning is an index sent back to whoever is
//  feeding us.
//
//  Rendering is strictly incremental - every element tracks what it last drew
//  and repaints only the difference. A full-screen or full-row repaint on a
//  timer reads as flicker on an SPI panel at this size, which cost real time to
//  diagnose during bring-up.
//
//  Hardware facts, all measured rather than taken from the vendor listing:
//  ST7796 320x480 native (ROT 1 = 480x320 landscape), FT6336 touch at 0x38,
//  ES8311 codec at 0x18, PA enable on GPIO1 ACTIVE LOW.
//
//  Copyright (c) 2026 Rich Sadowsky. MIT licensed - see LICENSE.
//  Written with Claude Code (Claude Opus 5).
// ============================================================================

#include <Arduino_GFX_Library.h>

#include "config.h"
#include "playback.h"
#include "spectrum.h"
#include "standalone.h"
#include "ui.h"
#include "stations.h"
#include "touch.h"

static Arduino_DataBus *bus =
    new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI, LCD_MISO);
static Arduino_GFX *gfx =
    new Arduino_ST7796(bus, LCD_RST, LCD_ROTATION, LCD_IPS, LCD_W, LCD_H);

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// Warm backlit-dial palette: amber and cream on a near-black ground, with the
// spectrum running green through amber to red the way a real level meter does.
static uint16_t C_BG, C_PANEL, C_DIM, C_TEXT, C_AMBER, C_DEEP, C_RED, C_GREEN,
    C_NEEDLE, C_MUTE_HI, C_MUTE_LO;

static void paletteInit() {
  C_BG     = rgb565(10, 9, 8);
  C_PANEL  = rgb565(26, 22, 18);
  C_DIM    = rgb565(96, 78, 54);
  C_TEXT   = rgb565(236, 222, 196);
  C_AMBER  = rgb565(255, 176, 48);
  C_DEEP   = rgb565(148, 96, 20);
  C_RED    = rgb565(232, 74, 52);
  C_GREEN  = rgb565(104, 196, 108);
  C_NEEDLE = rgb565(255, 92, 60);
  C_MUTE_HI = rgb565(84, 78, 70);
  C_MUTE_LO = rgb565(52, 48, 44);
}

// --- layout ----------------------------------------------------------------
#define HDR_H      42
#define NAME_Y     52
#define DESC_Y     98
#define SPEC_TOP  126
#define SPEC_H    102
#define SPEC_BOT  (SPEC_TOP + SPEC_H)
#define DIAL_TOP  240
#define DIAL_H     46
#define VOL_TOP   294
#define VOL_H      22

#define BAND_PITCH (SCREEN_W / SPEC_BANDS)
#define BAR_W      (BAND_PITCH - 4)
#define SEG_H        6
#define SEG_GAP      2
#define SEG_PITCH   (SEG_H + SEG_GAP)
#define SEG_COUNT   (SPEC_H / SEG_PITCH)

static int s_station = 0;
static uint32_t s_mutedSince = 0;

// DIRECT: the board streams the station itself, no host involved.
// HOST:   PCM arrives over UDP from tools/radio_server.py.
// Direct is the default - needing a Mac switched on is the thing this mode
// exists to remove. Host mode stays for server-generated content.
static bool s_direct = true;

// Mode, station and volume survive the restart that a mode change requires.
//
// NOINIT, not RTC_DATA_ATTR: RTC_DATA_ATTR variables live in an initialised
// section, so a normal boot after ESP.restart() reloads their startup values
// and the setting is lost. RTC_NOINIT_ATTR is never initialised, which is why
// the magic is required - on a cold power-on these hold garbage.
#define RTC_MAGIC 0x5AD10001UL
RTC_NOINIT_ATTR static uint32_t s_rtcMagic;
RTC_NOINIT_ATTR static uint8_t  s_rtcDirect;
RTC_NOINIT_ATTR static uint8_t  s_rtcStation;
RTC_NOINIT_ATTR static uint8_t  s_rtcVolPct;

// Switching mode restarts the board. The streaming engine cannot be destroyed
// safely - its destructor frees the I2S channel and its mutexes before stopping
// its own decode task, which corrupts the heap - so it can never hand the I2S
// peripheral back to playback.h within a single run. A restart is the honest
// way to change owner, and a mode switch is a rare, settings-level action
// rather than something done while listening.
static void switchModeAndRestart(bool direct) {
  s_rtcMagic = RTC_MAGIC;
  s_rtcDirect = direct ? 1 : 0;
  s_rtcStation = (uint8_t)s_station;
  s_rtcVolPct = (uint8_t)(audioVolume() * 100.0f + 0.5f);

  gfx->fillRect(0, NAME_Y - 4, SCREEN_W, 60, C_BG);
  gfx->setTextColor(C_AMBER);
  gfx->setTextSize(3);
  gfx->setCursor(14, NAME_Y);
  gfx->printf("-> %s", direct ? "DIRECT" : "HOST");
  Serial.printf("[mode] restarting into %s\n", direct ? "DIRECT" : "HOST");
  delay(600);
  ESP.restart();
}

// Mode and station changes are REQUESTED here and performed by the audio task.
// Doing the work inline from the touch handler meant core 1 reconfiguring - and
// in the HOST case deleting - an Audio object that core 0 was executing inside.
// When the current tune was requested. Until audio actually arrives the
// display is showing intent, not reality, so the indicator says TUNING rather
// than ON AIR - and says NO SIGNAL if nothing shows up.
static uint32_t s_tuneAt = 0;
#define TUNE_GRACE_MS 12000UL

static volatile int  s_reqStation = -1;
static volatile bool s_reqDirect = true;
static volatile bool s_reqPending = false;

static void applyMode(int station) {
  s_tuneAt = millis();
  s_reqStation = station;
  s_reqDirect = s_direct;
  s_reqPending = true;
}

// Runs on the audio task only.
static void serviceModeRequest() {
  if (!s_reqPending) return;
  s_reqPending = false;
  int station = s_reqStation;
  bool direct = s_reqDirect;

  if (direct) {
    audioSetSource(SRC_STREAM);
    streamBegin(station, audioVolume());
  } else {
    // The engine is never created in HOST mode, so there is nothing to stop.
    audioSetSource(SRC_NET);
    netTune(station);
  }
}

// Muted briefly is still "listening" - the stream keeps running so unmute is
// instant and the spectrum stays alive. Muted for MUTE_IDLE_MS means the radio
// is genuinely not in use, and the host should stop decoding.
static bool wantsAudio() {
  if (!audioMuted()) return true;
  return (millis() - s_mutedSince) < MUTE_IDLE_MS;
}
static volatile float s_rms = 0.0f;

// --- audio + analysis task (core 0) ----------------------------------------
static void audioTask(void *) {
  static float buf[HOP_SIZE];
  const float dt = (float)HOP_SIZE / (float)SAMPLE_RATE;
  for (;;) {
    serviceModeRequest();
    if (audioSource() == SRC_STREAM) {
      // Standalone: the library owns I2S and does the decoding. Its loop() is
      // where all the streaming work happens, so it wants calling often; the
      // spectrum is fed from the audio_process_i2s hook instead of from here.
      streamLoop();
      s_rms = streamRms();
      vTaskDelay(1);
    } else {
      sourceRead(buf, HOP_SIZE);
      s_rms = audioPlay(buf, HOP_SIZE);
      // Analyse exactly what was played - the block is already here and already
      // scaled, so the spectrum costs one FFT and no extra memory.
      spectrumProcess(buf, dt);
    }
  }
}

// --- spectrum --------------------------------------------------------------
static uint8_t s_drawnSeg[SPEC_BANDS];
static int8_t  s_drawnPeak[SPEC_BANDS];

static uint16_t segColor(int seg) {
  int top = SEG_COUNT - 1;
  if (audioMuted()) return seg >= top - 4 ? C_MUTE_HI : C_MUTE_LO;
  if (seg >= top - 1) return C_RED;
  if (seg >= top - 4) return C_AMBER;
  return C_GREEN;
}

static void spectrumInvalidate() {
  gfx->fillRect(0, SPEC_TOP - 2, SCREEN_W, SPEC_H + 4, C_BG);
  memset(s_drawnSeg, 0, sizeof(s_drawnSeg));
  memset(s_drawnPeak, -1, sizeof(s_drawnPeak));
}

static void drawSpectrum() {
  static float level[SPEC_BANDS], peak[SPEC_BANDS];
  spectrumSnapshot(level, peak);

  for (int b = 0; b < SPEC_BANDS; b++) {
    int x = b * BAND_PITCH + 2;
    int segs = (int)(level[b] * SEG_COUNT + 0.5f);
    if (segs > SEG_COUNT) segs = SEG_COUNT;
    int prev = s_drawnSeg[b];

    // Only paint the segments that changed state.
    if (segs > prev) {
      for (int s = prev; s < segs; s++) {
        int y = SPEC_BOT - (s + 1) * SEG_PITCH;
        gfx->fillRect(x, y, BAR_W, SEG_H, segColor(s));
      }
    } else if (segs < prev) {
      for (int s = segs; s < prev; s++) {
        int y = SPEC_BOT - (s + 1) * SEG_PITCH;
        gfx->fillRect(x, y, BAR_W, SEG_H, C_BG);
      }
    }
    s_drawnSeg[b] = segs;

    int pk = (int)(peak[b] * SEG_COUNT + 0.5f) - 1;
    if (pk >= SEG_COUNT) pk = SEG_COUNT - 1;
    if (pk != s_drawnPeak[b]) {
      int old = s_drawnPeak[b];
      if (old >= 0) {
        int y = SPEC_BOT - (old + 1) * SEG_PITCH;
        gfx->fillRect(x, y, BAR_W, SEG_H, old < segs ? segColor(old) : C_BG);
      }
      if (pk >= 0) {
        int y = SPEC_BOT - (pk + 1) * SEG_PITCH;
        gfx->fillRect(x, y, BAR_W, SEG_H, C_TEXT);
      }
      s_drawnPeak[b] = pk;
    }
  }
}

// --- tuning dial -----------------------------------------------------------
static int stationX(int i) {
  int slot = SCREEN_W / kStationCount;
  return slot * i + slot / 2;
}

static void drawDial(bool full) {
  if (full) {
    gfx->fillRect(0, DIAL_TOP, SCREEN_W, DIAL_H, C_PANEL);
    gfx->drawFastHLine(0, DIAL_TOP, SCREEN_W, C_DIM);
    // Fine ticks across the whole span, taller ones at each station.
    for (int x = 6; x < SCREEN_W; x += 8)
      gfx->drawFastVLine(x, DIAL_TOP + 6, 5, C_DEEP);
    for (int i = 0; i < kStationCount; i++) {
      int x = stationX(i);
      gfx->drawFastVLine(x, DIAL_TOP + 4, 10, C_AMBER);
      gfx->setTextSize(1);
      gfx->setTextColor(C_DIM);
      int w = strlen(kStations[i].dial) * 6;
      gfx->setCursor(x - w / 2, DIAL_TOP + 32);
      gfx->print(kStations[i].dial);
    }
  }

  static int drawnNeedle = -1;
  if (drawnNeedle == s_station && !full) return;

  if (drawnNeedle >= 0 && drawnNeedle != s_station) {
    // Erase the old needle and restore the tick and label underneath it.
    int ox = stationX(drawnNeedle);
    gfx->fillRect(ox - 8, DIAL_TOP + 2, 17, 26, C_PANEL);
    for (int x = ox - 8; x <= ox + 8; x += 1)
      if (((x - 6) % 8) == 0) gfx->drawFastVLine(x, DIAL_TOP + 6, 5, C_DEEP);
    gfx->drawFastVLine(ox, DIAL_TOP + 4, 10, C_AMBER);
    gfx->setTextSize(1);
    gfx->setTextColor(C_DIM);
    int ow = strlen(kStations[drawnNeedle].dial) * 6;
    gfx->fillRect(ox - ow / 2 - 2, DIAL_TOP + 30, ow + 4, 10, C_PANEL);
    gfx->setCursor(ox - ow / 2, DIAL_TOP + 32);
    gfx->print(kStations[drawnNeedle].dial);
  }

  int nx = stationX(s_station);
  for (int dy = 0; dy < 24; dy++) {
    int half = 6 - dy / 5;
    if (half < 1) half = 1;
    gfx->drawFastHLine(nx - half, DIAL_TOP + 2 + dy, half * 2 + 1, C_NEEDLE);
  }
  gfx->setTextSize(1);
  gfx->setTextColor(C_AMBER);
  int w = strlen(kStations[s_station].dial) * 6;
  gfx->fillRect(nx - w / 2 - 2, DIAL_TOP + 30, w + 4, 10, C_PANEL);
  gfx->setCursor(nx - w / 2, DIAL_TOP + 32);
  gfx->print(kStations[s_station].dial);
  drawnNeedle = s_station;
}

// --- station name ----------------------------------------------------------
static void drawModeBadge() {
  static int drawn = -1;
  int m = s_direct ? 1 : 0;
  if (drawn == m) return;
  drawn = m;
  gfx->fillRect(SCREEN_W - 190, 10, 84, 24, C_BG);
  gfx->drawRoundRect(SCREEN_W - 190, 10, 80, 24, 5, s_direct ? C_AMBER : C_DIM);
  gfx->setTextColor(s_direct ? C_AMBER : C_DIM);
  gfx->setTextSize(1);
  gfx->setCursor(SCREEN_W - 181, 18);
  gfx->print(s_direct ? "DIRECT" : "  HOST");
}

static void drawStation() {
  gfx->fillRect(0, NAME_Y - 4, SCREEN_W, DESC_Y - NAME_Y + 22, C_BG);
  gfx->setTextColor(C_AMBER);
  // The longest name ("SPORTS HUB") needs a smaller face to fit 480 px at
  // size 5, which is 30 px per character.
  gfx->setTextSize(strlen(kStations[s_station].name) > 9 ? 4 : 5);
  gfx->setCursor(14, NAME_Y);
  gfx->print(kStations[s_station].name);
}

// The subtitle is a single function of current state: the ICY title when the
// stream provides one, the station's own description otherwise. Deriving it in
// one place is what fixes the stale-subtitle bug - drawStation() used to read
// the title directly, but it runs on the touch handler while the station change
// is still queued for the audio task, so the title it saw belonged to the
// PREVIOUS station. Falling back to the description also covers streams that
// never send ICY metadata at all, which several of the AAC sports feeds do not.
// Some streams send placeholder ICY titles rather than none at all - Times
// Radio sends a single underscore. Anything without at least two letters or
// digits is treated as absent, so the station description shows instead.
static bool usableTitle(const char *t) {
  if (!t) return false;
  int alnum = 0;
  for (const char *c = t; *c; c++)
    if (isalnum((unsigned char)*c) && ++alnum >= 2) return true;
  return false;
}

static void refreshTitle() {
  static char shown[96] = "";
  const char *t = streamTitle();
  const char *want = usableTitle(t) ? t : kStations[s_station].desc;
  if (strncmp(shown, want, sizeof(shown) - 1) == 0) return;
  snprintf(shown, sizeof(shown), "%s", want);

  gfx->fillRect(0, DESC_Y - 2, SCREEN_W, 22, C_BG);
  gfx->setTextColor(C_DIM);
  gfx->setTextSize(2);
  gfx->setCursor(16, DESC_Y);
  gfx->print(want);
}

// --- header + volume -------------------------------------------------------
static void drawVolume(bool full) {
  static int drawn = -1;
  int w = (int)(audioVolume() * (SCREEN_W - 24) + 0.5f);
  if (full) {
    gfx->drawRect(10, VOL_TOP, SCREEN_W - 20, VOL_H, C_DIM);
    gfx->fillRect(11, VOL_TOP + 1, SCREEN_W - 22, VOL_H - 2, C_BG);
    drawn = -1;
  }
  if (w == drawn) return;
  if (w > drawn) gfx->fillRect(12 + (drawn < 0 ? 0 : drawn), VOL_TOP + 2,
                               w - (drawn < 0 ? 0 : drawn), VOL_H - 4, C_DEEP);
  else gfx->fillRect(12 + w, VOL_TOP + 2, drawn - w, VOL_H - 4, C_BG);
  drawn = w;

  gfx->fillRect(SCREEN_W - 96, 8, 88, 26, C_BG);
  gfx->setTextColor(C_TEXT);
  gfx->setTextSize(2);
  gfx->setCursor(SCREEN_W - 92, 12);
  gfx->printf("VOL %3d", (int)(audioVolume() * 100 + 0.5f));
}

// Derived from what is happening, not from what was asked for.
static HdrState headerState() {
  if (!wantsAudio()) return HDR_PAUSED;
  if (audioMuted()) return HDR_MUTED;
  if (s_rms > 0.0015f) return HDR_ONAIR;
  // A refused connect is known immediately; no point waiting out the grace.
  if (s_direct && !streamConnectOk()) return HDR_NOSIGNAL;
  // TLS plus an AAC stream can take several seconds to produce first audio.
  if (millis() - s_tuneAt < TUNE_GRACE_MS) return HDR_TUNING;
  return HDR_NOSIGNAL;
}

static void drawHeader() {
  const HdrState state = headerState();
  static int drawn = -1;
  static uint32_t lastBlink = 0;
  static bool blinkOn = false;

  // TUNING blinks, so a stall is visibly different from a steady state.
  bool blink = (state == HDR_TUNING);
  if (blink && millis() - lastBlink > 400) {
    lastBlink = millis();
    blinkOn = !blinkOn;
  } else if (!blink) {
    blinkOn = true;
  }

  int key = (int)state * 2 + (blink ? (int)blinkOn : 1);
  if (drawn == key) return;
  drawn = key;

  const char *label;
  uint16_t dot, text;
  switch (state) {
    case HDR_ONAIR:    label = "ON AIR";    dot = C_RED;    text = C_TEXT;  break;
    case HDR_TUNING:   label = "TUNING";    dot = C_AMBER;  text = C_AMBER; break;
    case HDR_NOSIGNAL: label = "NO SIGNAL"; dot = C_NEEDLE; text = C_NEEDLE; break;
    case HDR_MUTED:    label = "MUTED";     dot = C_DIM;    text = C_AMBER; break;
    default:           label = "PAUSED";    dot = C_DIM;    text = C_AMBER; break;
  }

  gfx->fillRect(8, 8, 200, 26, C_BG);
  gfx->fillCircle(18, 21, 6, blinkOn ? dot : C_BG);
  gfx->drawCircle(18, 21, 6, dot);
  gfx->setTextColor(text);
  gfx->setTextSize(2);
  gfx->setCursor(32, 13);
  gfx->print(label);
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  // Never block on USB. This is the S3's native USB-Serial-JTAG port: once the
  // host has enumerated it but no application is reading, the TX buffer fills
  // and Serial.write stalls waiting for a reader that is not coming.
  //
  // The [status] line below goes out every 5 s from loop(), and loop() is also
  // what polls touch - so with nothing draining the port, the screen stops
  // responding while audio carries on, because playback runs in its own task.
  //
  // Found the hard way in cyd-sonar-esp32s3-4inch, where the same pattern made
  // the buttons look intermittently dead for hours. Attaching a serial monitor
  // to investigate drains the buffer and makes it vanish, so it presents as a
  // flaky touch controller rather than as a blocked print.
  Serial.setTxTimeoutMs(0);
  ledcAttach(LCD_BL, BL_PWM_FREQ_HZ, BL_PWM_BITS);
  ledcWrite(LCD_BL, BL_LEVEL_DAY);

  if (s_rtcMagic == RTC_MAGIC) {
    s_direct = s_rtcDirect != 0;
    if (s_rtcStation < kStationCount) s_station = s_rtcStation;
  }

  paletteInit();
  gfx->begin(LCD_SPI_HZ);
  gfx->fillScreen(C_BG);
  gfx->drawFastHLine(0, HDR_H, SCREEN_W, C_DIM);

  Wire.begin(TOUCH_SDA, TOUCH_SCL, TOUCH_I2C_HZ);
  touchInit();
  spectrumInit();
  audioInit();

#if ENABLE_NET
  if (netInit()) {
    for (int i = 0; i < 40 && !netConnected(); i++) delay(250);
    if (netConnected()) netListen();
    Serial.printf("[net] %s\n", WiFi.localIP().toString().c_str());
  }
#endif

  if (s_rtcMagic == RTC_MAGIC && s_rtcVolPct <= 100)
    audioSetVolume(s_rtcVolPct / 100.0f);

  applyMode(s_station);
  xTaskCreatePinnedToCore(audioTask, "audio", 16384, nullptr, 5, nullptr, 0);

  drawStation();
  drawModeBadge();
  drawDial(true);
  drawVolume(true);
  drawHeader();
}

void loop() {
  static uint32_t lastPoll = 0, lastDraw = 0, latch = 0, lastLog = 0;

  // 50 Hz. Polling every iteration wedges the FT6336 - see touch.h.
  if (millis() - lastPoll >= 20) {
    lastPoll = millis();
    TouchPoint p = touchRead();
    if (p.pressed && millis() - latch > 120) {
      latch = millis();

      if (p.y < HDR_H) {
        switchModeAndRestart(!s_direct);
      } else if (p.y >= SPEC_TOP - 6 && p.y < SPEC_BOT + 6) {
        bool nowMuted = !audioMuted();
        audioSetMuted(nowMuted);
        s_mutedSince = millis();
        spectrumInvalidate();
        // Unmuting from the paused state must wake the host immediately rather
        // than waiting for the next heartbeat.
        if (!nowMuted) netTune(s_station);
        Serial.printf("[mute] %s\n", audioMuted() ? "on" : "off");
      } else if (p.y >= DIAL_TOP - 6 && p.y < DIAL_TOP + DIAL_H) {
        // Tune to whichever station marker is nearest the touch.
        int slot = SCREEN_W / kStationCount;
        int idx = constrain(p.x / slot, 0, kStationCount - 1);
        if (idx != s_station) {
          s_station = idx;
          drawStation();
          drawDial(false);
          applyMode(idx);
          if (audioMuted()) {
            audioSetMuted(false);
            spectrumInvalidate();
          }
          Serial.printf("[tune] %d %s\n", idx, kStations[idx].name);
        }
      } else if (p.y >= VOL_TOP - 8) {
        // Absolute volume: tap where you want it, no repeated presses.
        audioSetVolume((float)(p.x - 12) / (float)(SCREEN_W - 24));
        streamSetVolume(audioVolume());
        drawVolume(false);
      }
    }
  }

  if (millis() - lastDraw < 33) return;  // ~30 fps
  lastDraw = millis();

  drawSpectrum();
  drawHeader();
  refreshTitle();

#if ENABLE_NET
  // Re-assert the tuned station after a gap. Cheap, idempotent, and it keeps
  // the dial honest across a server restart without any handshake.
  static uint32_t lastAssert = 0;
  if (!s_direct && wantsAudio() && netStreamStalled(4000) &&
      millis() - lastAssert > 5000) {
    lastAssert = millis();
    netTune(s_station);
    Serial.printf("[tune] re-asserting %s after stream gap\n",
                  kStations[s_station].name);
  }
#endif

#if ENABLE_NET
  // Keep trying to join, and start listening as soon as we do - the socket
  // cannot be opened before the interface is up.
  netEnsureConnected();
  if (netConnected()) netListen();

  static uint32_t lastBeat = 0;
  // Beat quickly until a sender is known, so first boot connects in seconds
  // rather than waiting out a full heartbeat interval.
  uint32_t beatEvery = netHaveSender() ? HEARTBEAT_MS : 3000UL;
  if (!s_direct && wantsAudio() && millis() - lastBeat > beatEvery) {
    lastBeat = millis();
    netHeartbeat(s_station);
  }
#endif

  if (millis() - lastLog > 5000 && Serial) {
    lastLog = millis();
    Serial.printf("[status] up=%lus station=%s vol=%d%% rms=%.4f pkt=%lu "
                  "sender=%d fail=%u muted=%d want=%d wifi=%d ip=%s\n",
                  (unsigned long)(millis() / 1000), kStations[s_station].name,
                  (int)(audioVolume() * 100), s_rms,
#if ENABLE_NET
                  (unsigned long)netPacketCount(), (int)netHaveSender(),
#else
                  0UL, 0,
#endif
                  (unsigned)touchFailStreak(), (int)audioMuted(),
                  (int)wantsAudio(), (int)WiFi.status(),
                  WiFi.localIP().toString().c_str());
  }
}
