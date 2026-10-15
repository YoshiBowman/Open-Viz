"""Finds the NDI streams on the network, by the names a screen is told to show.

NDI senders announce themselves the way printers and AirPlay speakers do: by multicast DNS, as
services of the type "_ndi._tcp". This asks for them and listens for a moment. Standard library
only; nothing of NDI itself is needed to ask.

    python3 ndi_find.py            lists what is there
"""

import socket
import struct
import sys
import time

GROUP, PORT = "224.0.0.251", 5353
SERVICE = "_ndi._tcp.local."
PTR, IN, UNICAST = 12, 1, 0x8000


def encode_name(name):
    out = b""
    for label in name.rstrip(".").split("."):
        raw = label.encode("utf-8")
        out += bytes([len(raw)]) + raw
    return out + b"\0"


def query(service=SERVICE):
    """One question: who offers this service? Answers are asked for straight back to the asker."""
    return struct.pack(">HHHHHH", 0, 0, 1, 0, 0, 0) + encode_name(service) + struct.pack(">HH", PTR, IN | UNICAST)


def read_name(data, at, depth=0):
    """The labels of the name at `at`, and where what follows it begins. Names may point back into
    the packet to save room; a packet that points in circles is refused."""
    labels = []
    after = None
    while True:
        if at >= len(data) or depth > 16:
            raise ValueError("a name runs off the packet")
        length = data[at]
        if length == 0:
            at += 1
            break
        if length & 0xC0 == 0xC0:
            if at + 1 >= len(data):
                raise ValueError("a name runs off the packet")
            if after is None:
                after = at + 2
            at = ((length & 0x3F) << 8) | data[at + 1]
            depth += 1
            continue
        if length & 0xC0 or at + 1 + length > len(data):
            raise ValueError("a label is not as it should be")
        labels.append(data[at + 1:at + 1 + length].decode("utf-8", "replace"))
        at += 1 + length
    return labels, (at if after is None else after)


def instances(data, service=SERVICE):
    """The names of the services of this type that an answer lists. Nothing for a packet that is not one."""
    wanted = [label.casefold() for label in service.rstrip(".").split(".")]
    found = []
    try:
        if len(data) < 12:
            return found
        _, flags, questions, answers, authorities, additional = struct.unpack(">HHHHHH", data[:12])
        if not flags & 0x8000:
            return found    # a question, ours or another's
        at = 12
        for _ in range(questions):
            _, at = read_name(data, at)
            at += 4
        for _ in range(answers + authorities + additional):
            owner, at = read_name(data, at)
            if at + 10 > len(data):
                break
            kind, _, _, length = struct.unpack(">HHIH", data[at:at + 10])
            at += 10
            if at + length > len(data):
                break
            if kind == PTR and [label.casefold() for label in owner] == wanted:
                target, _ = read_name(data, at)
                if len(target) > len(wanted) and [label.casefold() for label in target[-len(wanted):]] == wanted:
                    found.append(".".join(target[:-len(wanted)]))
            at += length
    except (ValueError, struct.error):
        pass
    return found


def find(seconds=1.5, addresses=None, service=SERVICE, group=(GROUP, PORT)):
    """The NDI streams heard of within `seconds`, sorted. `addresses` are this computer's own
    network addresses to ask on (default: wherever the system sends multicast)."""
    sockets = []
    for address in addresses or [None]:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
            if address:
                sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(address))
                sock.bind((address, 0))
            sock.setblocking(False)
            sock.sendto(query(service), group)
            sockets.append(sock)
        except OSError:
            sock.close()    # an address this computer no longer has, or a network that is down
    names = set()
    deadline = time.time() + seconds
    asked_again = False
    try:
        while time.time() < deadline:
            if not asked_again and deadline - time.time() < seconds / 2.0:
                asked_again = True    # packets get lost: ask a second time halfway
                for sock in sockets:
                    try:
                        sock.sendto(query(service), group)
                    except OSError:
                        pass
            for sock in sockets:
                try:
                    while True:
                        data, _ = sock.recvfrom(9000)
                        names.update(instances(data, service))
                except (BlockingIOError, OSError):
                    pass
            time.sleep(0.03)
    finally:
        for sock in sockets:
            sock.close()
    return sorted(names, key=str.casefold)


if __name__ == "__main__":
    for name in find(2.5, sys.argv[1:] or None):
        print(name)
