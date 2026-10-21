#!/usr/bin/env python3
"""PosiStageNet for Openviz: reading what a tracking system sends, and a sender to try it with.

PosiStageNet (PSN 2.03, VYV and MA Lighting, open and free of royalties) tells consoles and
visualizers where tracked things are: performers wearing a beacon, a moving set piece. It comes
by UDP to the multicast group 236.10.10.10, port 56565, unless the tracking system is set
otherwise. Everything in a packet is a chunk: a 32-bit header (little-endian: the id in the low
16 bits, the length of what follows in the next 15, the top bit set when what follows is more
chunks) and its data. A data packet (0x6755) holds a packet header and the trackers, each a
chunk whose id is the tracker's number, holding its position and whatever else the system knows;
an info packet (0x6756), about once a second, holds the system's name and each tracker's name.
Chunks of a kind not known are passed over, as the protocol asks.

This is the same reading as the core's vizcore/psn.h, which the visualizer uses: keep the two
alike. Positions are metres, x to the right, y up, z into depth; seen from the house, that is
the rig file's x, z and y (`to_mvr`).

  python3 launcher/psn.py                     # two trackers walking circles, to 236.10.10.10:56565
  python3 launcher/psn.py --to 127.0.0.1 --port 56565 --trackers 4 --radius 3 --seconds 60
"""
from __future__ import annotations

import argparse
import math
import socket
import struct
import sys
import time

GROUP = "236.10.10.10"
PORT = 56565
VISUALIZER_PORT = 56566    # where the visualizer hears, on 127.0.0.1, what Openviz's receiver passes on (dmx_relay.py)
DATA, INFO = 0x6755, 0x6756
FIELDS = {0: "position", 1: "speed", 2: "orientation", 4: "acceleration", 5: "target"}    # three floats each
STATUS, TIMESTAMP = 3, 6


def _chunks(data):
    """(id, holds chunks, data) for each chunk in `data`; one that claims more than is there is
    read as far as it goes and is the last."""
    at = 0
    while at + 4 <= len(data):
        head = struct.unpack_from("<I", data, at)[0]
        size = (head >> 16) & 0x7FFF
        body = data[at + 4:at + 4 + size]
        yield head & 0xFFFF, bool(head >> 31), body
        if len(body) < size:
            return
        at += 4 + size


def parse(data):
    """A datagram as {"kind": "data" or "info", "timestamp", "frame", "frame_packets", "system",
    "trackers": {id: {...}}}, or None if it is not PSN. A data packet's trackers carry what the
    system sent of "position", "speed", "orientation", "acceleration", "target" (three floats
    each), "validity" and "timestamp"; an info packet's carry "name"."""
    if len(data) < 4:
        return None
    head = struct.unpack_from("<I", data)[0]
    kind = {DATA: "data", INFO: "info"}.get(head & 0xFFFF)
    if not kind or not head >> 31:
        return None
    packet = {"kind": kind, "system": "", "trackers": {}}
    header = False
    for chunk, more, body in _chunks(bytes(data[4:4 + ((head >> 16) & 0x7FFF)])):
        if chunk == 0 and not more and len(body) >= 12:
            packet["timestamp"], _, _, packet["frame"], packet["frame_packets"] = struct.unpack_from("<QBBBB", body)
            header = True
        elif kind == "info" and chunk == 1 and not more:
            packet["system"] = body.decode("utf-8", "replace")
        elif more and chunk == (2 if kind == "info" else 1):
            for number, holds, entry in _chunks(body):
                if not holds:
                    continue
                tracker = packet["trackers"].setdefault(number, {})
                for field, plain, value in _chunks(entry):
                    if plain:
                        continue
                    if kind == "info":
                        if field == 0:
                            tracker["name"] = value.decode("utf-8", "replace")
                    elif field in FIELDS and len(value) >= 12:
                        tracker[FIELDS[field]] = struct.unpack_from("<fff", value)
                    elif field == STATUS and len(value) >= 4:
                        tracker["validity"] = struct.unpack_from("<f", value)[0]
                    elif field == TIMESTAMP and len(value) >= 8:
                        tracker["timestamp"] = struct.unpack_from("<Q", value)[0]
    return packet if header else None


def _chunk(chunk, body, more=False):
    return struct.pack("<I", (chunk & 0xFFFF) | (len(body) & 0x7FFF) << 16 | (0x80000000 if more else 0)) + body


def _header(timestamp, frame):
    return _chunk(0, struct.pack("<QBBBB", timestamp, 2, 3, frame & 0xFF, 1))


def data_packet(trackers, timestamp, frame):
    """One data packet, as a tracking system sends it: `trackers` {id: {"position": (x, y, z), ...}}
    with any of the fields parse() gives. Not split: keep it under about 30 trackers."""
    entries = b""
    for number, tracker in sorted(trackers.items()):
        fields = b""
        for field, key in sorted(FIELDS.items()):
            if key in tracker:
                fields += _chunk(field, struct.pack("<fff", *tracker[key]))
            if field == 2 and "validity" in tracker:
                fields += _chunk(STATUS, struct.pack("<f", tracker["validity"]))
        if "timestamp" in tracker:
            fields += _chunk(TIMESTAMP, struct.pack("<Q", tracker["timestamp"]))
        entries += _chunk(number, fields, more=True)
    return _chunk(DATA, _header(timestamp, frame) + _chunk(1, entries, more=True), more=True)


def info_packet(system, names, timestamp, frame):
    """One info packet: the system's name and `names` {id: name}."""
    entries = b"".join(_chunk(number, _chunk(0, name.encode("utf-8")), more=True) for number, name in sorted(names.items()))
    return _chunk(INFO, _header(timestamp, frame) + _chunk(1, system.encode("utf-8")) + _chunk(2, entries, more=True), more=True)


def to_mvr(position):
    """A PSN position (metres: right, up, depth) in the rig file's terms: millimetres, x to stage
    left (the house's right), y from the house toward upstage, z up."""
    x, y, z = position
    return x * 1000.0, z * 1000.0, y * 1000.0


def walking(count, radius, seconds):
    """Where `count` trackers walking circles are after `seconds`: {id: {"position", "speed"}}, ids
    from 1, each on a circle of its own about the middle of the stage, a metre a second."""
    trackers = {}
    for k in range(count):
        r = radius * (k + 1) / count
        angle = seconds / max(r, 0.1) + k * 2 * math.pi / max(count, 1)
        trackers[k + 1] = {"position": (r * math.cos(angle), 0.0, r * math.sin(angle) + radius),
                           "speed": (-math.sin(angle), 0.0, math.cos(angle)), "validity": 1.0}
    return trackers


def main(argv=None):
    p = argparse.ArgumentParser(description="Sends PosiStageNet: trackers walking circles, for trying a receiver.")
    p.add_argument("--to", default=GROUP, help=f"where to send (default the PSN group {GROUP})")
    p.add_argument("--port", type=int, default=PORT, help=f"(default {PORT})")
    p.add_argument("--interface", default="", help="this computer's address to send multicast from (default the system's choice)")
    p.add_argument("--trackers", type=int, default=2, help="how many (default 2)")
    p.add_argument("--radius", type=float, default=3.0, help="of the largest circle, metres (default 3)")
    p.add_argument("--rate", type=float, default=60.0, help="data packets a second (default 60)")
    p.add_argument("--seconds", type=float, default=0.0, help="stop after this long (default: until stopped)")
    args = p.parse_args(argv)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
    if args.interface:
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(args.interface))
    names = {k + 1: f"Walker {k + 1}" for k in range(max(1, min(args.trackers, 30)))}
    started = time.monotonic()
    frame = 0
    last_info = -1.0
    print(f"sending {len(names)} tracker{'s' if len(names) != 1 else ''} to {args.to}:{args.port}; Control-C stops", flush=True)
    try:
        while not args.seconds or time.monotonic() - started < args.seconds:
            now = time.monotonic() - started
            stamp = int(now * 1e6)
            frame = (frame + 1) % 256
            if now - last_info >= 1.0:
                sock.sendto(info_packet("Openviz test sender", names, stamp, frame), (args.to, args.port))
                last_info = now
            sock.sendto(data_packet(walking(len(names), args.radius, now), stamp, frame), (args.to, args.port))
            time.sleep(1.0 / max(args.rate, 1.0))
    except KeyboardInterrupt:
        pass
    finally:
        sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
