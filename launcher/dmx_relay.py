#!/usr/bin/env python3
"""Receives the console's DMX for Openviz and hands it to Unreal on this computer, as Art-Net.

Everything the visualizer is sent goes through here, so that Openviz can show what is arriving,
record it, and put right what Unreal's own receivers get wrong.

sACN: it is sent to multicast groups, and a receiver has to join each group on the network
interface the data arrives at. Unreal's sACN receiver joins on whichever interface the operating
system treats as its default; on a computer with one network for lighting and one for everything
else that is usually the wrong one, and Unreal hears nothing. (Seen on macOS with Unreal 5.8. A
helper holding the groups on the right interface does not help: macOS only delivers a group's
traffic to the socket that joined it.) So the groups are joined here, on the interface chosen
under DMX. Where two sources send the same universe, the higher sACN priority has the say, and
sources of equal priority are merged slot by slot, the higher level winning ("HTP").

Art-Net: it arrives as broadcast or addressed to this computer's network address. It is received
here on every address and passed to Unreal, which listens on this computer's own address
(127.0.0.1) only: packets sent to that address go straight to Unreal and are not seen here.

Either way Unreal receives Art-Net through its own DMX plugin, with the universe numbers as sent.

  dmx_relay.py --protocol sacn --interface 192.168.1.45 --universes 116 --while-pid 4242

--monitor FILE   twice a second, write what is arriving to FILE as JSON (see Relay.snapshot)
--control FILE   read orders from FILE, a JSON object, whenever it changes:
                   "while_pid": stop when that process is gone (and the --parent-pid one is too)
                   "watch":     a universe whose levels are wanted in the monitor file
                   "record":    a file to record into (see dmx_record.py); null or absent stops it
"""
from __future__ import annotations

import argparse
import json
import os
import select
import signal
import socket
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import oslayer  # noqa: E402

import dmx_record  # noqa: E402
from dmx_packets import ARTNET_PORT, SACN_PORT, artdmx, parse_artdmx, parse_sacn, whole  # noqa: E402

SOURCE_TIMEOUT = 2.5    # E1.31's network data loss timeout: a source silent this long is gone


class Relay:
    def __init__(self, interface, universes, listen_port=None, to=("127.0.0.1", ARTNET_PORT), protocol="sacn"):
        self.protocol = protocol
        self.interface = interface
        self.to = to
        self.sockets = []
        self.sources = {}      # sACN: universe -> {source id: (priority, last heard, levels)}
        self.sequence = {}     # universe -> last Art-Net sequence number, 1-255
        self.passed = 0
        self.other = 0         # datagrams that were not levels: polls, sync, discovery
        self.seen = {}         # universe -> what the monitor shows of it (see _note)
        self.recording = None  # a dmx_record.Writer while recording
        self.record_started = 0.0
        self.record_error = ""
        self.out = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.out.bind(("127.0.0.1", 0))    # known, so that what is sent from it can be told apart if it comes back
        if listen_port is None:    # 0 is a port too: any free one, as the tests ask for
            listen_port = SACN_PORT if protocol == "sacn" else ARTNET_PORT
        try:
            if protocol == "sacn":
                sock = None
                for universe in range(1, universes + 1):
                    if (universe - 1) % 20 == 0:    # some systems allow only 20 groups to a socket
                        sock = self._listener(listen_port)
                    group = f"239.255.{universe >> 8}.{universe & 255}"
                    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, socket.inet_aton(group) + socket.inet_aton(interface))
            else:
                sock = self._listener(listen_port)
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        except OSError:
            self.close()
            raise

    def _listener(self, port):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sockets.append(sock)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if hasattr(socket, "SO_REUSEPORT"):
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
        except OSError:
            pass
        # The wildcard address: multicast and broadcast are not delivered to a socket bound to one interface's.
        sock.bind(("", port))
        sock.setblocking(False)
        return sock

    def close(self):
        self.record(None)
        for sock in self.sockets + [self.out]:
            sock.close()
        self.sockets = []

    # ------------------------------------------------------------------------------- passing on

    def handle(self, data, now, sender=("", 0)):
        """Passes one datagram on if it carries levels (for sACN: from the source that currently has the say)."""
        if self.protocol == "sacn":
            parsed = parse_sacn(data)
            if not parsed:
                self.other += 1
                return False
            universe, priority, source, levels, terminated, name = parsed
            if universe > 0x7FFF:
                return False    # no Art-Net Port-Address that high
            heard = self.sources.setdefault(universe, {})
            if terminated:
                heard.pop(source, None)    # the source says it is stopping
                return False
            heard[source] = (priority, now, levels)
            for other in [key for key, (_, last, _) in heard.items() if now - last > SOURCE_TIMEOUT]:
                del heard[other]
            top = max(entry[0] for entry in heard.values())
            self._note(universe, now, sender[0], name, priority, None)
            if priority < top:
                return False
            # Every source at the top priority has a say: slot by slot, the highest level.
            rivals = [entry[2] for key, entry in heard.items() if entry[0] == top and key != source]
            for other in rivals:
                if len(other) > len(levels):
                    levels = levels + bytes(len(other) - len(levels))
                levels = bytes(map(max, levels, other + bytes(len(levels) - len(other))))
        else:
            if sender[0] == "127.0.0.1" and sender[1] == self.out.getsockname()[1]:
                return False    # our own, come back: never pass it round again
            parsed = parse_artdmx(data)
            if not parsed:
                self.other += 1
                return False
            universe, levels = parsed
            self._note(universe, now, sender[0], "", None, None)
        sequence = self.sequence.get(universe, 0) % 255 + 1
        self.sequence[universe] = sequence
        try:
            self.out.sendto(artdmx(universe, sequence, whole(levels)), self.to)    # whole: Unreal reads nothing less
        except OSError:
            return False
        self.passed += 1
        self._note(universe, now, None, None, None, bytes(levels))
        if self.recording:
            try:
                self.recording.write(now - self.record_started, universe, bytes(levels))
            except OSError as error:
                self.record_error = str(error)
                self.recording = None
        return True

    def _note(self, universe, now, address, name, priority, passed_levels):
        """Keeps what the monitor shows of a universe: who sends it, how often, and what was last passed on."""
        row = self.seen.get(universe)
        if row is None:
            row = self.seen[universe] = {"packets": 0, "since": now, "rate": 0.0, "levels": b"", "changed": now, "sources": {}}
        if address is not None:
            row["packets"] += 1
            row["sources"][address] = {"name": name or "", "priority": priority, "heard": now}
        if passed_levels is not None and passed_levels != row["levels"]:
            row["levels"] = passed_levels
            row["changed"] = now

    # ----------------------------------------------------------------------- monitor and orders

    def snapshot(self, now, watch=None):
        """What is arriving, as plain data for the monitor file.

        universes: for each one heard in the last few seconds, its number, packets a second, the
        number of slots above zero in what was passed on, seconds since that last changed, and
        who sends it (address, sACN source name and priority). With `watch`, that universe's
        levels as passed on are included.
        """
        rows = []
        for universe in sorted(self.seen):
            row = self.seen[universe]
            elapsed = now - row["since"]
            if elapsed >= 0.4:
                row["rate"] = row["packets"] / elapsed
                row["packets"] = 0
                row["since"] = now
            row["sources"] = {address: source for address, source in row["sources"].items() if now - source["heard"] <= SOURCE_TIMEOUT}
            if not row["sources"]:
                continue    # nobody is sending it any more
            rows.append({"universe": universe, "rate": round(row["rate"], 1), "slots_in_use": sum(1 for level in row["levels"] if level),
                         "still_for": round(now - row["changed"], 1),
                         "sources": [{"address": address, "name": source["name"], "priority": source["priority"]}
                                     for address, source in sorted(row["sources"].items())]})
        state = {"protocol": self.protocol, "interface": self.interface, "universes": rows, "passed": self.passed, "other": self.other,
                 "recording": None}
        if self.recording:
            state["recording"] = {"file": os.path.basename(self.recording.path), "seconds": round(now - self.record_started, 1),
                                  "universes": len(self.recording.universes), "frames": self.recording.frames}
        if self.record_error:
            state["record_error"] = self.record_error
        if watch and watch in self.seen:
            state["watch"] = {"universe": watch, "levels": list(self.seen[watch]["levels"])}
        return state

    def record(self, path, now=None):
        """Starts recording what is passed on into `path`, or with None finishes the recording in hand."""
        if self.recording and (path is None or path != self.recording.path):
            try:
                self.recording.close()
            except OSError as error:
                self.record_error = str(error)
            self.recording = None
        if path and not self.recording:
            try:
                self.recording = dmx_record.Writer(path)
                self.record_started = time.monotonic() if now is None else now
                self.record_error = ""
            except OSError as error:
                self.record_error = str(error)

    def run(self, keep_going, poll=0.25, monitor=None, control=None):
        orders = {}
        orders_stamp = None
        next_report = 0.0
        while keep_going(orders):
            ready = select.select(self.sockets, [], [], poll)[0]
            now = time.monotonic()
            seen = set()    # a datagram for a group reaches every socket here; pass it on once
            for sock in ready:
                while True:
                    try:
                        data, sender = sock.recvfrom(2048)
                    except BlockingIOError:
                        break
                    if len(self.sockets) > 1:
                        key = (data[22:38], data[111:115])    # source, sequence number and universe
                        if key in seen:
                            continue
                        seen.add(key)
                    self.handle(data, now, sender)
            if control:
                try:
                    stamp = os.stat(control).st_mtime_ns
                    if stamp != orders_stamp:
                        with open(control, encoding="utf-8") as handle:
                            read = json.load(handle)
                        orders = read if isinstance(read, dict) else {}
                        orders_stamp = stamp
                        wanted = orders.get("record")
                        self.record(wanted if isinstance(wanted, str) and wanted else None, now)
                except (OSError, ValueError):
                    pass    # no orders, or half-written ones: carry on as before
            if monitor and now >= next_report:
                next_report = now + 0.5
                watch = orders.get("watch")
                try:
                    with open(monitor + ".part", "w", encoding="utf-8") as handle:
                        json.dump(dict(self.snapshot(now, watch if isinstance(watch, int) else None), written=time.time()), handle)
                    os.replace(monitor + ".part", monitor)
                except OSError:
                    pass


def alive(pid):
    return oslayer.process_alive(pid)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--protocol", choices=("sacn", "artnet"), default="sacn", help="what the console sends (default sacn)")
    p.add_argument("--interface", required=True, help="this computer's address on the lighting network")
    p.add_argument("--universes", type=int, default=64, help="sACN: listen for universes 1 to this (default 64)")
    p.add_argument("--while-pid", type=int, help="stop when the process with this id is gone")
    p.add_argument("--parent-pid", type=int, help="keep going while this process lives, whatever --while-pid says")
    p.add_argument("--monitor", help="write what is arriving to this file, twice a second")
    p.add_argument("--control", help="read orders from this file")
    p.add_argument("--listen-port", type=int, help=argparse.SUPPRESS)    # for the tests
    p.add_argument("--to-port", type=int, default=ARTNET_PORT, help=argparse.SUPPRESS)
    args = p.parse_args(argv)
    try:
        relay = Relay(args.interface, max(1, min(args.universes, 2000)), args.listen_port, ("127.0.0.1", args.to_port), args.protocol)
    except OSError as error:
        print(f"error: cannot listen for {'sACN' if args.protocol == 'sacn' else 'Art-Net'} on {args.interface}: {error}", file=sys.stderr)
        return 1
    stop = []
    signal.signal(signal.SIGTERM, lambda *_: stop.append(True))

    def keep_going(orders):
        if stop:
            return False
        served = orders.get("while_pid") if isinstance(orders.get("while_pid"), int) else args.while_pid
        if args.parent_pid is None and served is None:
            return True    # nobody to outlive: until told to stop
        return (args.parent_pid is not None and alive(args.parent_pid)) or (served is not None and alive(served))

    print("READY", flush=True)
    try:
        relay.run(keep_going, monitor=args.monitor, control=args.control)
    except KeyboardInterrupt:
        pass
    finally:
        relay.close()
        if args.monitor:
            try:
                os.remove(args.monitor)    # nothing is being watched any more
            except OSError:
                pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
