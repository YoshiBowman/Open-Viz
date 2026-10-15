"""MVR-xchange: rigs passed between programs over the network, without a file changing hands.

This is the "TCP mode" of the communication format in the MVR standard (DIN SPEC 15801). Stations
of one group find each other by multicast DNS (`_mvrxchange._tcp`, each under the group's name
with its station name and id in the text record), tell each other of a new MVR file with
`MVR_COMMIT`, and fetch one with `MVR_REQUEST`. Every message is its own connection: one packet
there, one back, closed.

A packet is six numbers and a payload, numbers most significant byte first:
    uint32 778682, uint32 version (1), uint32 number of this package, uint32 packages in the
    message, uint32 kind (0 JSON, 1 an MVR file), uint64 length of the payload

Written from the standard's text, with the reference library read for what it leaves open: the
group's name is the service's own name, a request names its stations as a list, and every field
of a message is sent even when it is empty, because the library does not take a missing one.
Not done: the WebSocket mode, and `MVR_NEW_SESSION_HOST`.

Standard library only.
"""

import json
import os
import socket
import struct
import threading
import time
import uuid

import mdns

SERVICE = "_mvrxchange._tcp.local."
MAGIC = 778682
VERSION = 1
JSON_KIND, FILE_KIND = 0, 1
HEADER = struct.Struct(">IIIIIQ")
MAX_JSON = 8 << 20
MAX_FILE = 4 << 30
MVR_VERSION = (1, 6)    # the version of the MVR format this reads and writes


class Refused(Exception):
    """Something to tell the user: the other station said no, or could not be reached."""


def packet(payload, kind=JSON_KIND, number=0, count=1):
    return HEADER.pack(MAGIC, VERSION, number, count, kind, len(payload)) + payload


def message(kind, **fields):
    """A JSON message as a packet."""
    return packet(json.dumps(dict({"Type": kind}, **fields)).encode("utf-8"))


def _exactly(sock, size):
    parts, left = [], size
    while left:
        chunk = sock.recv(min(left, 1 << 16))
        if not chunk:
            raise Refused("the connection broke off")
        parts.append(chunk)
        left -= len(chunk)
    return b"".join(parts)


def receive(sock, into=None):
    """Reads one message: (JSON_KIND, the message as a dict) or (FILE_KIND, the path it was saved to).

    A file is only taken if `into` says where to put it. A message may come in several packages."""
    kind, parts, total, expected, handle = None, [], 0, None, None
    try:
        while True:
            magic, _, number, count, this_kind, length = HEADER.unpack(_exactly(sock, HEADER.size))
            if magic != MAGIC:
                raise Refused("that is not an MVR-xchange packet")
            if kind is None:
                kind, expected = this_kind, max(count, 1)
                if kind not in (JSON_KIND, FILE_KIND):
                    raise Refused("a packet of a kind this does not know")
                if kind == FILE_KIND:
                    if into is None:
                        raise Refused("a file arrived that was not asked for")
                    handle = open(into, "wb")
            elif this_kind != kind:
                raise Refused("the packages of one message disagree about what it is")
            total += length
            if total > (MAX_JSON if kind == JSON_KIND else MAX_FILE):
                raise Refused("the message is larger than this takes")
            if kind == JSON_KIND:
                parts.append(_exactly(sock, length))
            else:
                left = length
                while left:
                    chunk = sock.recv(min(left, 1 << 20))
                    if not chunk:
                        raise Refused("the file broke off")
                    handle.write(chunk)
                    left -= len(chunk)
            if number + 1 >= expected:
                break
    except (OSError, struct.error) as error:
        raise Refused(f"the message could not be read: {error}") from None
    finally:
        if handle:
            handle.close()
    if kind == FILE_KIND:
        return FILE_KIND, into
    try:
        said = json.loads(b"".join(parts).decode("utf-8", "replace"))
    except ValueError:
        raise Refused("the message is not JSON") from None
    if not isinstance(said, dict) or not isinstance(said.get("Type"), str):
        raise Refused("the message does not say what it is")
    return JSON_KIND, said


def whole(value, fallback=0):
    """A number that may have been written as text, as some stations do."""
    try:
        return int(value)
    except (TypeError, ValueError):
        return fallback


def yes(value):
    return value is True or (isinstance(value, str) and value.strip().lower() == "true")


def same(a, b):
    return isinstance(a, str) and isinstance(b, str) and a.strip().casefold() == b.strip().casefold() and bool(a.strip())


def in_group(name, group):
    """Whether a group's name, as a station gives it, is this group's. Capitals do not count."""
    return name.casefold() == group.casefold()


# How stations find each other. The standard says only that a station registers the service
# `_mvrxchange._tcp.local.` and the "sub service" `<group>._mvrxchange._tcp.local.`. What
# grandMA3 does with that (2.5.1, read off the wire on 2026-10-12), and so what is done here:
#
#   asked for PTR of either name, a station answers, to everybody on the mDNS port, with four
#   records that all belong to the name <group>._mvrxchange._tcp.local:
#     PTR  -> <station>.<group>._mvrxchange._tcp.local
#     A    -> its address
#     SRV  -> its port (target: the group's name again)
#     TXT  -> StationName=..., StationUUID=...
#
# It does not answer the asker alone, and its records are not laid out as DNS service discovery
# lays them out (the details under the instance's own name, the address under a host's), so a
# general browser does not find it. Stations are therefore read packet by packet: one answer is one
# station, at the address it came from.

def _label(station):
    """A station's name as one piece of a service name: no dots, no spaces."""
    kept = "".join(ch if ch.isalnum() or ch in "-_" else "-" for ch in station.strip())
    return (kept.strip("-") or "station")[:60]


def stations_in(packets):
    """The stations that the answers `mdns.hear` collected tell of: [{"group", "name", "uuid",
    "port", "addresses"}], the address an answer came from first. A station without a port, or
    one that does not say which it is, is left out."""
    service = SERVICE.rstrip(".").casefold()
    found = {}
    for packet, sender in packets:
        records = packet["records"]
        for owner, kind, _, value in records:
            if kind != mdns.SRV or not owner.endswith("." + service):
                continue
            port = value[0]
            group = owner[:-len(service) - 1].rsplit(".", 1)[-1]    # "<group>", or "<station>.<group>" from a station that follows DNS-SD
            texts = [text for name, what, _, text in records if what == mdns.TXT and name == owner] or [text for _, what, _, text in records if what == mdns.TXT]
            text = texts[0] if texts else {}
            their = str(text.get("StationUUID", "")).strip()
            if not their or not port or not group:
                continue
            # The group as its station writes it: the pointer to the station spells it with its capitals.
            for name, what, _, target in records:
                if what == mdns.PTR and isinstance(target, str) and target.casefold().endswith("." + group + "." + service):
                    group = target[:len(target) - len(service) - 1].rsplit(".", 1)[-1]
                    break
            entry = found.setdefault(their.casefold(), {"group": group, "name": str(text.get("StationName", "")), "uuid": their, "port": port, "addresses": []})
            entry["port"] = port
            said = [address for _, what, _, address in records if what == mdns.A]
            for address in [sender, *said]:
                if address and address not in entry["addresses"] and not address.startswith("169.254."):
                    entry["addresses"].append(address)
    return sorted(found.values(), key=lambda entry: (entry["group"].casefold(), entry["name"].casefold(), entry["uuid"]))


def find_stations(seconds=1.5, addresses=None, groups=(), group=(mdns.GROUP, mdns.PORT)):
    """The MVR-xchange stations on the network now, of every group. `groups` are group names to
    ask for by name as well, which is the question grandMA3 itself asks."""
    questions = [(SERVICE, mdns.PTR)] + [(f"{name}.{SERVICE}", mdns.PTR) for name in groups if name]
    return stations_in(mdns.hear(questions, seconds, addresses, group))


class Announcer(mdns.Responder):
    """Says that a station is here, the way grandMA3 says it (see above): asked for the service or
    for the group's name under it, it answers with its four records, all under the group's name."""

    def __init__(self, group_name, station_name, station_uuid, port, addresses, group=(mdns.GROUP, mdns.PORT), bind_port=None):
        super().__init__(group_name, SERVICE, port, {"StationName": station_name, "StationUUID": station_uuid}, addresses, "unused.local.", group=group, bind_port=bind_port)
        self.owner = f"{group_name}.{SERVICE.rstrip('.')}"
        self.station = f"{_label(station_name)}.{self.owner}"

    def wants(self, packet):
        ours = {self.service, self.owner.casefold(), self.station.casefold()}
        return any(name in ours and kind in (mdns.PTR, mdns.SRV, mdns.TXT, mdns.A, mdns.ANY) for name, kind, _ in packet["questions"])

    def records(self, address, ttl=60):
        return [mdns.record(self.owner, mdns.PTR, mdns.encode_name(self.station), ttl),
                mdns.record(self.owner, mdns.A, socket.inet_aton(address), ttl),
                mdns.record(self.owner, mdns.SRV, struct.pack(">HHH", 0, 0, self.port) + mdns.encode_name(self.owner), ttl),
                mdns.record(self.owner, mdns.TXT, mdns.text_data(self.text), ttl)]


class Station:
    """This program as a member of one MVR-xchange group.

    `folder` is where fetched files are put. `discover` returns the stations on the network as
    `find_stations` does; it is given for tests, and otherwise the network is asked. `announce`
    false keeps it off multicast DNS (tests again)."""

    def __init__(self, name, station_uuid, group, folder, provider="Openviz", addresses=None, discover=None, announce=True):
        self.name = name
        self.uuid = station_uuid
        self.group = group
        self.folder = folder
        self.provider = provider
        self.addresses = list(addresses or ["127.0.0.1"])
        self.discover = discover or (lambda: find_stations(1.2, self.addresses, [self.group]))
        self.announce = announce
        self.lock = threading.RLock()
        self.members = {}     # station id, lower case -> {"uuid", "name", "provider", "addresses", "port", "joined"}
        self.groups = {}      # every group heard of on the network -> how many stations (ours not counted)
        self.offers = {}      # file id, lower case -> {"uuid", "file", "size", "comment", "station", "from", "mine", "path", "at"}
        self.port = 0
        self.server = None
        self.responder = None
        self.running = False
        self.threads = []
        self.trouble = ""

    # ----------------------------------------------------------------------------- lifetime

    def start(self):
        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server.bind(("", 0))
        self.server.listen(16)
        self.server.settimeout(0.3)
        self.port = self.server.getsockname()[1]
        self.running = True
        if self.announce:
            try:
                self.responder = Announcer(self.group, self.name, self.uuid, self.port, self.addresses)
                self.responder.start()
            except OSError as error:
                self.responder = None
                self.trouble = f"Other stations cannot find this one: {error}"
        for target in (self._accept, self._keep_up):
            thread = threading.Thread(target=target, daemon=True)
            thread.start()
            self.threads.append(thread)

    def stop(self):
        if not self.running:
            return
        with self.lock:
            members = list(self.members.values())
        for member in members:
            self._ask(member, message("MVR_LEAVE", FromStationUUID=self.uuid), connect=0.5, wait=1.0)
        self.running = False
        if self.responder:
            self.responder.stop()
        for thread in self.threads:
            thread.join(2)
        self.server.close()

    def _keep_up(self):
        """Looks round the network now and then: stations come and go."""
        next_look = 0.0
        while self.running:
            if time.time() >= next_look:
                try:
                    self.refresh()
                except Exception as error:    # noqa: BLE001 (a look that fails is tried again)
                    self.trouble = f"The network could not be asked: {error}"
                next_look = time.time() + 20.0
            time.sleep(0.2)

    # ---------------------------------------------------------------------------- the others

    def refresh(self):
        """Asks the network who is there, and joins every station of this group not joined yet."""
        found = self.discover()
        groups, present = {}, {}
        for entry in found:
            their = entry["uuid"]
            if same(their, self.uuid) or not their:
                continue
            # One spelling for a group, however its stations capitalise it: ours if it is ours.
            spelled = self.group if in_group(entry["group"], self.group) else next((name for name in groups if in_group(entry["group"], name)), entry["group"])
            groups[spelled] = groups.get(spelled, 0) + 1
            if in_group(entry["group"], self.group):
                present[their.casefold()] = entry
        with self.lock:
            self.groups = groups
            for key, entry in present.items():
                member = self.members.setdefault(key, self._blank(entry["uuid"]))
                member["name"] = member["name"] or entry["name"]
                member["addresses"] = entry["addresses"] + [address for address in member["addresses"] if address not in entry["addresses"]]
                member["port"] = entry["port"]
                member["missed"] = 0
            for key, member in list(self.members.items()):
                if key not in present:
                    member["missed"] += 1
                    if not member["joined"] or member["missed"] >= 3:    # never joined, or gone without a word for a minute
                        del self.members[key]
            to_join = [member for member in self.members.values() if not member["joined"] and member["port"]]
        for member in to_join:
            self._join(member)
        self._tell()

    @staticmethod
    def _blank(their_uuid):
        """A station just heard of. `told` are the files of ours it has been told about; `missed` counts the looks round the network it was not seen in."""
        return {"uuid": their_uuid, "name": "", "provider": "", "joined": False, "addresses": [], "port": 0, "told": set(), "missed": 0}

    def _commit_fields(self, offer):
        return {"verMajor": MVR_VERSION[0], "verMinor": MVR_VERSION[1], "FileSize": offer["size"], "FileUUID": offer["uuid"], "StationUUID": offer["station"],
                "ForStationsUUID": [], "Comment": offer["comment"], "FileName": offer["file"]}

    def _about_me(self, member=None):
        """Who this station is and what it has to offer. Whoever is told this has been told of those files."""
        with self.lock:
            mine = [offer for offer in self.offers.values() if offer["mine"]]
            if member is not None:
                member["told"].update(offer["uuid"] for offer in mine)
        return {"Provider": self.provider, "StationName": self.name, "StationUUID": self.uuid, "verMajor": MVR_VERSION[0], "verMinor": MVR_VERSION[1],
                "Commits": [dict({"Type": "MVR_COMMIT"}, **self._commit_fields(offer)) for offer in mine]}

    def _tell(self):
        """Tells every station that can be reached of each file of ours it has not been told of. Returns the names of those told now."""
        with self.lock:
            due = [(member, offer) for member in self.members.values() if member["port"] and member["joined"]
                   for offer in self.offers.values() if offer["mine"] and offer["uuid"] not in member["told"]]
        told = []

        def tell(member, offer):
            answer = self._ask(member, message("MVR_COMMIT", **self._commit_fields(offer)))
            if answer and answer[0] == JSON_KIND and yes(answer[1].get("OK", True)):
                with self.lock:
                    member["told"].add(offer["uuid"])
                told.append(member["name"] or member["uuid"])
        threads = [threading.Thread(target=tell, args=entry, daemon=True) for entry in due]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(15)
        return sorted(set(told))

    def _join(self, member):
        answer = self._ask(member, message("MVR_JOIN", **self._about_me(member)))
        if not answer or answer[0] != JSON_KIND or not yes(answer[1].get("OK", True)):
            with self.lock:
                member["told"].clear()    # it did not hear us after all
            return False
        with self.lock:
            member["joined"] = True
            member["name"] = answer[1].get("StationName") or member["name"]
            member["provider"] = answer[1].get("Provider") or member["provider"]
        self._note_commits(answer[1].get("Commits"), member)
        return True

    def _note_commits(self, commits, member):
        """Remembers the files a station says it has."""
        for commit in commits if isinstance(commits, list) else []:
            if not isinstance(commit, dict) or not isinstance(commit.get("FileUUID"), str) or not commit["FileUUID"].strip():
                continue
            key = commit["FileUUID"].strip().casefold()
            with self.lock:
                if key in self.offers:
                    continue
                self.offers[key] = {"uuid": commit["FileUUID"].strip(), "file": os.path.basename(str(commit.get("FileName") or "")), "size": whole(commit.get("FileSize")),
                                    "comment": str(commit.get("Comment") or ""), "station": member["uuid"], "from": member["name"] or member["provider"] or "a station",
                                    "mine": False, "path": "", "at": time.time()}

    def _ask(self, member, data, into=None, connect=1.5, wait=10.0):
        """Sends one packet to a station and reads its answer; None if it cannot be reached or answers nonsense."""
        for address in list(member["addresses"]):
            try:
                with socket.create_connection((address, member["port"]), timeout=connect) as sock:
                    sock.settimeout(wait)
                    sock.sendall(data)
                    answer = receive(sock, into)
                with self.lock:    # this address works: first next time
                    member["addresses"].remove(address)
                    member["addresses"].insert(0, address)
                return answer
            except (OSError, Refused, ValueError):
                continue
        return None

    # ------------------------------------------------------------------------------ incoming

    def _accept(self):
        while self.running:
            try:
                conn, peer = self.server.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            threading.Thread(target=self._serve, args=(conn, peer), daemon=True).start()

    def _serve(self, conn, peer):
        with conn:
            conn.settimeout(10.0)
            try:
                kind, said = receive(conn)
                reply = self._answer(said, peer[0]) if kind == JSON_KIND else message("MVR_REQUEST_RET", OK=False, Message="A file was sent that nobody asked for")
                if isinstance(reply, str):    # a file to send
                    size = os.path.getsize(reply)
                    conn.settimeout(120.0)
                    conn.sendall(HEADER.pack(MAGIC, VERSION, 0, 1, FILE_KIND, size))
                    with open(reply, "rb") as handle:
                        while True:
                            chunk = handle.read(1 << 20)
                            if not chunk:
                                break
                            conn.sendall(chunk)
                else:
                    conn.sendall(reply)
            except (OSError, Refused):
                pass

    def _member(self, their_uuid, address, name="", provider=""):
        key = their_uuid.strip().casefold()
        with self.lock:
            member = self.members.setdefault(key, self._blank(their_uuid.strip()))
            member["name"] = name or member["name"]
            member["provider"] = provider or member["provider"]
            if address not in member["addresses"]:
                member["addresses"].insert(0, address)
            return member

    def _answer(self, said, address):
        kind = said["Type"]
        if kind == "MVR_JOIN":
            their = said.get("StationUUID")
            if not isinstance(their, str) or not their.strip():
                return message("MVR_JOIN_RET", OK=False, Message="A station has to say who it is", **self._about_me())
            member = self._member(their, address, str(said.get("StationName") or ""), str(said.get("Provider") or ""))
            with self.lock:
                member["joined"] = True
                known = bool(member["port"])
            self._note_commits(said.get("Commits"), member)
            if not known:    # it found us before we found it: look for where it listens
                threading.Thread(target=self._look_again, daemon=True).start()
            return message("MVR_JOIN_RET", OK=True, Message="", **self._about_me(member))
        if kind == "MVR_LEAVE":
            their = said.get("FromStationUUID") or said.get("StationUUID")
            with self.lock:
                if isinstance(their, str):
                    self.members.pop(their.strip().casefold(), None)
            return message("MVR_LEAVE_RET", OK=True, Message="")
        if kind == "MVR_COMMIT":
            their = said.get("StationUUID")
            if not isinstance(their, str) or not their.strip() or not isinstance(said.get("FileUUID"), str) or not said["FileUUID"].strip():
                return message("MVR_COMMIT_RET", OK=False, Message="A commit names its file and its station")
            wanted = said.get("ForStationsUUID")
            if isinstance(wanted, list) and wanted and not any(same(entry, self.uuid) for entry in wanted):
                return message("MVR_COMMIT_RET", OK=True, Message="")    # meant for others
            with self.lock:
                self.offers.pop(said["FileUUID"].strip().casefold(), None)    # said again: as it is now
            self._note_commits([said], self._member(their, address))
            return message("MVR_COMMIT_RET", OK=True, Message="")
        if kind == "MVR_REQUEST":
            wanted = said.get("FileUUID")
            with self.lock:
                mine = sorted((offer for offer in self.offers.values() if offer["path"]), key=lambda offer: offer["at"])
                if isinstance(wanted, str) and wanted.strip() and wanted.strip("0-") != "":
                    mine = [offer for offer in mine if same(offer["uuid"], wanted)]
                else:
                    mine = [offer for offer in mine if offer["mine"]]    # no file named: the last one sent from here
            if mine and os.path.isfile(mine[-1]["path"]):
                return mine[-1]["path"]
            return message("MVR_REQUEST_RET", OK=False, Message="The MVR is not available on this client")
        if kind.endswith("_RET"):
            return message("MVR_REQUEST_RET", OK=False, Message="An answer arrived where a question was expected")
        return packet(json.dumps({"Type": kind + "_RET", "OK": False, "Message": "This station does not know that message"}).encode("utf-8"))

    def _look_again(self):
        try:
            self.refresh()
        except Exception:    # noqa: BLE001
            pass

    # ------------------------------------------------------------------------------ outgoing

    def commit(self, path, comment="", file_name=None):
        """Offers a file to the group. Returns the offer and the names of the stations told of it at once."""
        offer = {"uuid": str(uuid.uuid4()), "file": file_name or os.path.basename(path), "size": os.path.getsize(path), "comment": comment, "station": self.uuid,
                 "from": self.name, "mine": True, "path": path, "at": time.time()}
        with self.lock:
            self.offers[offer["uuid"].casefold()] = offer
        # Stations whose whereabouts are not known yet are told as soon as they are (see refresh).
        return dict(offer, told=self._tell())

    def fetch(self, file_uuid):
        """Fetches a file a station offered. Returns the path it was saved to."""
        with self.lock:
            offer = self.offers.get(str(file_uuid).strip().casefold())
            if not offer:
                raise Refused("No station has offered that file.")
            if offer["path"] and os.path.isfile(offer["path"]):
                return offer["path"]
            # The station that offered it first, then the others: one of them may have fetched it too.
            order = sorted((member for member in self.members.values() if member["port"]), key=lambda member: not same(member["uuid"], offer["station"]))
        if not order:
            raise Refused("The station that offered it is not on the network any more.")
        os.makedirs(self.folder, exist_ok=True)
        name = offer["file"] if offer["file"].lower().endswith(".mvr") else (offer["file"] or f"From {offer['from']}") + ".mvr"
        name = "".join(ch if ch.isalnum() or ch in " ._-()" else "_" for ch in name).strip(" .") or "rig.mvr"
        partial = os.path.join(self.folder, f".{offer['uuid']}.part")
        said = "it did not answer"
        try:
            for member in order:
                answer = self._ask(member, message("MVR_REQUEST", FileUUID=offer["uuid"], FromStationUUID=[member["uuid"]]), into=partial, wait=120.0)
                if not answer:
                    continue
                if answer[0] == JSON_KIND:
                    said = str(answer[1].get("Message") or "it said no")
                    continue
                with open(partial, "rb") as handle:
                    if handle.read(2) != b"PK":
                        said = "what it sent is not an MVR file"
                        continue
                # A folder of its own: two offers may carry the same file name, and each stays what it was.
                target = os.path.join(self.folder, offer["uuid"].lower(), name)
                os.makedirs(os.path.dirname(target), exist_ok=True)
                os.replace(partial, target)
                with self.lock:
                    offer["path"] = target
                return target
        finally:
            if os.path.exists(partial):
                os.remove(partial)
        raise Refused(f"{offer['file'] or 'The file'} could not be fetched from {offer['from']}: {said}.")

    def state(self):
        with self.lock:
            return {"group": self.group, "name": self.name, "uuid": self.uuid, "port": self.port, "trouble": self.trouble,
                    "stations": sorted(({"name": member["name"] or member["provider"] or member["uuid"], "provider": member["provider"], "uuid": member["uuid"],
                                         "joined": member["joined"], "address": (member["addresses"] or [""])[0]} for member in self.members.values()),
                                       key=lambda entry: entry["name"].casefold()),
                    "groups": dict(self.groups),
                    "offers": sorted(({"uuid": offer["uuid"], "file": offer["file"], "size": offer["size"], "comment": offer["comment"], "from": offer["from"],
                                       "mine": offer["mine"], "have": bool(offer["path"]), "at": int(offer["at"])} for offer in self.offers.values()),
                                     key=lambda entry: -entry["at"])}
