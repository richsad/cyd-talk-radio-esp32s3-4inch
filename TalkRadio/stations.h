#pragma once

// ============================================================================
//  Station list.
//
//  Names only. The board never opens a stream - it receives decoded PCM - so
//  the URLs live in tools/radio_server.py on the Mac, which does the HTTPS and
//  the decoding. The two lists must stay in the SAME ORDER, because tuning is
//  sent as an index, not a name.
//
//  Tuning works by replying to whoever is sending us audio: the UDP receive
//  callback records the sender's address, and a station change is sent back
//  there on CONTROL_PORT. No configuration, no discovery - the thing feeding us
//  is by definition the thing that can retune us.
//
//  Copyright (c) 2026 Rich Sadowsky. MIT licensed - see LICENSE.
//  Written with Claude Code (Claude Opus 5).
// ============================================================================

#include <Arduino.h>

struct Station {
  const char *name;   // shown large
  const char *desc;   // shown small underneath
  const char *dial;   // short label on the tuning dial
  const char *url;    // used only in standalone mode; the host ignores it
};

// Keep in sync with STATIONS in tools/radio_server.py.
// URLs are only used by STANDALONE mode, where the board streams and decodes
// for itself. In tethered mode the host resolves the index against its own list
// in tools/radio_server.py - which must stay in the same order.
//
// All five verified: each returns exactly 3 s of audio in 3 s through ffmpeg.
static const Station kStations[] = {
    {"NPR",       "News & talk, national",    "NPR",
     "https://npr-ice.streamguys1.com/live.mp3"},
    {"SPORTS HUB","SPORTS - Boston 98.5",     "HUB",
     "https://playerservices.streamtheworld.com/api/livestream-redirect/WBZFMAAC.aac"},
    {"WNYC",      "New York public radio",    "WNYC",
     "https://fm939.wnyc.org/wnycfm"},
    {"KQED",      "San Francisco public",     "KQED",
     "https://streams.kqed.org/kqedradio"},
    {"WBEZ",      "Chicago public radio",     "WBEZ",
     "https://stream.wbez.org/wbez128.mp3"},
    {"BBC WORLD", "World service, London",    "BBC",
     "https://stream.live.vc.bbcmedia.co.uk/bbc_world_service"},
    {"LBC",       "UK talk & debate, London", "LBC",
     "https://media-ssl.musicradio.com/LBCUK"},
    {"TIMES",     "Times Radio, UK news",     "TIMES",
     "https://timesradio.wireless.radio/stream"},
    {"KNBR 680",  "SPORTS - Bay Area",        "KNBR",
     "https://playerservices.streamtheworld.com/api/livestream-redirect/KNBRAMAAC.aac"},
    {"talkSPORT", "SPORTS - UK",              "TSPT",
     "https://radio.talksport.com/stream"},
};
static const int kStationCount = sizeof(kStations) / sizeof(kStations[0]);

#define CONTROL_PORT 3334

// Idle protocol. The server has no way to know whether anyone is listening, so
// the board says so: a heartbeat while it wants audio, silence when it does
// not. Board muted for a long stretch, or simply switched off, both stop the
// heartbeat and let the host shut ffmpeg down instead of decoding into a void.
//
// HEARTBEAT_MS must be comfortably under the server's IDLE_TIMEOUT.
#define HEARTBEAT_MS   20000UL
#define MUTE_IDLE_MS  120000UL
