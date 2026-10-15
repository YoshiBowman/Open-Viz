"""OSC coming in: Open Sound Control 1.0 messages and bundles over UDP, as a console, a control
surface or a show controller sends them. Only reading: Openviz sends no OSC.

grandMA3 sends what a macro tells it to, for example
    SendOSC 1 "/openviz/view,i,3"
and TouchOSC, QLab, Companion and the like send whatever address and value a control is given.
Openviz's own addresses are listed in the launcher (Launcher.osc_heard).
"""
import socket
import struct
import threading
import time


def _string(data, at):
    """An OSC string at `at`: its text, and where the next field starts (strings are padded to four bytes)."""
    end = data.index(b"\x00", at)
    text = data[at:end].decode("utf-8", "replace")
    return text, (end + 4) & ~3


def parse(data, depth=0):
    """The messages in one datagram, bundles opened: [(address, [arguments])]. What cannot be read
    is left out; a datagram that is not OSC at all gives []."""
    messages = []
    try:
        if data.startswith(b"#bundle\x00"):
            if depth > 8:
                return []
            at = 16    # the name and the time tag: everything is done as soon as it arrives
            while at + 4 <= len(data):
                size = struct.unpack_from(">i", data, at)[0]
                at += 4
                if size <= 0 or at + size > len(data):
                    break
                messages += parse(data[at:at + size], depth + 1)
                at += size
            return messages
        if not data.startswith(b"/"):
            return []
        address, at = _string(data, 0)
        arguments = []
        if at < len(data) and data[at:at + 1] == b",":
            tags, at = _string(data, at)
            for tag in tags[1:]:
                if tag == "i":
                    arguments.append(struct.unpack_from(">i", data, at)[0])
                    at += 4
                elif tag == "f":
                    arguments.append(struct.unpack_from(">f", data, at)[0])
                    at += 4
                elif tag == "h":
                    arguments.append(struct.unpack_from(">q", data, at)[0])
                    at += 8
                elif tag == "d":
                    arguments.append(struct.unpack_from(">d", data, at)[0])
                    at += 8
                elif tag in "sS":
                    text, at = _string(data, at)
                    arguments.append(text)
                elif tag == "b":
                    size = struct.unpack_from(">i", data, at)[0]
                    arguments.append(bytes(data[at + 4:at + 4 + size]))
                    at = (at + 4 + size + 3) & ~3
                elif tag == "T":
                    arguments.append(True)
                elif tag == "F":
                    arguments.append(False)
                elif tag in "NI":
                    arguments.append(None)
                elif tag in "crm":
                    arguments.append(struct.unpack_from(">I", data, at)[0])
                    at += 4
                else:
                    break    # a type this does not know: what follows cannot be found
        messages.append((address, arguments))
    except (ValueError, struct.error):
        pass
    return messages


def message(address, *arguments):
    """An OSC message, for tests and for anyone who wants to send one: ints, floats, strings, True, False."""
    def padded(raw):
        raw += b"\x00"
        return raw + b"\x00" * (-len(raw) % 4)
    tags, body = ",", b""
    for value in arguments:
        if value is True or value is False:
            tags += "T" if value else "F"
        elif isinstance(value, int):
            tags += "i"
            body += struct.pack(">i", value)
        elif isinstance(value, float):
            tags += "f"
            body += struct.pack(">f", value)
        else:
            tags += "s"
            body += padded(str(value).encode("utf-8"))
    return padded(address.encode("utf-8")) + padded(tags.encode("ascii")) + body


def bundle(*contents):
    """An OSC bundle of messages (or bundles), to be done at once."""
    out = b"#bundle\x00" + struct.pack(">Q", 1)
    for content in contents:
        out += struct.pack(">i", len(content)) + content
    return out


class Listener:
    """Listens for OSC on a UDP port, on every address of this computer, and hands each message to
    `handle(address, arguments, sender)` on its own thread."""

    def __init__(self, port, handle, host=""):
        self.port = port
        self.handle = handle
        self.host = host
        self._socket = None
        self._thread = None
        self._stop = threading.Event()
        self.received = 0

    def start(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.bind((self.host, self.port))    # OSError if the port is taken: the caller says so
        except OSError:
            sock.close()
            raise
        sock.settimeout(0.25)
        self._socket = sock
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="osc", daemon=True)
        self._thread.start()

    def _run(self):
        while not self._stop.is_set():
            try:
                data, sender = self._socket.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                break
            for address, arguments in parse(data):
                self.received += 1
                try:
                    self.handle(address, arguments, sender)
                except Exception:    # noqa: BLE001 - one bad message must not end the listening
                    pass

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2)
        if self._socket:
            self._socket.close()
        self._socket = None
        self._thread = None

    @property
    def running(self):
        return self._thread is not None and self._thread.is_alive()


def _now():
    return time.time()
