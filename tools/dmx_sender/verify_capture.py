#!/usr/bin/env python3
"""Capture Art-Net / sACN traffic and check it against the published packet layouts.

The decoders here are written from the specs' field tables and share no code
with dmx_sender.py, so a mistake in the sender's packet builder cannot hide
behind the same mistake in the checker.

  Art-Net 4, protocol release V1.4: "ArtDmx packet definition"
  ANSI E1.31-2018: E1.31 data packet (root, framing and DMP layers)

Run it in one terminal, then start the sender (or a console) in another:

  verify_capture.py --seconds 5 --expect-rate 44
"""
from __future__ import annotations

import argparse
import selectors
import socket
import statistics
import struct
import sys
import time
import uuid
from collections import defaultdict, namedtuple

ARTNET_PORT = 6454
SACN_PORT = 5568

# t is a perf_counter timestamp taken when the packet was read; src/dst are (ip, port).
Packet = namedtuple("Packet", "t proto src dst data")


def check_artdmx(data):
    """Check one datagram against the ArtDmx definition. Returns (fields, problems)."""
    if len(data) < 18:
        return None, [f"{len(data)} bytes is shorter than the 18-byte ArtDmx header"]
    problems = []
    if bytes(data[0:8]) != b"Art-Net\x00":
        problems.append("ID is not 'Art-Net' followed by a null")
    opcode = data[8] | data[9] << 8                  # transmitted low byte first
    if opcode != 0x5000:
        problems.append(f"OpCode is 0x{opcode:04x}, expected OpDmx 0x5000")
    version = data[10] << 8 | data[11]               # high byte first
    if version != 14:
        problems.append(f"protocol revision is {version}, expected 14")
    sequence, physical, sub_uni, net = data[12], data[13], data[14], data[15]
    if net & 0x80:
        problems.append("Net has bit 7 set; the Port-Address is 15 bits")
    length = data[16] << 8 | data[17]                # high byte first
    if not 2 <= length <= 512:
        problems.append(f"Length {length} is outside 2-512")
    if length % 2:
        problems.append(f"Length {length} is odd")
    if len(data) != 18 + length:
        problems.append(f"Length says {length} slots but the datagram carries {len(data) - 18}")
    return {"sequence": sequence, "physical": physical, "universe": net << 8 | sub_uni,
            "length": length, "dmx": bytes(data[18:18 + length])}, problems


def check_e131_data(data):
    """Check one datagram against the E1.31 data packet layout. Returns (fields, problems)."""
    size = len(data)
    if size < 126:
        return None, [f"{size} bytes is shorter than the 126 octets before the first slot"]
    problems = []

    def u16(offset):
        return data[offset] << 8 | data[offset + 1]

    def u32(offset):
        return int.from_bytes(data[offset:offset + 4], "big")

    # Root layer, octets 0-37
    if u16(0) != 0x0010:
        problems.append(f"preamble size is 0x{u16(0):04x}, expected 0x0010")
    if u16(2) != 0x0000:
        problems.append(f"post-amble size is 0x{u16(2):04x}, expected 0x0000")
    if bytes(data[4:16]) != b"ASC-E1.17\x00\x00\x00":
        problems.append("ACN packet identifier is not 'ASC-E1.17' padded with three nulls")
    # Each PDU's low 12 bits count octets from its own flags-and-length field to the end.
    for layer, offset in (("root", 16), ("framing", 38), ("DMP", 115)):
        flags_length = u16(offset)
        if flags_length >> 12 != 0x7:
            problems.append(f"{layer} layer flags are 0x{flags_length >> 12:x}, expected 0x7")
        if flags_length & 0x0FFF != size - offset:
            problems.append(f"{layer} layer length is {flags_length & 0x0FFF}, "
                            f"but {size - offset} octets follow from octet {offset}")
    if u32(18) != 0x00000004:
        problems.append(f"root vector is 0x{u32(18):08x}, expected VECTOR_ROOT_E131_DATA 0x00000004")
    cid = bytes(data[22:38])
    if cid == bytes(16):
        problems.append("CID is all zero")
    elif uuid.UUID(bytes=cid).variant != uuid.RFC_4122:
        problems.append("CID is not an RFC 4122 UUID")

    # Framing layer, octets 38-114
    if u32(40) != 0x00000002:
        problems.append(f"framing vector is 0x{u32(40):08x}, expected VECTOR_E131_DATA_PACKET 0x00000002")
    raw_name = bytes(data[44:108])
    name, _, padding = raw_name.partition(b"\x00")
    if len(name) == 64:
        problems.append("source name is not null-terminated")
    elif padding.strip(b"\x00"):
        problems.append("source name is not padded with nulls")
    try:
        source_name = name.decode("utf-8")
    except UnicodeDecodeError:
        source_name = repr(name)
        problems.append("source name is not UTF-8")
    priority = data[108]
    if priority > 200:
        problems.append(f"priority {priority} is above 200")
    sequence, options = data[111], data[112]
    if options & 0x1F:
        problems.append(f"options 0x{options:02x} sets reserved bits 0-4")
    universe = u16(113)
    if not 1 <= universe <= 63999:
        problems.append(f"universe {universe} is outside 1-63999")

    # DMP layer, octets 115 onwards
    if data[117] != 0x02:
        problems.append(f"DMP vector is 0x{data[117]:02x}, expected VECTOR_DMP_SET_PROPERTY 0x02")
    if data[118] != 0xA1:
        problems.append(f"address type & data type is 0x{data[118]:02x}, expected 0xa1")
    if u16(119) != 0x0000:
        problems.append(f"first property address is 0x{u16(119):04x}, expected 0x0000")
    if u16(121) != 0x0001:
        problems.append(f"address increment is 0x{u16(121):04x}, expected 0x0001")
    count = u16(123)
    if not 1 <= count <= 513:
        problems.append(f"property value count {count} is outside 1-513")
    if count != size - 125:
        problems.append(f"property value count is {count}, but {size - 125} octets follow")
    if data[125] != 0x00:
        problems.append(f"START code is 0x{data[125]:02x}, expected 0x00 for dimmer data")
    return {"cid": cid, "source_name": source_name, "priority": priority,
            "sync_address": u16(109), "sequence": sequence, "options": options,
            "preview": bool(options & 0x80), "terminated": bool(options & 0x40),
            "universe": universe, "dmx": bytes(data[126:])}, problems


CHECKERS = {"artnet": check_artdmx, "sacn": check_e131_data}


def open_receiver(port, bind="127.0.0.1", multicast_groups=(), interface="127.0.0.1"):
    """A UDP socket for capture. Port 0 picks a free port; read it back with getsockname()."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, "SO_REUSEPORT"):
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
    except OSError:
        pass
    try:
        # Multicast is only delivered to a socket bound to the wildcard or the group address.
        sock.bind(("" if multicast_groups else bind, port))
        for group in multicast_groups:
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                            socket.inet_aton(group) + socket.inet_aton(interface))
    except OSError:
        sock.close()
        raise
    sock.setblocking(False)
    return sock


def capture(sockets, stop, idle=0.3):
    """Read from {socket: protocol} until stop() is true and nothing has arrived for `idle` seconds."""
    selector = selectors.DefaultSelector()
    for sock, proto in sockets.items():
        selector.register(sock, selectors.EVENT_READ, (proto, sock.getsockname()))
    packets = []
    last = time.perf_counter()
    while True:
        events = selector.select(timeout=0.05)
        now = time.perf_counter()
        for key, _ in events:
            proto, dst = key.data
            while True:
                try:
                    data, src = key.fileobj.recvfrom(2048)
                except BlockingIOError:
                    break
                packets.append(Packet(time.perf_counter(), proto, src, dst, data))
                last = now
        if stop() and now - last > idle:
            break
    selector.close()
    return packets


def analyse(packets, expect_rate=None, rate_tolerance=0.5, expect_ramp=False):
    """Decode every packet and check each universe's stream.

    Returns {"streams": {(proto, universe): stats}, "problems": [...], "undecodable": n}.
    """
    problems = []
    undecodable = 0
    streams = defaultdict(list)
    for index, packet in enumerate(packets):
        fields, found = CHECKERS[packet.proto](packet.data)
        problems += [f"packet {index} ({packet.proto}): {text}" for text in found]
        if fields is None:
            undecodable += 1
            continue
        streams[(packet.proto, fields["universe"])].append((packet.t, fields))

    report = {}
    for (proto, universe), entries in sorted(streams.items()):
        where = f"{proto} universe {universe}"
        live = [(t, f) for t, f in entries if not f.get("terminated")]
        terminated = len(entries) - len(live)
        if proto == "sacn" and any(f["terminated"] for _, f in entries[:len(live)]):
            problems.append(f"{where}: data follows a Stream_Terminated packet")

        sequence_errors = 0
        for (_, a), (_, b) in zip(entries, entries[1:]):
            if proto == "artnet":
                expected = a["sequence"] % 255 + 1      # 1-255, 0 is skipped
            else:
                expected = (a["sequence"] + 1) & 0xFF
            sequence_errors += b["sequence"] != expected
        if proto == "artnet" and any(f["sequence"] == 0 for _, f in entries):
            problems.append(f"{where}: Sequence 0 seen, which disables sequencing")
        if sequence_errors:
            problems.append(f"{where}: {sequence_errors} sequence break(s)")

        rate = None
        intervals = [b[0] - a[0] for a, b in zip(live, live[1:])]
        if intervals:
            rate = len(intervals) / (live[-1][0] - live[0][0])
            if expect_rate is not None and abs(rate - expect_rate) > rate_tolerance:
                problems.append(f"{where}: {rate:.2f} Hz, expected {expect_rate:g} +/- {rate_tolerance:g}")

        if expect_ramp:
            # dmx_sender's ramp pattern: slot s of universe u on frame f is (s + u + f) mod 256.
            previous = None
            mismatched = out_of_step = 0
            for _, f in live:
                dmx = f["dmx"]
                frame = (dmx[0] - universe) & 0xFF
                mismatched += any(value != (slot + universe + frame) & 0xFF
                                  for slot, value in enumerate(dmx))
                out_of_step += previous is not None and frame != (previous + 1) & 0xFF
                previous = frame
            if mismatched:
                problems.append(f"{where}: {mismatched} packet(s) do not carry the ramp pattern")
            if out_of_step:
                problems.append(f"{where}: ramp frame counter skipped or repeated {out_of_step} time(s)")

        report[(proto, universe)] = {
            "packets": len(entries), "live": len(live), "terminated": terminated,
            "sequence_errors": sequence_errors, "rate": rate,
            "interval_ms": ([min(intervals) * 1000, statistics.median(intervals) * 1000,
                             max(intervals) * 1000] if intervals else None),
            "slots": sorted({len(f["dmx"]) for _, f in entries}),
            "payloads": [f["dmx"] for _, f in live],
        }

    # Where a universe arrived on both protocols over the same run, the DMX must match frame for frame.
    compared = 0
    for (proto, universe), stats in report.items():
        other = report.get(("sacn", universe))
        if proto != "artnet" or other is None or other["live"] != stats["live"]:
            continue
        compared += 1
        differing = sum(a != b for a, b in zip(stats["payloads"], other["payloads"]))
        if differing:
            problems.append(f"universe {universe}: {differing} frame(s) differ between Art-Net and sACN")
    return {"streams": report, "problems": problems, "undecodable": undecodable,
            "cross_checked_universes": compared}


def write_pcap(path, packets, wall_start=None):
    """Write the capture as a pcap (raw IPv4 link type) that Wireshark opens directly."""
    wall_start = time.time() if wall_start is None else wall_start
    t0 = packets[0].t if packets else 0.0
    with open(path, "wb") as out:
        out.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 101))
        for packet in packets:
            udp_length = 8 + len(packet.data)
            src = socket.inet_aton(packet.src[0])
            dst = socket.inet_aton("127.0.0.1" if packet.dst[0] in ("", "0.0.0.0") else packet.dst[0])
            ip = struct.pack(">BBHHHBBH4s4s", 0x45, 0, 20 + udp_length, 0, 0, 64, 17, 0, src, dst)
            total = sum(struct.unpack(">10H", ip))
            total = (total & 0xFFFF) + (total >> 16)
            total = (total & 0xFFFF) + (total >> 16)
            ip = ip[:10] + struct.pack(">H", ~total & 0xFFFF) + ip[12:]
            udp = struct.pack(">HHHH", packet.src[1], packet.dst[1], udp_length, 0)
            record = ip + udp + packet.data
            stamp = wall_start + (packet.t - t0)
            out.write(struct.pack("<IIII", int(stamp), int(stamp % 1 * 1_000_000),
                                  len(record), len(record)) + record)


def print_report(result, out=sys.stdout):
    streams = result["streams"]
    if not streams:
        print("no Art-Net or sACN packets captured", file=out)
    for (proto, universe), s in streams.items():
        rate = f"{s['rate']:.2f} Hz" if s["rate"] else "n/a"
        interval = ("interval min/median/max {:.2f}/{:.2f}/{:.2f} ms".format(*s["interval_ms"])
                    if s["interval_ms"] else "")
        ended = f", {s['terminated']} terminate" if s["terminated"] else ""
        print(f"{proto:6} universe {universe:5}  {s['packets']:6} packets{ended}  {rate:>9}  "
              f"{interval}  slots {s['slots']}  sequence breaks {s['sequence_errors']}", file=out)
    print(f"{result['cross_checked_universes']} universe(s) compared between Art-Net and sACN", file=out)
    problems = result["problems"]
    for text in problems[:40]:
        print("PROBLEM:", text, file=out)
    if len(problems) > 40:
        print(f"... and {len(problems) - 40} more", file=out)
    total = sum(s["packets"] for s in streams.values()) + result["undecodable"]
    print(f"{total} packets checked, {len(problems)} problem(s)", file=out)


def _universe_list(text):
    universes = []
    for part in filter(None, text.split(",")):
        first, _, last = part.partition("-")
        universes += range(int(first), int(last or first) + 1)
    return universes


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--seconds", type=float, default=5.0, help="how long to capture (default 5)")
    p.add_argument("--bind", default="127.0.0.1",
                   help="local IP to listen on (default 127.0.0.1; 0.0.0.0 also hears the network)")
    p.add_argument("--artnet-port", type=int, default=ARTNET_PORT)
    p.add_argument("--sacn-port", type=int, default=SACN_PORT)
    p.add_argument("--sacn-multicast", metavar="UNIVERSES",
                   help="join the sACN multicast groups for these universes, e.g. 1-20")
    p.add_argument("--interface", default="127.0.0.1", help="interface IP for the multicast joins")
    p.add_argument("--expect-rate", type=float, help="fail if a universe's frame rate differs from this")
    p.add_argument("--rate-tolerance", type=float, default=0.5, help="allowed deviation in Hz (default 0.5)")
    p.add_argument("--expect-ramp", action="store_true", help="check payloads against dmx_sender's ramp pattern")
    p.add_argument("--pcap", help="also write the capture to this file for Wireshark")
    args = p.parse_args(argv)

    groups = [f"239.255.{u >> 8}.{u & 0xFF}" for u in _universe_list(args.sacn_multicast or "")]
    sockets = {open_receiver(args.artnet_port, args.bind): "artnet",
               open_receiver(args.sacn_port, args.bind, groups, args.interface): "sacn"}
    print(f"capturing for {args.seconds:g} s on {args.bind}: Art-Net port {args.artnet_port}, "
          f"sACN port {args.sacn_port}", file=sys.stderr)
    wall_start = time.time()
    end = time.perf_counter() + args.seconds
    packets = capture(sockets, lambda: time.perf_counter() >= end, idle=0.0)
    if args.pcap:
        write_pcap(args.pcap, packets, wall_start)
    result = analyse(packets, args.expect_rate, args.rate_tolerance, args.expect_ramp)
    print_report(result)
    return 1 if result["problems"] or not result["streams"] else 0


if __name__ == "__main__":
    sys.exit(main())
