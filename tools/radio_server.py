#!/usr/bin/env python3
"""Stream talk radio to the CYD as mono 16-bit PCM, and switch stations on request.

The board never opens a stream itself. Most stations are HTTPS now, and TLS plus
an AAC decode on an ESP32-S3 that is also driving a 480x320 panel is a bad trade.
So ffmpeg does the work here and the board receives raw PCM it can hand straight
to the codec.

Neither end is configured with the other's address. The board broadcasts a
heartbeat until it knows who is feeding it, and this server takes the board's
address from whichever control packet arrives - so a DHCP lease change on either
side fixes itself within one heartbeat. Hard-coding the board's IP here meant a
morning of "Host is down" the first time its lease moved.

    ./radio_server.py                 # learns the board's address itself
    ./radio_server.py --station 2

Copyright (c) 2026 Rich Sadowsky. MIT licensed - see LICENSE.
Written with Claude Code (Claude Opus 5).
"""

import argparse
import os
import shutil
import socket
import subprocess
import sys
import threading
import time

AUDIO_PORT = 3333
CONTROL_PORT = 3334
RATE = 32000
SAMPLES_PER_PACKET = 512          # 1024 bytes, comfortably inside one MTU
BYTES_PER_PACKET = SAMPLES_PER_PACKET * 2

# Stop decoding when nothing is listening. The board heartbeats every 20 s while
# it wants audio and stops when it is paused or switched off, so this only has
# to be comfortably longer than one heartbeat. Without it, ffmpeg would decode a
# stream to nobody around the clock - real CPU and battery on a laptop, since
# UDP never notices the far end is gone.
IDLE_TIMEOUT = 75.0

# How far behind schedule the sender may fall before abandoning the backlog.
# Below this it simply stops sleeping and catches up, which is what keeps the
# average rate at exactly 62.5 packets/s despite imprecise sleeps.
MAX_LAG = 0.5

# MUST stay in the same order as kStations[] in TalkRadio/stations.h - the board
# tunes by index, not by name.
STATIONS = [
    ("NPR",        "https://npr-ice.streamguys1.com/live.mp3"),
    ("SPORTS HUB", "https://playerservices.streamtheworld.com/api/livestream-redirect/WBZFMAAC.aac"),
    ("WNYC",       "https://fm939.wnyc.org/wnycfm"),
    ("KQED",       "https://streams.kqed.org/kqedradio"),
    ("WBEZ",       "https://stream.wbez.org/wbez128.mp3"),
    ("BBC WORLD",  "https://stream.live.vc.bbcmedia.co.uk/bbc_world_service"),
    ("LBC",        "https://media-ssl.musicradio.com/LBCUK"),
    ("TIMES",      "https://timesradio.wireless.radio/stream"),
    ("KNBR 680",   "https://playerservices.streamtheworld.com/api/livestream-redirect/KNBRAMAAC.aac"),
    ("talkSPORT",  "https://radio.talksport.com/stream"),
]


def require_ffmpeg() -> str:
    """Locate ffmpeg without depending on PATH.

    Under launchd, PATH is /usr/bin:/bin:/usr/sbin:/sbin - Homebrew is not on
    it, so which() finds nothing and the service crash-loops with a message that
    looks like ffmpeg is missing when it is installed and working. Fall back to
    the usual install locations.
    """
    path = shutil.which("ffmpeg")
    if path:
        return path
    for candidate in ("/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg",
                      "/opt/local/bin/ffmpeg"):
        if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    sys.exit("ffmpeg not found. Install it with: brew install ffmpeg")


class Tuner:
    """Owns the current ffmpeg process and swaps it out on a station change."""

    def __init__(self, host, index: int):
        # None until a control packet tells us where the board is.
        self.host = host
        self.index = index
        self.lock = threading.Lock()
        self.generation = 0        # bumped on every tune, retires old readers
        self.proc = None
        # Starts idle: nothing is decoded until the board says it is listening.
        self.last_seen = 0.0

    def heard_from_board(self, addr: str):
        with self.lock:
            was_idle = self._idle_locked()
            self.last_seen = time.monotonic()
            moved = addr != self.host
            if moved:
                old = self.host
                self.host = addr
                # Retire the current stream so packets start going to the new
                # address immediately rather than after the station changes.
                self.generation += 1
                if self.proc:
                    self.proc.kill()
                    self.proc = None
        if moved:
            print(f"[board] address {old} -> {addr}", flush=True)
        if was_idle:
            print("[idle] board is listening; resuming", flush=True)

    def _idle_locked(self) -> bool:
        return (time.monotonic() - self.last_seen) > IDLE_TIMEOUT

    def idle(self) -> bool:
        with self.lock:
            return self._idle_locked()

    def _spawn(self, index: int):
        name, url = STATIONS[index]
        print(f"[tune] {index}: {name}  {url}", flush=True)
        return subprocess.Popen(
            [
                require_ffmpeg(),
                "-loglevel", "error",
                # Reconnect rather than dying on a mid-stream hiccup; a radio
                # that stops for good on one dropped connection is useless.
                "-reconnect", "1",
                "-reconnect_streamed", "1",
                "-reconnect_delay_max", "5",
                "-i", url,
                "-vn",
                "-f", "s16le", "-acodec", "pcm_s16le",
                "-ac", "1", "-ar", str(RATE),
                "-",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )

    def tune(self, index: int, force: bool = False):
        if not 0 <= index < len(STATIONS):
            print(f"[tune] ignoring out-of-range index {index}", flush=True)
            return
        with self.lock:
            # Already on this station with a live stream: nothing to do. A dead
            # stream is the run loop's job to retry, with its own backoff - not
            # something to restart from every heartbeat.
            if index == self.index and (self.proc is not None or not force):
                return
            self.index = index
            self.generation += 1
            if self.proc:
                self.proc.kill()
                self.proc = None

    def _new_socket(self):
        return socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def run(self):
        sock = self._new_socket()
        while True:
            # Park here while nothing is listening. No ffmpeg, no packets, no
            # CPU - the process just waits for a heartbeat.
            if self.idle() or self.host is None:
                if self.host is None:
                    print("[wait] no board yet; waiting for a heartbeat",
                          flush=True)
                else:
                    print("[idle] no heartbeat; stopping stream", flush=True)
                while self.idle() or self.host is None:
                    time.sleep(1.0)

            with self.lock:
                gen = self.generation
                idx = self.index
                self.proc = self._spawn(idx)
                proc = self.proc

            # Pace packets to real time. Sending as fast as ffmpeg decodes would
            # overrun the board's jitter buffer in seconds.
            period = SAMPLES_PER_PACKET / RATE
            next_at = time.monotonic()
            sent = 0
            checked = 0
            try:
                while True:
                    # Taking the lock on every packet costs more than it is
                    # worth at 62 packets a second; a few packets of latency on
                    # a station change is imperceptible.
                    checked += 1
                    if checked % 8 == 0:
                        with self.lock:
                            if gen != self.generation:
                                break      # retired by a tune
                            if self._idle_locked():
                                break      # nobody listening any more
                    chunk = proc.stdout.read(BYTES_PER_PACKET)
                    if not chunk or len(chunk) < BYTES_PER_PACKET:
                        break              # stream ended or stalled
                    try:
                        sock.sendto(chunk, (self.host, AUDIO_PORT))
                    except OSError as e:
                        # EADDRNOTAVAIL (49) and friends happen whenever the
                        # host's network changes under us - a Wi-Fi reconnect,
                        # sleep, a new DHCP lease. The socket is stale, not the
                        # stream. Rebuild it and carry on; dying here took the
                        # radio off the air for the rest of an afternoon.
                        print(f"[net] send failed ({e}); rebuilding socket",
                              flush=True)
                        try:
                            sock.close()
                        except OSError:
                            pass
                        time.sleep(1.0)
                        sock = self._new_socket()
                        next_at = time.monotonic()
                        continue
                    sent += 1
                    if sent % 300 == 0:
                        print(f"    {sent * period:6.0f}s streamed", flush=True)
                    next_at += period
                    delay = next_at - time.monotonic()
                    if delay > 0:
                        time.sleep(delay)
                    elif delay < -MAX_LAG:
                        # Far enough behind that catching up would burst badly -
                        # give up on the backlog and resync. Resyncing on every
                        # small overrun (the old behaviour) silently capped the
                        # send rate at whatever the loop could manage, which is
                        # how 62.5 packets/s became 27.
                        print(f"[pace] {-delay:.2f}s behind; resyncing",
                              flush=True)
                        next_at = time.monotonic()
                    # Otherwise: slightly behind, so skip the sleep and let the
                    # next iterations catch up.
            finally:
                try:
                    proc.kill()
                except Exception:
                    pass

            with self.lock:
                self.proc = None
                retry = gen == self.generation and not self._idle_locked()
            if retry:
                print("[stream] ended, retrying in 2s", flush=True)
                time.sleep(2)


def control_listener(tuner: Tuner):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", CONTROL_PORT))
    print(f"[control] listening on udp/{CONTROL_PORT}", flush=True)
    while True:
        data, addr = sock.recvfrom(64)
        text = data.decode("utf-8", "replace").strip()
        parts = text.split()
        if not parts:
            continue

        # Any control traffic counts as "someone is listening".
        if parts[0] in ("TUNE", "ALIVE"):
            tuner.heard_from_board(addr[0])

        try:
            if parts[0] == "TUNE":
                tuner.tune(int(parts[1]), force=True)
            elif parts[0] == "ALIVE":
                # Corrects the station after a server restart, and otherwise
                # does nothing - see the guard in tune().
                tuner.tune(int(parts[1]))
            else:
                print(f"[control] unknown verb {text!r} from {addr}", flush=True)
        except (IndexError, ValueError):
            print(f"[control] malformed request {text!r} from {addr}", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("host", nargs="?", default=None,
                    help="board IP (optional; learned from its heartbeat)")
    ap.add_argument("--station", type=int, default=0, help="initial station index")
    args = ap.parse_args()

    print("Stations:")
    for i, (name, _) in enumerate(STATIONS):
        print(f"  {i}  {name}")

    tuner = Tuner(args.host, args.station)
    threading.Thread(target=control_listener, args=(tuner,), daemon=True).start()

    # Nothing below the UI should be able to take the radio off the air. Any
    # unexpected error restarts the streaming loop rather than exiting.
    while True:
        try:
            tuner.run()
        except KeyboardInterrupt:
            print("\nstopped")
            return
        except Exception as e:  # noqa: BLE001 - deliberately broad
            print(f"[fatal] {type(e).__name__}: {e}; restarting in 3s",
                  flush=True)
            time.sleep(3)


if __name__ == "__main__":
    main()
