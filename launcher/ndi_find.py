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
    python3 ndi_find.py --library <libndi> --frame "NAME" --out frame.png
                                                saves one frame of a stream, at its full size, as PNG

A frame is what the Screens tab's Custom Crop shows to draw a screen's part of a stream on (owner,
2026-10-27: "see a preview of the ndi feed and essentially draw out what part of it you want").
"""
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

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


def grab_with_library(library, name, seconds=6.0):
    """One frame of the NDI stream `name` at its full size, received by NDI's own library in a process
    of its own: (PNG bytes, width, height), or None with why: (None, reason)."""
    handle, out = tempfile.mkstemp(suffix=".png", prefix="openviz-ndi-")
    os.close(handle)
    try:
        done = subprocess.run([sys.executable, os.path.join(HERE, "ndi_find.py"), "--library", library, "--frame", name, "--seconds", str(seconds), "--out", out],
                              capture_output=True, text=True, timeout=seconds + 20)
        said = json.loads(done.stdout or "{}")
        if not isinstance(said, dict) or not said.get("width"):
            return None, (said.get("error") if isinstance(said, dict) else "") or "no frame arrived"
        with open(out, "rb") as png:
            return (png.read(), int(said["width"]), int(said["height"])), ""
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        return None, str(error)
    finally:
        try:
            os.remove(out)
        except OSError:
            pass


def png_rgba(width, height, rgba):
    """A PNG of `rgba` (width x height pixels, four bytes each, rows top down). Standard library only."""
    stride = width * 4
    rows = b"".join(b"\x00" + rgba[row * stride:(row + 1) * stride] for row in range(height))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 1)) + chunk(b"IEND", b""))


def _fourcc(text):
    return text[0] | text[1] << 8 | text[2] << 16 | text[3] << 24


def _grab_library(library, name, seconds):
    """What --frame does, in this process: {"width", "height", "png"} or {"error"}."""
    import ctypes
    ndi = ctypes.CDLL(library)
    ndi.NDIlib_initialize.restype = ctypes.c_bool
    if not ndi.NDIlib_initialize():
        return {"error": "NDI's library would not start on this computer"}

    class Source(ctypes.Structure):
        _fields_ = [("p_ndi_name", ctypes.c_char_p), ("p_url_address", ctypes.c_char_p)]

    class FindCreate(ctypes.Structure):
        _fields_ = [("show_local_sources", ctypes.c_bool), ("p_groups", ctypes.c_char_p), ("p_extra_ips", ctypes.c_char_p)]

    class RecvCreate(ctypes.Structure):
        _fields_ = [("source_to_connect_to", Source), ("color_format", ctypes.c_int), ("bandwidth", ctypes.c_int),
                    ("allow_video_fields", ctypes.c_bool), ("p_ndi_recv_name", ctypes.c_char_p)]

    class VideoFrame(ctypes.Structure):
        _fields_ = [("xres", ctypes.c_int), ("yres", ctypes.c_int), ("FourCC", ctypes.c_int), ("frame_rate_N", ctypes.c_int),
                    ("frame_rate_D", ctypes.c_int), ("picture_aspect_ratio", ctypes.c_float), ("frame_format_type", ctypes.c_int),
                    ("timecode", ctypes.c_int64), ("p_data", ctypes.POINTER(ctypes.c_uint8)), ("line_stride_in_bytes", ctypes.c_int),
                    ("p_metadata", ctypes.c_char_p), ("timestamp", ctypes.c_int64)]

    ndi.NDIlib_find_create_v2.restype = ctypes.c_void_p
    ndi.NDIlib_find_create_v2.argtypes = [ctypes.POINTER(FindCreate)]
    ndi.NDIlib_find_wait_for_sources.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    ndi.NDIlib_find_get_current_sources.restype = ctypes.POINTER(Source)
    ndi.NDIlib_find_get_current_sources.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
    ndi.NDIlib_find_destroy.argtypes = [ctypes.c_void_p]
    ndi.NDIlib_recv_create_v3.restype = ctypes.c_void_p
    ndi.NDIlib_recv_create_v3.argtypes = [ctypes.POINTER(RecvCreate)]
    ndi.NDIlib_recv_capture_v2.restype = ctypes.c_int
    ndi.NDIlib_recv_capture_v2.argtypes = [ctypes.c_void_p, ctypes.POINTER(VideoFrame), ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32]
    ndi.NDIlib_recv_free_video_v2.argtypes = [ctypes.c_void_p, ctypes.POINTER(VideoFrame)]
    ndi.NDIlib_recv_destroy.argtypes = [ctypes.c_void_p]

    deadline = time.time() + seconds
    finder = ndi.NDIlib_find_create_v2(ctypes.byref(FindCreate(True, None, None)))
    if not finder:
        return {"error": "NDI's library could not look for streams"}
    receiver = None
    try:
        # The stream's address, as NDI's own search finds it.
        wanted = name.encode("utf-8")
        source = None
        while time.time() < deadline and source is None:
            ndi.NDIlib_find_wait_for_sources(finder, 500)
            count = ctypes.c_uint32(0)
            found = ndi.NDIlib_find_get_current_sources(finder, ctypes.byref(count))
            for k in range(count.value):
                if found[k].p_ndi_name == wanted:
                    source = Source(found[k].p_ndi_name, found[k].p_url_address)
        if source is None:
            return {"error": f"{name} is not being sent on the network right now"}
        # Full size (bandwidth highest), each pixel as R, G, B and A, so that pixels drawn on it are the stream's own.
        receiver = ndi.NDIlib_recv_create_v3(ctypes.byref(RecvCreate(source, 2, 100, False, b"Openviz preview")))
        if not receiver:
            return {"error": "NDI's library could not receive the stream"}
        frame = VideoFrame()
        while time.time() < deadline:
            if ndi.NDIlib_recv_capture_v2(receiver, ctypes.byref(frame), None, None, 1000) != 1:
                continue
            try:
                width, height, stride = frame.xres, frame.yres, frame.line_stride_in_bytes
                data = ctypes.string_at(frame.p_data, stride * height)
                kind = frame.FourCC
            finally:
                ndi.NDIlib_recv_free_video_v2(receiver, ctypes.byref(frame))
            rows = data if stride == width * 4 else b"".join(data[row * stride:row * stride + width * 4] for row in range(height))
            pixels = bytearray(rows)
            if kind in (_fourcc(b"BGRA"), _fourcc(b"BGRX")):
                pixels[0::4], pixels[2::4] = pixels[2::4], pixels[0::4]
            elif kind not in (_fourcc(b"RGBA"), _fourcc(b"RGBX")):
                return {"error": "the stream came in a pixel format this preview does not read"}
            pixels[3::4] = b"\xff" * (width * height)    # seen whole: what is see-through in the stream is shown as it lights
            return {"width": width, "height": height, "png": png_rgba(width, height, bytes(pixels))}
        return {"error": f"{name} sent no picture within {seconds:g} seconds"}
    finally:
        if receiver:
            ndi.NDIlib_recv_destroy(receiver)
        ndi.NDIlib_find_destroy(finder)


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "--library":
        wait = float(sys.argv[sys.argv.index("--seconds") + 1]) if "--seconds" in sys.argv else 3.0
        if "--frame" in sys.argv:
            grabbed = _grab_library(sys.argv[2], sys.argv[sys.argv.index("--frame") + 1], wait)
            if "png" in grabbed:
                with open(sys.argv[sys.argv.index("--out") + 1], "wb") as saved:
                    saved.write(grabbed.pop("png"))
            print(json.dumps(grabbed))
        else:
            print(json.dumps(_ask_library(sys.argv[2], wait)))
    else:
        for name in find(2.5, sys.argv[1:] or None):
            print(name)
