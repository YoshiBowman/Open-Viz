"""Finds the NDI streams on the network, by the names a screen is told to show.

NDI senders announce themselves the way printers and AirPlay speakers do: by multicast DNS, as
services of the type "_ndi._tcp". This asks for them and listens for a moment, for answers sent
straight back and for answers sent to everybody on the shared mDNS port (a Hippotizer answers
only so: found 2026-10-20, when Openviz listed none of its streams). Standard library only.

Where Unreal's NDI library is at hand, find_with_library asks NDI itself, as the visualizer's
receiver does, so that discovery servers and addresses set up in NDI's own settings count too.
It runs in a process of its own (`python3 ndi_find.py --library <libndi>`), so that a library
that misbehaves cannot take Openviz with it.

    python3 ndi_find.py                         lists what is there, by multicast DNS
    python3 ndi_find.py --library <libndi>      lists what NDI's own library finds
"""
import json
import os
import socket
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))

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
    # Answers sent to everybody arrive on the shared port, for whoever has joined the group there.
    for address in addresses or [None]:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            if hasattr(socket, "SO_REUSEPORT"):
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
            sock.bind(("", group[1]))
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, socket.inet_aton(group[0]) + socket.inet_aton(address or "0.0.0.0"))
            sock.setblocking(False)
            sockets.append(sock)
        except OSError:
            sock.close()    # someone holds the port without sharing it, or the address is gone: the direct answers still count
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


def library_path(engine):
    """Where Unreal keeps its NDI library, under an engine folder (the one above Engine/), or None."""
    if not engine:
        return None
    for relative in (("Mac", "libndi.dylib"), ("Win64", "Processing.NDI.Lib.x64.dll"), ("Linux", "libndi.so")):
        path = os.path.join(engine, "Engine", "Plugins", "Media", "NDIMedia", "Binaries", "ThirdParty", *relative)
        if os.path.isfile(path):
            return path
    return None


def find_with_library(library, seconds=3.0):
    """The names NDI's own library finds within `seconds`, or None if it cannot be asked."""
    try:
        done = subprocess.run([sys.executable, os.path.join(HERE, "ndi_find.py"), "--library", library, "--seconds", str(seconds)],
                              capture_output=True, text=True, timeout=seconds + 15)
        found = json.loads(done.stdout or "null")
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return None
    return sorted(found, key=str.casefold) if isinstance(found, list) and all(isinstance(name, str) for name in found) else None


def _ask_library(library, seconds):
    import ctypes
    ndi = ctypes.CDLL(library)
    ndi.NDIlib_initialize.restype = ctypes.c_bool
    if not ndi.NDIlib_initialize():
        return []

    class FindCreate(ctypes.Structure):
        _fields_ = [("show_local_sources", ctypes.c_bool), ("p_groups", ctypes.c_char_p), ("p_extra_ips", ctypes.c_char_p)]

    class Source(ctypes.Structure):
        _fields_ = [("p_ndi_name", ctypes.c_char_p), ("p_url_address", ctypes.c_char_p)]

    ndi.NDIlib_find_create_v2.restype = ctypes.c_void_p
    ndi.NDIlib_find_create_v2.argtypes = [ctypes.POINTER(FindCreate)]
    ndi.NDIlib_find_wait_for_sources.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    ndi.NDIlib_find_get_current_sources.restype = ctypes.POINTER(Source)
    ndi.NDIlib_find_get_current_sources.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
    ndi.NDIlib_find_destroy.argtypes = [ctypes.c_void_p]
    finder = ndi.NDIlib_find_create_v2(ctypes.byref(FindCreate(True, None, None)))
    if not finder:
        return []
    deadline = time.time() + seconds
    while time.time() < deadline:
        ndi.NDIlib_find_wait_for_sources(finder, max(1, int(min(1.0, deadline - time.time()) * 1000)))
    count = ctypes.c_uint32(0)
    sources = ndi.NDIlib_find_get_current_sources(finder, ctypes.byref(count))
    names = [sources[k].p_ndi_name.decode("utf-8", "replace") for k in range(count.value) if sources[k].p_ndi_name]
    ndi.NDIlib_find_destroy(finder)
    return names


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "--library":
        wait = float(sys.argv[sys.argv.index("--seconds") + 1]) if "--seconds" in sys.argv else 3.0
        print(json.dumps(_ask_library(sys.argv[2], wait)))
    else:
        for name in find(2.5, sys.argv[1:] or None):
            print(name)
