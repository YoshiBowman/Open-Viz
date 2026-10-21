#!/usr/bin/env python3
"""Records the DMX that arrives for the visualizer, and plays a recording back to it.

A recording holds levels, not packets: for each universe, its levels whenever they changed, with
the time. So a held look costs nothing, a recording made from sACN plays back the same as one made
from Art-Net, and playing back is simply being a console that sends those levels again. Playback
goes to the visualizer as Art-Net.

The file: the line "OPENVIZ-DMX 1", then one record after another, each a time in seconds from the
start (8-byte float), a universe number and a count of levels (two bytes each, all little-endian),
and that many levels.

  dmx_record.py info take.dmxrec
  dmx_record.py play take.dmxrec --to 127.0.0.1
"""
from __future__ import annotations

import argparse
import os
import select
import socket
import struct
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "tools", "dmx_sender"))

sys.path.insert(0, HERE)

from dmx_packets import ARTNET_PORT, artdmx, parse_artdmx, parse_sacn, whole  # noqa: E402

MAGIC = b"OPENVIZ-DMX 1\n"
RECORD = struct.Struct("<dHH")
KEEP_ALIVE = 1.0        # a universe that does not change is still written, and sent, this often
LONGEST = 4 * 3600.0    # a recording stops by itself after this long


def levels_of(protocol, data):
    """(universe, levels) if a datagram carries DMX levels, else None. See dmx_packets for how leniently."""
    if protocol == "sacn":
        parsed = parse_sacn(data)
        if not parsed or parsed[4]:    # not levels, or the source saying goodbye
            return None
        return parsed[0], bytes(parsed[3])
    return parse_artdmx(data)


class Writer:
    """A recording being written: give it each universe's levels as they arrive, with the time.

    Levels that have not changed are left out, but for one record every KEEP_ALIVE seconds. The
    file appears under its own name only when close() has finished it.
    """

    def __init__(self, path):
        self.path = path
        self.frames = 0          # records written
        self.universes = set()
        self.last_time = 0.0
        self._last = {}          # universe -> (levels, time written)
        self._out = open(path + ".part", "wb")
        self._out.write(MAGIC)

    def write(self, now, universe, levels, force=False):
        if not 0 < universe <= 0x7FFF or self._out is None:
            return
        last = self._last.get(universe)
        if last and last[0] == levels and now - last[1] < KEEP_ALIVE and not force:
            return    # nothing new on this universe
        self._out.write(RECORD.pack(now, universe, len(levels)) + levels)
        self._last[universe] = (levels, now)
        self.universes.add(universe)
        self.frames += 1
        self.last_time = now

    def close(self):
        if self._out is None:
            return
        self._out.close()
        self._out = None
        os.replace(self.path + ".part", self.path)


class Recorder(threading.Thread):
    """Writes what arrives on `sock` to `path` until stop() or `limit` seconds have passed."""

    def __init__(self, sock, protocol, path, limit=LONGEST):
        super().__init__(daemon=True)
        self.sock = sock
        self.protocol = protocol
        self.path = path
        self.limit = min(float(limit), LONGEST)
        self.started = time.monotonic()
        self.frames = 0
        self.universes = set()
        self.error = None
        self._halt = threading.Event()

    def seconds(self):
        return (self._ended if hasattr(self, "_ended") else time.monotonic()) - self.started

    def stop(self):
        self._halt.set()
        self.join(timeout=5)

    def run(self):
        try:
            writer = Writer(self.path)
            self.universes = writer.universes
            while not self._halt.is_set() and time.monotonic() - self.started < self.limit:
                if not select.select([self.sock], [], [], 0.1)[0]:
                    continue
                while True:
                    try:
                        data = self.sock.recvfrom(2048)[0]
                    except BlockingIOError:
                        break
                    found = levels_of(self.protocol, data)
                    if found:
                        writer.write(time.monotonic() - self.started, found[0], found[1])
                        self.frames = writer.frames
            writer.close()
        except OSError as error:
            self.error = str(error)
        finally:
            self._ended = time.monotonic()
            self.sock.close()


def read(path):
    """Yields (seconds, universe, levels) for every record of a recording. Stops at the first damaged one."""
    with open(path, "rb") as handle:
        if handle.read(len(MAGIC)) != MAGIC:
            raise ValueError("not an Openviz DMX recording")
        while True:
            head = handle.read(RECORD.size)
            if len(head) < RECORD.size:
                return
            at, universe, count = RECORD.unpack(head)
            levels = handle.read(count)
            if len(levels) < count or count > 512 or not at >= 0.0:
                return
            yield at, universe, levels


def trim(source, target, start, end):
    """Writes the part of a recording from `start` to `end` seconds as a recording of its own,
    beginning with every universe as it stood at `start`. Returns its summary."""
    if not 0.0 <= start < end:
        raise ValueError("the part must start before it ends, at 0 seconds or later")
    standing = {}    # universe -> its levels at the latest record before `start`
    writer = Writer(target)
    begun = False
    try:
        for at, universe, levels in read(source):
            if at < start:
                standing[universe] = levels
                continue
            if at > end:
                break
            if not begun:
                for held, held_levels in sorted(standing.items()):
                    writer.write(0.0, held, held_levels)
                begun = True
            writer.write(at - start, universe, levels)
        if not begun:
            for held, held_levels in sorted(standing.items()):
                writer.write(0.0, held, held_levels)
        # The last look is held to the end of the part, so that the part lasts as long as asked.
        for held, (held_levels, written) in sorted(writer._last.items()):
            if written < end - start:
                writer.write(end - start, held, held_levels, force=True)
    finally:
        writer.close()
    return summary(target)


def summary(path):
    """How long a recording is, and what is in it."""
    seconds, frames, universes = 0.0, 0, set()
    for at, universe, _ in read(path):
        seconds = max(seconds, at)
        frames += 1
        universes.add(universe)
    return {"seconds": round(seconds, 2), "frames": frames, "universes": len(universes),
            "highest_universe": max(universes) if universes else 0}


class Player(threading.Thread):
    """Sends a recording to `to` as Art-Net, in its own time, again and again if `loop`."""

    def __init__(self, path, to=("127.0.0.1", ARTNET_PORT), loop=True):
        super().__init__(daemon=True)
        self.records = list(read(path))
        self.length = self.records[-1][0] if self.records else 0.0
        self.to = to
        self.loop = loop
        self.position = 0.0      # seconds into the recording
        self.rounds = 0          # times it has been played through
        self.sent = 0
        self._halt = threading.Event()

    def stop(self):
        self._halt.set()
        self.join(timeout=5)

    def run(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sequence = {}
        state = {}       # universe -> levels as last sent
        refreshed = {}   # universe -> when

        def send(universe, levels, now):
            sequence[universe] = sequence.get(universe, 0) % 255 + 1
            try:
                sock.sendto(artdmx(universe, sequence[universe], whole(levels)), self.to)    # whole: Unreal reads nothing less
            except OSError:
                return
            state[universe] = levels
            refreshed[universe] = now
            self.sent += 1

        try:
            while not self._halt.is_set() and self.records:
                started = time.monotonic()
                index = 0
                while not self._halt.is_set():
                    now = time.monotonic()
                    self.position = now - started
                    while index < len(self.records) and self.records[index][0] <= self.position:
                        _, universe, levels = self.records[index]
                        send(universe, levels, now)
                        index += 1
                    # What holds still is said again now and then, as a console does.
                    for universe, levels in list(state.items()):
                        if now - refreshed[universe] >= KEEP_ALIVE:
                            send(universe, levels, now)
                    if index >= len(self.records):
                        break
                    self._halt.wait(min(max(self.records[index][0] - self.position, 0.0), 0.02))
                self.rounds += 1
                if not self.loop:
                    break
                # Hold the last look for a moment before starting over, so a short recording does not flicker.
                self._halt.wait(0.25)
        finally:
            sock.close()


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="command", required=True)
    info = sub.add_parser("info", help="say what is in a recording")
    info.add_argument("file")
    play = sub.add_parser("play", help="send a recording as Art-Net")
    play.add_argument("file")
    play.add_argument("--to", default="127.0.0.1", help="where the visualizer listens (default 127.0.0.1)")
    play.add_argument("--once", action="store_true", help="play it through once instead of round and round")
    args = p.parse_args(argv)
    try:
        facts = summary(args.file)
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(f"{facts['seconds']:.1f} s, {facts['universes']} universes (highest {facts['highest_universe']}), {facts['frames']} changes")
    if args.command == "play":
        player = Player(args.file, (args.to, ARTNET_PORT), loop=not args.once)
        player.start()
        try:
            while player.is_alive():
                player.join(0.2)
        except KeyboardInterrupt:
            player.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
