#pragma once

// ============================================================================
//  UI types.
//
//  Exists because Arduino generates function prototypes and inserts them
//  directly after the last #include - above anything declared in the sketch
//  body. A type used in a function signature must therefore come from a header,
//  or the build fails with "'HdrState' does not name a type" pointing at the
//  signature rather than at the declaration.
//
//  Copyright (c) 2026 Rich Sadowsky. MIT licensed - see LICENSE.
//  Written with Claude Code (Claude Opus 5).
// ============================================================================

#include <Arduino.h>

// What the header indicator is reporting. Derived from what is actually
// happening - audio arriving, a refused connect - rather than from what the
// user last asked for, so the display cannot claim a station that is not
// playing.
enum HdrState : uint8_t {
  HDR_ONAIR = 0,   // audio actually flowing
  HDR_TUNING,      // change requested, nothing arrived yet
  HDR_NOSIGNAL,    // gave up waiting, or the connect was refused
  HDR_MUTED,
  HDR_PAUSED,
};
