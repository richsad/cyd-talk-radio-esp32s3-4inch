# CYD Talk Radio — Hosyond ESP32-S3 4.0"

A backlit-dial internet talk radio for the Hosyond ESP32-S3 4.0" touchscreen
module. Audio plays out the board's own speaker header.

Two modes, switched by tapping the header:

- **DIRECT** (default) — the board opens the station itself over HTTPS and
  decodes it on-chip. No computer involved.
- **HOST** — decoded PCM arrives over UDP from `tools/radio_server.py`. Kept for
  server-generated content, where the host has to produce the audio anyway.

Tap the dial to tune. Tap the volume bar to set the level. A 30-band FFT
spectrum runs across the middle, driven by the exact audio block just played.

---

## The vendor listing is wrong

Three specs on the product page do not match the hardware. All three were
measured, and each is documented in
[`TalkRadio/config.h`](TalkRadio/config.h) with the evidence:

| | Listed | Actually |
|---|---|---|
| Panel | 240×320 | **320×480** (ROT 1 → 480×320 landscape) |
| Driver | ILI9341 | **ST7796** |
| Audio | I2S amp, `IO1` enable | **ES8311 codec**, `IO1` **active LOW** |

Hosyond's 2.8"/3.5"/4.0" ESP32-S3 boards are all listed as 240×320, which no
3.5" or 4.0" panel is — the 2.8" spec appears to have been pasted across the
line. Treat [LCD wiki's 2.8" page](https://www.lcdwiki.com/2.8inch_ESP32-S3_Display)
as a starting point, not a reference.

## Build modes

DIRECT needs the MP3 decoder linked in, which does not fit the default
partition:

```sh
arduino-cli lib install "ESP32-audioI2S-master"
```

and build with `PartitionScheme=huge_app` (3 MB app). The sketch is ~2 MB with
the decoder, against a 1.31 MB default partition.

## Setup

Arduino IDE or `arduino-cli`, ESP32 core 3.x, **ESP32S3 Dev Module**, PSRAM
enabled. Requires [`Arduino_GFX`](https://github.com/moononournation/Arduino_GFX)
and `ffmpeg` on the host (`brew install ffmpeg`).

```sh
cp TalkRadio/secrets.h.example TalkRadio/secrets.h   # then fill in Wi-Fi
```

`secrets.h` is gitignored and compiled in — **editing it requires a reflash**.

```sh
arduino-cli compile --upload -p /dev/cu.usbmodemXXXX \
  --fqbn esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=huge_app,CDCOnBoot=cdc \
  TalkRadio
```

`CDCOnBoot=cdc` routes `Serial` over the native USB port. The board prints its
IP on the serial monitor; point the server at it:

```sh
tools/radio_server.py 192.168.1.42
```

## How it works

The board never opens a stream. Most stations are HTTPS now, and TLS plus an AAC
decode on an S3 that is also driving a 480×320 panel is a bad trade. `ffmpeg`
decodes and resamples on the host; the board receives mono 16-bit PCM at 32 kHz
over UDP and hands it to the codec.

UDP is deliberate — a dropped packet costs one glitch, where TCP would stall
playback to retransmit.

**Neither end is configured with the other's address.** The board broadcasts a
heartbeat until it learns who is feeding it; the server takes the board's
address from whichever control packet arrives. A DHCP lease change on either
side heals within one heartbeat. Station names live in
[`TalkRadio/stations.h`](TalkRadio/stations.h) and URLs in
[`tools/radio_server.py`](tools/radio_server.py) — **same order in both**, since
tuning is sent as an index.

**The server idles when nobody is listening.** The board heartbeats every 20 s
while it wants audio and stops when it is paused or switched off; after 75 s of
silence the server stops ffmpeg entirely and waits. Nothing decodes into a void,
which matters on a laptop.

The trade-off is that this is **tethered**: it needs the host running. Making it
standalone means on-board HTTP streaming behind the existing `sourceRead()` seam.

## Using it

| Gesture | Effect |
|---|---|
| Tap the header | Toggles DIRECT / HOST. **Restarts the board** - see below. The badge top-right shows which. |
| Tap a station on the dial | Tunes. In DIRECT the board reconnects; in HOST the server switches stream. |
| Tap anywhere on the spectrum | Mute / unmute. The bars keep moving, drained to grey. |
| Tap anywhere on the volume bar | Sets volume directly to that position. |

Muted for two minutes drops to **PAUSED**: the heartbeat stops and the host
shuts ffmpeg down. Any touch resumes it.

The header indicator reports what is actually happening, not what was asked
for: **ON AIR** only once audio is flowing, **TUNING** (blinking) while waiting,
**NO SIGNAL** if the connect was refused or nothing arrived within 12 s.

Switching mode restarts the board, carrying mode, station and volume across in
RTC memory. The streaming engine cannot be torn down safely - its destructor
frees the I2S channel before stopping its own decode task - so it can never
return the peripheral within a single run.

## Naming traps on macOS

Two of these cost real time, so they are worth stating plainly.

**Header filenames are case-insensitive.** A sketch header shadows any core or
library header whose name differs only in case. `audio.h` shadowed the library's
`<Audio.h>`; `stream.h` shadowed Arduino's `<Stream.h>` and derailed `Arduino.h`
itself, producing errors about `Serial` being undefined in core files. Hence
`playback.h` and `standalone.h`. Avoid `Stream`, `Audio`, `Client`, `Server`,
`Print`, `WiFi`, `Update` as filenames here.

**Sketch macros have no namespace.** ESP32-audioI2S declares `FFT_SIZE` and
`NUM_BANDS` as struct members; unprefixed `#define`s rewrote those declarations
into `const uint16_t 1024 = 512;`, with the error blamed on `config.h`. Ours are
`SPEC_`-prefixed. When adding a library, check your macro names against it.

## If it goes wrong

| Symptom | Cause |
|---|---|
| Silence, everything else healthy | The ES8311 needs an I2C register init and MCLK. Clocking I2S is not enough. |
| Silence after changing volume | `ES8311 0x32` is logarithmic: `−95.5 + 0.5 × value` dB. `0xBF` is unity, `0x90` is −23.5 dB. |
| Silence with the codec configured | I2S must be **TX-only**. Creating an RX channel alongside TX mutes the output. |
| Touch works then stops | Do not poll the FT6336 every loop iteration. 50 Hz is plenty; faster corrupts reads and wedges it. |
| Screen flickers | Something is full-repainting on a timer. Draw incrementally. |
| Board vanishes from USB | Something drove **GPIO19/20** — the S3's USB D−/D+. Recover via ROM download mode (hold BOOT while plugging in). |
| Blank screen, flashes fine, no serial | Still in download mode. Press RESET on its own. |
| Audio flutters, "like a fan" | Packet rate below 62.5/s. Check `[pace]` lines in the log; under launchd it means `ProcessType: Interactive` is missing. |
| `Host is down` in the log forever | Stale board address. Fixed — the server now learns it from heartbeats — but check the board actually joined Wi-Fi (`wifi=3` in its serial status). |
| Board won't join Wi-Fi, `AUTH_EXPIRE` | Usually a wedged radio: fully power-cycle the board. It scans and lists visible SSIDs on serial after ~25 s if it still cannot join. |
| Service says `ffmpeg not found` | launchd's minimal `PATH`. See the plist's `EnvironmentVariables`. |

## Status

Working: panel, touch, audio out, Wi-Fi with reconnect, **standalone HTTPS
streaming and on-board MP3 decode**, UDP playback, FFT spectrum in both modes,
ICY stream titles, tuning dial, mute, volume, zero-config discovery, host
idle/resume, and a LaunchAgent.

**All five stations verified** — each returns exactly 3 s of audio in 3 s.
C-SPAN was dropped for LBC after failing TLS certificate verification.

Not built: the AI station, mic call-in, sleep timer. See [DESIGN.md](DESIGN.md).

## Enclosure

A 3D-printable retro tabletop-radio case lives in [`enclosure/`](enclosure/) —
parametric, two parts, four screws, no supports. Board dimensions in it are
still placeholders pending measurement.

## License and credits

MIT — see [LICENSE](LICENSE).

Builds directly on **CydSpectrum** (`../cyd-audio-spectrum-esp32s3-7inch`),
which is the source of the FFT and band mapping in
[`TalkRadio/spectrum.h`](TalkRadio/spectrum.h), the pluggable audio-source
pattern, the core-0/core-1 split, and the UDP PCM wire format. This project
reuses that transport and adds the I2S *output* path, which CydSpectrum never
needed since it only ever analysed audio.

Written with [Claude Code](https://claude.com/claude-code) (Claude Opus 5).
