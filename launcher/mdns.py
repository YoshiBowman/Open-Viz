"""Multicast DNS, as far as Openviz needs it: finding the services of a type with their address,
port and text record, and answering for one service of our own. Standard library only.

`browse` asks and listens for a moment. `Responder` answers questions about one service until it
is stopped, without going through the system's own responder, because MVR-xchange stations of
one group all announce themselves under the same name, which the system's would refuse.
"""

import socket
import struct
import threading
import time

from ndi_find import GROUP, PORT, encode_name, read_name

A, PTR, TXT, SRV, ANY = 1, 12, 16, 33, 255
IN = 1
TOP = 0x8000    # on a question's class: answer me alone; on a record's: forget what you had


def question(name, kind=PTR, unicast=True, ident=0):
    return struct.pack(">HHHHHH", ident, 0, 1, 0, 0, 0) + encode_name(name) + struct.pack(">HH", kind, IN | (TOP if unicast else 0))


def record(name, kind, data, ttl=120, flush=False):
    return encode_name(name) + struct.pack(">HHIH", kind, IN | (TOP if flush else 0), ttl, len(data)) + data


def text_data(text):
    """A text record: "key=value" strings, each with its length before it."""
    out = b""
    for key, value in text.items():
        entry = f"{key}={value}".encode("utf-8")[:255]
        out += bytes([len(entry)]) + entry
    return out or b"\0"


def response(records, ident=0, questions=b"", count=0):
    return struct.pack(">HHHHHH", ident, 0x8400, count, len(records), 0, 0) + questions + b"".join(records)


def parse(data):
    """A packet as {"ident", "response", "questions": [(name, kind, unicast)], "records": [(name, kind, ttl, value)]}.
    Names are lower-case and end without a dot. A record's value is a name (PTR), (port, name)
    (SRV), a dict (TXT) or an address (A); other kinds are passed over. None for what is no DNS packet."""
    try:
        if len(data) < 12:
            return None
        ident, flags, questions, answers, authorities, additional = struct.unpack(">HHHHHH", data[:12])
        out = {"ident": ident, "response": bool(flags & 0x8000), "questions": [], "records": []}
        at = 12
        for _ in range(questions):
            labels, at = read_name(data, at)
            kind, klass = struct.unpack(">HH", data[at:at + 4])
            at += 4
            out["questions"].append((".".join(labels).casefold(), kind, bool(klass & TOP)))
        for _ in range(answers + authorities + additional):
            labels, at = read_name(data, at)
            if at + 10 > len(data):
                break
            kind, _, ttl, length = struct.unpack(">HHIH", data[at:at + 10])
            at += 10
            if at + length > len(data):
                break
            name = ".".join(labels).casefold()
            if kind == PTR:
                out["records"].append((name, kind, ttl, ".".join(read_name(data, at)[0])))
            elif kind == SRV and length >= 7:
                out["records"].append((name, kind, ttl, (struct.unpack(">H", data[at + 4:at + 6])[0], ".".join(read_name(data, at + 6)[0]).casefold())))
            elif kind == TXT:
                text, inside = {}, at
                while inside < at + length:
                    size = data[inside]
                    entry = data[inside + 1:inside + 1 + size].decode("utf-8", "replace")
                    if "=" in entry:
                        key, value = entry.split("=", 1)
                        text[key] = value
                    inside += 1 + size
                out["records"].append((name, kind, ttl, text))
            elif kind == A and length == 4:
                out["records"].append((name, kind, ttl, socket.inet_ntoa(data[at:at + 4])))
            at += length
        return out
    except (ValueError, struct.error, IndexError):
        return None


def browse(service, seconds=1.5, addresses=None, group=(GROUP, PORT)):
    """The services of this type heard of within `seconds`: [{"name", "host", "port", "addresses", "text"}].

    `name` is the service's own name without its type. Two computers may announce the same name
    (MVR-xchange stations do): each is listed, told apart by where it answered from and its port."""
    service = service.rstrip(".").casefold()
    sockets = []
    for address in addresses or [None]:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
            if address:
                sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(address))
                sock.bind((address, 0))
            sock.setblocking(False)
            sock.sendto(question(service), group)
            sockets.append(sock)
        except OSError:
            sock.close()
    found = {}      # (name, where it answered from, port) -> entry
    loose = {}      # (name, where from) -> what arrived without a port: text, to be joined on
    hosts = {}      # host name -> addresses
    asked = set()
    written_as = {}    # a service's name as its owner writes it, capitals and all
    deadline = time.time() + seconds
    again = time.time() + seconds / 2.0
    try:
        while time.time() < deadline:
            if again and time.time() >= again:
                again = None    # packets get lost: ask a second time halfway
                for sock in sockets:
                    try:
                        sock.sendto(question(service), group)
                    except OSError:
                        pass
            for sock in sockets:
                try:
                    while True:
                        data, sender = sock.recvfrom(9000)
                        packet = parse(data)
                        if not packet or not packet["response"]:
                            continue
                        records = packet["records"]
                        for name, kind, _, value in records:
                            if kind == A:
                                hosts.setdefault(name, [])
                                if value not in hosts[name]:
                                    hosts[name].append(value)
                        instances = {value.casefold(): value for name, kind, _, value in records if kind == PTR and name == service}
                        for name, kind, _, _ in records:    # an answer about one service by name: as it was written, if we heard that
                            if kind in (SRV, TXT) and name.endswith("." + service):
                                instances.setdefault(name, written_as.get(name, name))
                        written_as.update(instances)
                        for key, written in instances.items():
                            if not key.endswith("." + service):
                                continue
                            ports = [value for name, kind, _, value in records if kind == SRV and name == key]
                            texts = [value for name, kind, _, value in records if kind == TXT and name == key]
                            label = written[:len(written) - len(service) - 1]
                            if ports:
                                port, host = ports[0]
                                entry = found.setdefault((key, sender[0], port), {"name": label, "host": host, "port": port, "from": sender[0], "text": {}})
                                if texts:
                                    entry["text"] = texts[0]
                                elif (key, sender[0]) in loose:
                                    entry["text"] = loose[(key, sender[0])]
                            else:
                                if texts:
                                    loose[(key, sender[0])] = texts[0]
                                    for (name, where, _), entry in found.items():
                                        if name == key and where == sender[0] and not entry["text"]:
                                            entry["text"] = texts[0]
                                if (key, sender[0]) not in asked:    # only its name came: ask for the rest
                                    asked.add((key, sender[0]))
                                    try:
                                        sock.sendto(question(key, SRV), group)
                                        sock.sendto(question(key, TXT), group)
                                    except OSError:
                                        pass
                except (BlockingIOError, OSError):
                    pass
            time.sleep(0.03)
    finally:
        for sock in sockets:
            sock.close()
    # One station answers on every network it is on: the same name, host and port are one station.
    merged = {}
    for (key, _, _), entry in found.items():
        station = merged.setdefault((key, entry["host"], entry["port"]), {"name": written_as.get(key, entry["name"] + "." + service)[:-len(service) - 1],
                                                                          "host": entry["host"], "port": entry["port"], "addresses": [], "text": {}})
        station["text"] = station["text"] or entry["text"]
        # Where it says it is; and where it answered from, which is the surest way back to it.
        for address in [entry["from"], *hosts.get(entry["host"], [])]:
            if address not in station["addresses"] and not address.startswith("169.254."):
                station["addresses"].append(address)
    return sorted(merged.values(), key=lambda entry: (entry["name"].casefold(), entry["host"], entry["port"]))


def hear(questions, seconds=1.5, addresses=None, group=(GROUP, PORT)):
    """Asks `questions` ((name, kind) pairs) on every address and returns every answer heard within
    `seconds`, as (packet, sender's address) pairs, packets as `parse` gives them.

    An answer may come straight back to the socket that asked, or be said to everybody on the
    mDNS port, which is all some programs do (grandMA3 among them): both are listened to. The
    port is shared with whatever else on this computer speaks mDNS."""
    asking = []
    for address in addresses or [None]:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
            if address:
                sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(address))
                sock.bind((address, 0))
            sock.setblocking(False)
            asking.append(sock)
        except OSError:
            sock.close()
    listening = list(asking)
    try:
        multicast = 224 <= int(group[0].split(".")[0]) <= 239
    except ValueError:
        multicast = False
    if multicast:
        shared = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            shared.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            if hasattr(socket, "SO_REUSEPORT"):
                shared.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
            shared.bind(("", group[1]))
            for address in addresses or []:
                if address.startswith("127."):
                    continue
                try:
                    shared.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, socket.inet_aton(group[0]) + socket.inet_aton(address))
                except OSError:
                    pass    # a network that is down, or joined already
            shared.setblocking(False)
            listening.append(shared)
        except OSError:
            shared.close()

    def ask():
        for sock in asking:
            for name, kind in questions:
                try:
                    sock.sendto(question(name, kind), group)
                except OSError:
                    pass

    heard = []
    deadline = time.time() + seconds
    again = time.time() + seconds / 2.0
    try:
        ask()
        while time.time() < deadline:
            if again and time.time() >= again:
                again = None    # packets get lost: ask a second time halfway
                ask()
            for sock in listening:
                try:
                    while True:
                        data, sender = sock.recvfrom(9000)
                        packet = parse(data)
                        if packet and packet["response"]:
                            heard.append((packet, sender[0]))
                except (BlockingIOError, OSError):
                    pass
            time.sleep(0.03)
    finally:
        for sock in listening:
            sock.close()
    return heard


def nearest(addresses, other):
    """Of this computer's addresses, the one most likely on the same network as `other`: the one that shares the most leading bits with it."""
    def bits(address):
        try:
            return struct.unpack(">I", socket.inet_aton(address))[0]
        except OSError:
            return 0
    if not addresses:
        return None
    wanted = bits(other)
    if not wanted:
        return addresses[0]    # nothing to go by: the first
    return max(addresses, key=lambda address: 32 - (bits(address) ^ wanted).bit_length())


class Responder:
    """Answers multicast DNS questions about one service: its name, where it is and its text record.

    `instance` is the service's own name, `service` its type ("_mvrxchange._tcp.local."), `also`
    further names that lead to it (a sub-type). It announces itself when started and says goodbye
    when stopped. Each answer names the one address of this computer nearest the asker."""

    def __init__(self, instance, service, port, text, addresses, host, also=(), group=(GROUP, PORT), bind_port=None):
        self.service = service.rstrip(".").casefold()
        self.written = f"{instance}.{service.rstrip('.')}"
        self.name = self.written.casefold()
        self.port = port
        self.text = dict(text)
        self.addresses = [address for address in addresses if not address.startswith("127.")] or list(addresses) or ["127.0.0.1"]
        self.host = host.rstrip(".").casefold()
        self.also = [name.rstrip(".").casefold() for name in also]
        self.group = group
        self.bind_port = group[1] if bind_port is None else bind_port
        self.sock = None
        self.thread = None
        self.running = False
        self.answered = 0

    def records(self, address, ttl=120):
        return [record(self.service, PTR, encode_name(self.written), ttl),
                *[record(name, PTR, encode_name(self.written), ttl) for name in self.also],
                record(self.written, SRV, struct.pack(">HHH", 0, 0, self.port) + encode_name(self.host), ttl, flush=True),
                record(self.written, TXT, text_data(self.text), ttl, flush=True),
                record(self.host, A, socket.inet_aton(address), ttl, flush=True)]

    def wants(self, packet):
        """Whether a question in this packet is one of ours to answer."""
        ours = {self.service, self.name, self.host, *self.also}
        return any(name in ours and kind in (PTR, SRV, TXT, A, ANY) for name, kind, _ in packet["questions"])

    def start(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if hasattr(socket, "SO_REUSEPORT"):
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
        sock.bind(("", self.bind_port))
        try:
            socket.inet_aton(self.group[0])
            multicast = 224 <= int(self.group[0].split(".")[0]) <= 239
        except (OSError, ValueError):
            multicast = False
        if multicast:
            for address in self.addresses:
                try:
                    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, socket.inet_aton(self.group[0]) + socket.inet_aton(address))
                except OSError:
                    pass    # a network that is down, or joined already
        sock.settimeout(0.25)
        self.sock = sock
        self.multicast = multicast
        self.running = True
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()
        self.announce()

    def _to_all(self, ttl):
        for address in self.addresses:
            try:
                if self.multicast:
                    self.sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(address))
                self.sock.sendto(response(self.records(address, ttl)), self.group)
            except OSError:
                pass

    def announce(self):
        if self.sock and self.multicast:
            self._to_all(120)

    def _run(self):
        while self.running:
            try:
                data, sender = self.sock.recvfrom(9000)
            except socket.timeout:
                continue
            except OSError:
                break
            packet = parse(data)
            if not packet or packet["response"] or not self.wants(packet):
                continue
            address = nearest(self.addresses, sender[0])
            records = self.records(address)
            self.answered += 1
            try:
                if sender[1] != PORT or any(unicast for _, _, unicast in packet["questions"]) or not self.multicast:
                    # Straight back to the asker, with its question and its number repeated, as a plain DNS asker expects.
                    asked = b"".join(encode_name(name) + struct.pack(">HH", kind, IN) for name, kind, _ in packet["questions"])
                    self.sock.sendto(response(records, packet["ident"], asked, len(packet["questions"])), sender)
                if self.multicast:
                    self.sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(address))
                    self.sock.sendto(response(records), self.group)
            except OSError:
                pass

    def stop(self):
        if not self.running:
            return
        self.running = False
        if self.multicast:
            self._to_all(0)    # goodbye: records that live no longer
        if self.thread:
            self.thread.join(2)
        self.sock.close()
        self.sock = None
