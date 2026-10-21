#!/usr/bin/env python3
"""Openviz's launcher: one window to collect shows and fixtures, set things up and open the visualizer.

  launcher/launcher.py            start it and open its page in the browser
  launcher/launcher.py --no-open  only print the address (the Mac app does this and shows the page itself)

It is a small web server that listens on this computer only. The page it serves is the interface;
this file does the work behind it:

  - keeps dropped MVR files in library/shows/ and GDTF files in library/fixtures/
  - searches and downloads from GDTF Share (tools/gdtf_share), with your own login
  - remembers settings in library/settings.json
  - starts Unreal on a show, or on a row of one fixture type, and reports what it loaded

Every request must carry the token the launcher made at startup, so no other page or program on
this computer can drive it. The GDTF Share password goes straight to gdtf-share.com. It is kept
only if "Stay logged in" is ticked, and then in the macOS Keychain, never in a file here.
"""
from __future__ import annotations

import argparse
import contextlib
import csv
import hmac
import http.server
import ipaddress
import json
import math
import os
import re
import secrets
import select
import shutil
import signal
import socket
import subprocess
import uuid
import sys
import tempfile
import threading
import time
import urllib.parse
import webbrowser
import zipfile
import xml.etree.ElementTree as ET

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools", "gdtf_share"))
sys.path.insert(0, os.path.join(REPO, "tools", "dmx_sender"))

import gdtf_share  # noqa: E402
import verify_capture  # noqa: E402    (its Art-Net and sACN decoders read what arrives for the DMX check)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import dmx_record  # noqa: E402
import fixture_builder  # noqa: E402
import mvr_xchange  # noqa: E402
import osc  # noqa: E402
import psn  # noqa: E402
import ndi_find  # noqa: E402
import package  # noqa: E402
import oslayer  # noqa: E402
import paperwork  # noqa: E402

WEB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web")
RELAY = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dmx_relay.py")
PROJECT = os.path.join(REPO, "unreal", "LightingViz.uproject")
PLUGIN_BINARIES = os.path.join(REPO, "unreal", "Plugins", "LightingVisualizer", "Binaries")
DEFAULT_LIBRARY = os.path.join(REPO, "library")
PARITY_TRACKER = os.path.join(REPO, "docs", "parity-tracker.md")
DEFAULT_ENGINE = oslayer.default_engine()
MAX_UPLOAD = 4 << 30

# Where grandMA3 onPC writes MVR exports (2.5: gma3_library/mvr; 2.0 to 2.3: lib_mvr in its own
# folder, which also holds the demo rig it ships with).
HOME = os.path.expanduser("~")
EXPORT_FOLDERS = [("grandMA3", os.path.join(HOME, "MALightingTechnology", "gma3_library", "mvr"))]
try:
    for _name in sorted(os.listdir(os.path.join(HOME, "MALightingTechnology")), reverse=True):
        if _name.startswith("gma3_") and _name != "gma3_library":
            EXPORT_FOLDERS.append(("grandMA3", os.path.join(HOME, "MALightingTechnology", _name, "shared", "resource", "lib_mvr")))
except OSError:
    pass

DEFAULTS = {
    "engine": DEFAULT_ENGINE,
    "protocol": "artnet",          # artnet or sacn
    "dmx_address": "127.0.0.1",    # the network address Unreal listens on
    "window": "windowed",          # windowed or fullscreen
    "resolution": "1600x900",
    "megalights": True,
    "shadows": False,
    "exposure": -8.0,              # stops; photometric fixtures need about -8
    "haze": 0.06,                  # fog density
    "haze_drift": 0.4,             # how unevenly the haze hangs in the beams: 0 even, 1 thinning to nothing in places
    "house_lights": 0.0,           # 0 to 1: an even light on everything, off by default
    "beams": "cone",               # cone: each beam drawn as its own shaft; fog: beams only light the haze
    "beam_detail": 100,            # percent of the picture's resolution that beams in haze are drawn at: 100, 75 or 50
    "beam_shadows": True,          # what stands in a beam cuts the part of it behind
    "adapt_beams": True,           # draw beams in haze at less detail by itself while the picture runs below 30 frames a second
    "rig_haze": True,              # fog and haze machines in the rig, driven by DMX, add to the haze
    "floor": 0.8,                  # how rough the stage floor is: 0 a mirror, 1 matte
    "floor_shade": 0.14,           # its colour, 0 black to 1 white
    "bloom": 0.675,                # how far bright parts glow into what is around them (the engine's bloom; 0.675 its own)
    "motion_blur": True,           # what moves fast is blurred along its path
    "surface_colour": False,       # coloured gobos and split colours land in colour on surfaces (the engine's colour light-function atlas)
    "idle_look": True,             # light fixtures at their defaults until DMX arrives
    "preview_count": 6,
    "mouse_look": "hold",          # hold: the view turns while the right button is held; always: game style
    "look_speed": 1.0,             # 0.2 to 3
    "move_speed": 6.0,             # metres a second
    "invert_y": False,
    "share_user": "",              # set when "Stay logged in" is on; the password is in the Keychain
    "plot_paper": "auto",          # the sheet a plot is drawn on (paperwork.PAPERS); auto: by this computer's region
    "plot_units": "auto",          # metric, imperial, or auto likewise
    "xchange": False,              # take part in MVR-xchange: rigs to and from the console over the network
    "xchange_group": "",           # the group to be in; stations of one group see each other's files
    "xchange_name": "",            # what other stations call this one; empty: "Openviz on <this computer>"
    "xchange_uuid": "",            # this station's id, made once and kept
    "osc": False,                  # listen for OSC from a console or a control surface (views, pictures, exposure, haze)
    "osc_port": 8000,
    "psn": False,                  # hear PosiStageNet: set pieces follow the trackers they are given
    "psn_group": "236.10.10.10",   # the multicast group it comes to; empty: sent to this computer's own address
    "psn_port": 56565,
}

# What to tell someone who drops a file this cannot read. The applications' own formats are
# closed; MVR is the open format all of them export.
NOT_READABLE = {
    ".show": ("grandMA3 show file",
              "grandMA3 keeps shows in its own closed format. Export the rig as MVR instead: in grandMA3 open "
              "Menu, then Patch, press Export at the bottom, leave the file type on MVR and press Export. "
              "The .mvr file is written to the gma3_library/mvr folder; drop it here."),
    ".vwx": ("Vectorworks document",
             "Vectorworks documents are a closed format. Export the rig as MVR instead: in Vectorworks Spotlight "
             "choose File, Export, Export MVR, and drop the .mvr file here."),
    ".c2p": ("Capture project",
             "Capture projects are a closed format. Export the rig as MVR instead: in Capture choose File, "
             "Export, then MVR, and drop the .mvr file here."),
}


class Problem(Exception):
    """Something to show the user. `code` lets the page react, e.g. by asking for a login."""

    def __init__(self, message, code="problem", status=400):
        super().__init__(message)
        self.code = code
        self.status = status


def editor_binary(engine):
    return oslayer.editor_binary(engine)


class PdfBytes(bytes):
    """An answer that is a PDF to look at, not JSON."""


class PictureBytes(bytes):
    """An answer that is a picture, not JSON: `content_type` says which kind."""
    content_type = "image/png"


MODEL_ENDINGS = (".glb", ".3ds", ".obj")


def model_companions(path):
    """The files an OBJ names beside itself: its MTL file, and the pictures that names. Only plain
    names in the OBJ's own folder are taken."""
    if not path.lower().endswith(".obj"):
        return []
    folder = os.path.dirname(path)
    found = []

    def beside(name):
        name = name.strip().replace("\\", "/")
        candidate = os.path.join(folder, os.path.basename(name))
        return candidate if name and os.path.isfile(candidate) else None
    try:
        with open(path, encoding="utf-8", errors="replace") as handle:
            libraries = [line.split(None, 1)[1] for line in handle if line.startswith("mtllib ") and len(line.split(None, 1)) == 2]
    except OSError:
        return []
    for library in libraries[:1]:
        mtl = beside(library)
        if not mtl:
            continue
        found.append(mtl)
        with open(mtl, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                words = line.split()
                if words and words[0].startswith("map_") and len(words) >= 2:
                    picture = beside(words[-1])
                    if picture and picture not in found:
                        found.append(picture)
    return found


def safe_name(name, extension):
    """A plain file name with the given extension: no folders, nothing hidden, nothing odd."""
    stem = os.path.basename(str(name).replace("\\", "/"))
    if stem.lower().endswith(extension):
        stem = stem[:-len(extension)]
    stem = re.sub(r'[\\/:*?"<>|\x00-\x1f\x7f]', "_", stem).strip(" .")
    stem = stem.encode("utf-8")[:180].decode("utf-8", "ignore") or "untitled"
    return stem + extension


def local_addresses():
    """This computer's IPv4 addresses, for the "listen on" choice. Loopback first."""
    return oslayer.local_addresses()


class Keychain:
    """One password in the macOS Keychain: the GDTF Share login, when "Stay logged in" is on.

    It goes through the system's own `security` tool. The password is handed over on that tool's
    input, not on its command line, where other programs could see it; and as hex, so that no
    character in it can be mistaken for part of the command.
    """
    TOOL = "/usr/bin/security"

    def __init__(self, service="Openviz.GDTF-Share", account="gdtf-share"):
        self.service = service
        self.account = account

    @classmethod
    def available(cls):
        return sys.platform == "darwin" and os.path.exists(cls.TOOL)

    def save(self, password):
        secret = password.encode("utf-8").hex()
        line = f'add-generic-password -U -a {self.account} -s {self.service} -l "Openviz: GDTF Share login" -w {secret}\n'
        try:
            done = subprocess.run([self.TOOL, "-i"], input=line, capture_output=True, text=True, timeout=20)
        except (OSError, subprocess.TimeoutExpired):
            return False
        return done.returncode == 0 and self.load() == password

    def load(self):
        try:
            done = subprocess.run([self.TOOL, "find-generic-password", "-a", self.account, "-s", self.service, "-w"],
                                  capture_output=True, text=True, timeout=20)
            if done.returncode != 0:
                return None
            return bytes.fromhex(done.stdout.strip()).decode("utf-8")
        except (OSError, ValueError, subprocess.TimeoutExpired):
            return None

    def forget(self):
        try:
            subprocess.run([self.TOOL, "delete-generic-password", "-a", self.account, "-s", self.service],
                           capture_output=True, timeout=20)
        except (OSError, subprocess.TimeoutExpired):
            pass


def openviz_version():
    """What Openviz calls its version: the release an installer wrote down (openviz-version.txt), else 0.1 beta."""
    try:
        with open(os.path.join(REPO, "openviz-version.txt"), encoding="utf-8") as handle:
            return handle.read().strip().replace("-", " ") or "0.1 beta"
    except OSError:
        return "0.1 beta"


def system_keychain():
    """Where this system keeps passwords for "Stay logged in": the macOS Keychain, or Windows'
    Credential Manager. None elsewhere: then nothing is remembered."""
    if Keychain.available():
        return Keychain()
    if oslayer.WindowsCredentials.available():
        return oslayer.WindowsCredentials()
    return None


class Launcher:
    """Everything the page can ask for. No HTTP in here, so the tests call it directly too."""

    def __init__(self, library=DEFAULT_LIBRARY, share_url=gdtf_share.DEFAULT_URL, inspector=gdtf_share.INSPECTOR,
                 project=PROJECT, export_folders=None, keychain="system", relay=False, xchange=False, osc_allowed=False, build_when_stale=False):
        # relay: receive the console's DMX here and pass it to Unreal (dmx_relay.py), which is what
        # makes the live monitor and recording during a show possible. Off unless asked for, so
        # that nothing takes the DMX ports just because a Launcher object exists; main() asks.
        self.relay_enabled = bool(relay)
        self._relay = None         # {"process", "config", "started"} while the relay runs
        self._relay_error = ""
        self._control = {}         # the orders last written for the relay
        self._relay_take = None    # the recording the relay is making, if any
        self._relay_take_started = 0.0
        self.library = os.path.abspath(library)
        self.shows_dir = os.path.join(self.library, "shows")
        self.fixtures_dir = os.path.join(self.library, "fixtures")
        self.logs_dir = os.path.join(self.library, "logs")
        self.exports_dir = os.path.join(self.library, "exports")
        self.pictures_dir = os.path.join(self.library, "pictures")
        self.recordings_dir = os.path.join(self.library, "recordings")
        self.videos_dir = os.path.join(self.library, "videos")
        self.models_dir = os.path.join(self.library, "models")    # model files for set pieces: GLB, 3DS, OBJ
        self._videos = {}    # a movie the visualizer saved -> {"state", "file", "error"}: made into H.264 here
        self._panoramas = {}    # the six views of a 360-degree picture -> {"state", "file", "error"}: stitched here
        self._monitor_path = os.path.join(self.logs_dir, "dmx-monitor.json")    # written by the relay
        self._control_path = os.path.join(self.logs_dir, "dmx-control.json")    # read by it
        self._picture_path = os.path.join(self.logs_dir, "picture.json")        # the picture settings, read by a visualizer that is open
        self._commands_path = os.path.join(self.logs_dir, "visualizer-commands.json")    # a view or a picture asked of a visualizer that is open
        self._command_serial = int(time.time() * 1000) % 1_000_000_000
        self._region = None        # (measures in inches, paper is Letter) for this computer's region; asked when first needed
        self._xchange_allowed = xchange    # whether this process goes on the network for MVR-xchange at all (tests do not)
        self._station = None       # the mvr_xchange.Station while exchange is on
        self._build = None                  # the visualizer being built: see build_visualizer
        self.build_when_stale = build_when_stale    # whether a visualizer whose code changed is built before it starts (only main() asks)
        self._osc_allowed = osc_allowed    # whether this process listens for OSC at all (tests do not, unless they start it themselves)
        self._osc = None                   # the osc.Listener while it is on
        self._osc_trouble = ""
        self._osc_heard = []               # the last messages, newest last, with what came of each
        self.received_dir = os.path.join(self.library, "received")
        self._recorder = None      # a dmx_record.Recorder while recording
        self._player = None        # a dmx_record.Player while playing back, with the name of what it plays
        self._playing = ""
        self._takes = {}           # path -> (mtime, size, summary)
        for folder in (self.shows_dir, self.fixtures_dir, self.logs_dir):
            os.makedirs(folder, exist_ok=True)
        self.share_url = share_url
        self.inspector = inspector
        self.project = project
        self.export_folders = EXPORT_FOLDERS if export_folders is None else export_folders
        # Where a remembered password lives; None where there is no keychain to keep it in.
        self.keychain = system_keychain() if keychain == "system" else keychain
        self._lock = threading.RLock()
        self._share = None           # a logged-in gdtf_share.Share, or None
        self._summaries = {}         # path -> (mtime, size, summary)
        self._list_cache = None      # GDTF Share's list as last read from disk
        self.relay_options = []      # extra arguments for the sACN relay; the tests point it at their own ports
        self.psn_to_port = psn.VISUALIZER_PORT    # where the visualizer hears the PosiStageNet the relay passes on
        self.code_files = [os.path.abspath(__file__), RELAY, gdtf_share.__file__, paperwork.__file__, paperwork.pdf.__file__, ndi_find.__file__, oslayer.__file__, mvr_xchange.__file__, mvr_xchange.mdns.__file__]
        self._code_stamp = self._code_changed_at()    # to notice being out of date: see state()
        self._run = None             # the Unreal process started last, and what it was started on

    # ------------------------------------------------------------------------------- settings

    @property
    def _settings_path(self):
        return os.path.join(self.library, "settings.json")

    def settings(self):
        values = dict(DEFAULTS)
        try:
            with open(self._settings_path, encoding="utf-8") as handle:
                saved = json.load(handle)
        except (OSError, ValueError):
            saved = {}
        if isinstance(saved, dict):
            for key, value in saved.items():
                try:
                    values.update(self._checked({key: value}))
                except Problem:
                    pass    # a value this version no longer accepts falls back to its default
        return values

    @staticmethod
    def _checked(changes):
        if not isinstance(changes, dict):
            raise Problem("Settings must be given as names and values.")
        out = {}
        for key, value in changes.items():
            if key not in DEFAULTS:
                raise Problem(f"There is no setting named {key}.")
            if key in ("megalights", "shadows", "beam_shadows", "adapt_beams", "rig_haze", "idle_look", "invert_y", "motion_blur", "surface_colour"):
                if not isinstance(value, bool):
                    raise Problem(f"{key} must be on or off.")
            elif key == "protocol":
                if value not in ("artnet", "sacn"):
                    raise Problem("The protocol must be Art-Net (artnet) or sACN (sacn).")
            elif key == "window":
                if value not in ("windowed", "fullscreen"):
                    raise Problem("The window mode must be windowed or fullscreen.")
            elif key == "resolution":
                found = re.fullmatch(r"(\d{3,5})x(\d{3,5})", str(value))
                if not found or not (640 <= int(found.group(1)) <= 16384 and 360 <= int(found.group(2)) <= 16384):
                    raise Problem("Enter the resolution as width x height, for example 1920x1080.")
            elif key == "dmx_address":
                try:
                    value = str(ipaddress.IPv4Address(str(value).strip()))
                except ValueError:
                    raise Problem("Enter a network address such as 192.168.1.20.") from None
            elif key == "exposure":
                if isinstance(value, bool) or not isinstance(value, (int, float)) or not -16 <= value <= 8:
                    raise Problem("Exposure must be between -16 and +8 EV.")
                value = float(value)
            elif key == "haze_drift":
                if isinstance(value, bool) or not isinstance(value, (int, float)) or not 0 <= value <= 1:
                    raise Problem("Haze texture must be between 0 and 1.")
                value = float(value)
            elif key == "haze":
                if isinstance(value, bool) or not isinstance(value, (int, float)) or not 0 <= value <= 0.5:
                    raise Problem("Haze must be between 0 and 0.5.")
                value = float(value)
            elif key in ("floor", "floor_shade"):
                if isinstance(value, bool) or not isinstance(value, (int, float)) or not 0 <= value <= 1:
                    raise Problem("Floor finish and colour must be between 0 and 1.")
                value = float(value)
            elif key == "bloom":
                if isinstance(value, bool) or not isinstance(value, (int, float)) or not 0 <= value <= 4:
                    raise Problem("Glow must be between 0 and 4.")
                value = float(value)
            elif key == "house_lights":
                if isinstance(value, bool) or not isinstance(value, (int, float)) or not 0 <= value <= 1:
                    raise Problem("Work light must be between 0 and 1.")
                value = float(value)
            elif key == "beams":
                if value not in ("cone", "fog"):
                    raise Problem("Beam rendering must be cone (volumetric) or fog.")
            elif key == "beam_detail":
                if isinstance(value, bool) or value not in (100, 75, 50):
                    raise Problem("Beam resolution must be 100, 75 or 50 percent.")
                value = int(value)
            elif key == "mouse_look":
                if value not in ("hold", "always"):
                    raise Problem("Mouse look must be hold or always.")
            elif key == "look_speed":
                if isinstance(value, bool) or not isinstance(value, (int, float)) or not 0.2 <= value <= 3:
                    raise Problem("Look sensitivity must be between 0.2 and 3.")
                value = float(value)
            elif key == "move_speed":
                if isinstance(value, bool) or not isinstance(value, (int, float)) or not 0.5 <= value <= 50:
                    raise Problem("Movement speed must be between 0.5 and 50 m/s.")
                value = float(value)
            elif key in ("xchange", "osc", "psn"):
                if not isinstance(value, bool):
                    raise Problem(f"{ {'xchange': 'MVR-xchange', 'osc': 'OSC', 'psn': 'PosiStageNet'}[key]} must be on or off.")
            elif key in ("osc_port", "psn_port"):
                if isinstance(value, bool) or not isinstance(value, int) or not 1024 <= value <= 65535:
                    raise Problem(f"The {'OSC' if key == 'osc_port' else 'PosiStageNet'} port must be a number from 1024 to 65535.")
            elif key == "psn_group":
                # A multicast group (224.0.0.0 to 239.255.255.255), or empty for what is sent to this computer's own address.
                parts = value.split(".") if isinstance(value, str) else []
                if value != "" and not (len(parts) == 4 and all(part.isdigit() and int(part) <= 255 for part in parts) and 224 <= int(parts[0]) <= 239):
                    raise Problem("The PosiStageNet group must be a multicast address such as 236.10.10.10, or empty.")
            elif key == "xchange_group":
                # It becomes one piece of a name on the network: no dots, nothing that is not plain.
                if not isinstance(value, str) or len(value) > 60 or re.search(r"[^A-Za-z0-9 _-]", value) or value != value.strip():
                    raise Problem("Group names may contain letters, digits, spaces, hyphens and underscores.")
            elif key == "xchange_name":
                if not isinstance(value, str) or len(value) > 80 or any(ord(ch) < 32 for ch in value):
                    raise Problem("That is not a valid station name.")
                value = value.strip()
            elif key == "xchange_uuid":
                if not isinstance(value, str) or (value and not re.fullmatch(r"[0-9A-Fa-f]{8}(-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}", value)):
                    raise Problem("That is not a valid station ID.")
            elif key == "plot_paper":
                if value != "auto" and value not in paperwork.PAPERS:
                    raise Problem("That is not a supported paper size.")
            elif key == "plot_units":
                if value != "auto" and value not in paperwork.UNITS:
                    raise Problem("Units must be metric or imperial.")
            elif key == "share_user":
                if not isinstance(value, str) or len(value) > 200 or any(ord(ch) < 32 for ch in value):
                    raise Problem("That is not a valid user name.")
            elif key == "preview_count":
                if isinstance(value, bool) or not isinstance(value, int) or not 1 <= value <= 64:
                    raise Problem("The preview fixture count must be between 1 and 64.")
            elif key == "engine":
                if not isinstance(value, str) or not value.strip() or "\x00" in value:
                    raise Problem("The engine setting must be the folder Unreal Engine is installed in.")
                value = value.strip()
            out[key] = value
        return out

    # The settings a visualizer that is open takes up as they change (LvShow, -LvPicture): how the
    # picture looks and how the camera flies. The window and its resolution wait for the next start.
    PICTURE = ("exposure", "haze", "haze_drift", "house_lights", "beams", "beam_detail", "beam_shadows", "adapt_beams", "shadows", "megalights", "rig_haze",
               "floor", "floor_shade", "bloom", "motion_blur", "mouse_look", "look_speed", "move_speed", "invert_y")

    def _write_picture(self, values):
        try:
            with open(self._picture_path + ".part", "w", encoding="utf-8") as handle:
                json.dump({key: values[key] for key in self.PICTURE}, handle)
            os.replace(self._picture_path + ".part", self._picture_path)
        except OSError:
            pass    # the settings themselves are kept; an open visualizer just does not hear of it

    def update_settings(self, changes):
        with self._lock:
            values = self.settings()
            values.update(self._checked(changes))
            with open(self._settings_path + ".part", "w", encoding="utf-8") as handle:
                json.dump(values, handle, indent=2)
            os.replace(self._settings_path + ".part", self._settings_path)
            if any(key in self.PICTURE for key in changes):
                self._write_picture(values)
        if self.relay_enabled and any(key in changes for key in ("protocol", "dmx_address", "psn", "psn_group", "psn_port")):
            self.ensure_relay()    # listen where it now says
        return values

    # ---------------------------------------------------------------------------------- state

    def _code_changed_at(self):
        try:
            return max(os.path.getmtime(path) for path in self.code_files)
        except OSError:
            return 0.0

    def state(self):
        settings = self.settings()
        engine_found = os.path.exists(editor_binary(settings["engine"]))
        built = os.path.isdir(PLUGIN_BINARIES) and any("LvShow" in name for _, _, names in os.walk(PLUGIN_BINARIES) for name in names)
        return {
            "settings": settings,
            "addresses": local_addresses(),
            "engine_found": engine_found,
            "project_built": built,
            "project_stale": self.visualizer_stale() if built else [False, ""],
            "build": self.build_state(),
            "core_built": os.path.exists(self.inspector),
            "build_command": oslayer.build_command(settings["engine"], self.project),
            "share": self.share_state(),
            "run": self.run_state(),
            "relay": self.relay_state(),
            "plot": dict(zip(("paper", "units"), self.plot_choices()),
                         papers=[{"key": key, "label": entry[0]} for key, entry in paperwork.PAPERS.items()]),
            "library": self.library,
            "version": openviz_version(),
            "system": sys.platform,
            "file_manager": oslayer.file_manager(),
            # This process is still running the program as it was when Openviz was opened. If the
            # files have changed since, what the window does and what the docs say no longer match.
            "stale": self._code_changed_at() != self._code_stamp,
        }

    # ------------------------------------------------------------------------ files and shows

    def _summary(self, path):
        """What the core says about a .mvr or .gdtf, cached until the file changes."""
        try:
            stat = os.stat(path)
        except OSError:
            return {"error": "the file is gone"}
        swapped = 0
        if path.lower().endswith(".mvr"):
            # A show's stand-ins and its edits are part of what is said about it: which types are
            # still placeholders, and how many fixtures have no place.
            for beside in (self._types_path(path), self._edits_path(path)):
                try:
                    swapped += os.stat(beside).st_mtime
                except OSError:
                    pass
        cached = self._summaries.get(path)
        if cached and cached[:2] == (stat.st_mtime, (stat.st_size, swapped)):
            return cached[2]
        summary = {"types": [], "missing": [], "modes": [], "layer_list": [], "class_list": [], "bare": [], "standins": {}}
        if not os.path.exists(self.inspector):
            summary["unchecked"] = True
        else:
            try:
                done = subprocess.run([self.inspector, "--summary", path], capture_output=True, text=True,
                                      errors="replace", timeout=120)
                for line in done.stdout.splitlines():
                    parts = line.split("\t")
                    if parts[0] in ("type", "missing") and len(parts) == 3:
                        # A rig may name one fixture type with and without ".gdtf"; it is one type.
                        rows = summary["types" if parts[0] == "type" else "missing"]
                        name = parts[2][:-5] if parts[2].lower().endswith(".gdtf") else parts[2]
                        same = next((row for row in rows if row["name"] == name), None)
                        if same:
                            same["count"] += int(parts[1])
                        else:
                            rows.append({"count": int(parts[1]), "name": name})
                    elif parts[0] in ("layer", "class") and len(parts) == 4:
                        summary[f"{parts[0]}_list"].append({"name": parts[3], "fixtures": int(parts[1]), "pieces": int(parts[2])})
                    elif parts[0] == "mode" and len(parts) == 3:
                        summary["modes"].append({"name": parts[2], "footprint": int(parts[1])})
                    elif parts[0] == "bare" and len(parts) == 5:
                        # A type the file says next to nothing about: a console's placeholder.
                        summary["bare"].append({"spec": parts[4], "name": bare_type(parts[4]), "count": int(parts[1]), "silent": int(parts[2]), "channels": int(parts[3])})
                    elif parts[0] == "standin" and len(parts) == 3:
                        summary["standins"][parts[1]] = parts[2]
                    elif parts[0] in ("fixtures", "universes", "warnings", "unpatched", "unplaced", "without_type", "objects", "objects_with_shape",
                                      "layers", "classes") and len(parts) == 2:
                        summary[parts[0]] = int(parts[1])
                    elif len(parts) == 2:
                        summary[parts[0]] = parts[1].strip()
                if done.returncode != 0 and "error" not in summary:
                    summary["error"] = "the core could not read it"
            except (OSError, ValueError, subprocess.TimeoutExpired) as error:
                summary["error"] = f"the check did not run ({error})"
        self._summaries[path] = (stat.st_mtime, (stat.st_size, swapped), summary)
        return summary

    def _placeholders(self, summary):
        """What still only dims: a show's placeholder types that nothing stands in for, or whose
        stand-in has gone from the library. Looked at afresh each time: the library changes."""
        have = {spec.casefold() for spec, file in summary.get("standins", {}).items() if os.path.isfile(os.path.join(self.fixtures_dir, file))}
        return [row for row in summary.get("bare", []) if row["spec"].casefold() not in have]

    @staticmethod
    def _types_path(mvr_path):
        return mvr_path[:-4] + ".types.xml"    # the fixture files that stand in for types of the show; written and read by the core

    def shows(self):
        rows = []
        for name in sorted(os.listdir(self.shows_dir), key=str.casefold):
            path = os.path.join(self.shows_dir, name)
            if name.lower().endswith(".mvr") and os.path.isfile(path):
                summary = self._summary(path)
                rows.append(dict(summary, name=name, size=os.path.getsize(path), modified=int(os.path.getmtime(path)),
                                 edited=self._edited(path), hidden=self._hidden(path), groups=self._group_rows(path), placeholders=self._placeholders(summary),
                                 pieces=len(self._piece_list(path)), **self.history(name)))
        return rows

    # ---------------------------------------------------------------------------- undo and redo

    # What a show is beside its rig file: what was changed in Openviz and in the Unreal Editor.
    # A change made from this page keeps these as they were before it and after it, so that it can
    # be undone, and redone, in order. Kept in library/history/, so that Openviz can be quit between.
    SIDE_FILES = (".edits.xml", ".types.xml", ".pieces.json", ".screens.json", ".hidden.json", ".paper.json", ".groups.json")
    UNDO_DEPTH = 50

    def _side_files(self, mvr_path):
        found = {}
        for ending in self.SIDE_FILES:
            try:
                with open(mvr_path[:-4] + ending, encoding="utf-8") as handle:
                    found[ending] = handle.read()
            except (OSError, UnicodeDecodeError):
                found[ending] = None
        return found

    def _history_path(self, name):
        return os.path.join(self.library, "history", name[:-4] + ".json")

    def _history(self, name):
        try:
            with open(self._history_path(name), encoding="utf-8") as handle:
                kept = json.load(handle)
            return {"undo": list(kept.get("undo", [])), "redo": list(kept.get("redo", []))}
        except (OSError, ValueError, AttributeError):
            return {"undo": [], "redo": []}

    def _keep_history(self, name, history):
        target = self._history_path(name)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target + ".part", "w", encoding="utf-8") as handle:
            json.dump({"undo": history["undo"][-self.UNDO_DEPTH:], "redo": history["redo"][-self.UNDO_DEPTH:]}, handle)
        os.replace(target + ".part", target)

    def history(self, name):
        """What Undo and Redo would do to a show now: {"undo": what, "redo": what}, None where there is nothing."""
        kept = self._history(name)
        return {"undo": kept["undo"][-1]["what"] if kept["undo"] else None, "redo": kept["redo"][-1]["what"] if kept["redo"] else None}

    @contextlib.contextmanager
    def changing(self, name, what):
        """Around a change to a show made from this page: what its files were before, kept for Undo."""
        try:
            path = self._show_path(name)
        except Problem:
            yield    # not a show: the change says so itself
            return
        before = self._side_files(path)
        yield
        after = self._side_files(path)
        changed = [ending for ending in self.SIDE_FILES if before[ending] != after[ending]]
        if not changed:
            return
        with self._lock:
            kept = self._history(name)
            kept["undo"].append({"what": what, "at": int(time.time()), "before": {ending: before[ending] for ending in changed},
                                 "after": {ending: after[ending] for ending in changed}})
            kept["redo"] = []    # a new change: what was undone before it cannot be redone over it
            self._keep_history(name, kept)

    def _put_side_files(self, path, contents):
        for ending, text in contents.items():
            target = path[:-4] + ending
            if text is None:
                if os.path.exists(target):
                    os.remove(target)
            else:
                with open(target + ".part", "w", encoding="utf-8") as handle:
                    handle.write(text)
                os.replace(target + ".part", target)

    def undo(self, name):
        """Takes back the last change made to a show from this page. A show that is open follows."""
        return self._step_back_or_on(name, "undo")

    def redo(self, name):
        """Makes again the last change undone."""
        return self._step_back_or_on(name, "redo")

    def _step_back_or_on(self, name, which):
        path = self._show_path(name)
        with self._lock:
            kept = self._history(name)
            if not kept[which]:
                raise Problem("There is nothing to undo." if which == "undo" else "There is nothing to redo.")
            entry = kept[which][-1]
            now, then = (entry["after"], entry["before"]) if which == "undo" else (entry["before"], entry["after"])
            # Only over the files as that change left them: what was done since, in the Unreal Editor say, is not thrown away.
            current = self._side_files(path)
            if any(current[ending] != text for ending, text in now.items()):
                raise Problem(f"The show was changed since ({'in the Unreal Editor, or ' if self._run else ''}by hand), so "
                              f"{'undoing' if which == 'undo' else 'redoing'} \"{entry['what']}\" would discard those changes. Revert them manually instead.", code="changed_since")
            self._put_side_files(path, then)
            kept[which].pop()
            kept["redo" if which == "undo" else "undo"].append(entry)
            self._keep_history(name, kept)
        return dict(self.history(name), done=entry["what"], name=name)

    @staticmethod
    def _hidden_path(mvr_path):
        return mvr_path[:-4] + ".hidden.json"    # the layers and classes of a show that are not to be shown

    def _hidden(self, mvr_path):
        """What of a show is chosen not to be shown: {"layers": [...], "classes": [...], "fixtures": [...]};
        the fixtures are the ids of those in hidden groups, written there from the groups (_write_groups)."""
        try:
            with open(self._hidden_path(mvr_path), encoding="utf-8") as handle:
                kept = json.load(handle)
        except (OSError, ValueError):
            kept = {}
        if not isinstance(kept, dict):
            kept = {}
        return {key: [name for name in kept.get(key, []) if isinstance(name, str)] if isinstance(kept.get(key), list) else []
                for key in ("layers", "classes", "fixtures")}

    def _write_hidden(self, mvr_path, hidden):
        target = self._hidden_path(mvr_path)
        if not any(hidden.get(key) for key in ("layers", "classes", "fixtures")):
            if os.path.exists(target):
                os.replace(target, target + ".was")    # everything shown again: set the old choice aside
            return
        with open(target + ".part", "w", encoding="utf-8") as handle:
            json.dump({key: hidden.get(key, []) for key in ("layers", "classes", "fixtures") if hidden.get(key) or key != "fixtures"}, handle, indent=1)
        os.replace(target + ".part", target)

    # Groups of fixtures of one's own: kept beside the show as "<show>.groups.json", {"groups": [{"id",
    # "name", "uuids", "hidden"}]}. A group can be selected in the patch, and hidden: the fixtures of
    # hidden groups are written into the hidden file (above), which the visualizer reads.
    @staticmethod
    def _groups_path(mvr_path):
        return mvr_path[:-4] + ".groups.json"

    def _group_list(self, mvr_path):
        try:
            with open(self._groups_path(mvr_path), encoding="utf-8") as handle:
                listed = json.load(handle).get("groups", [])
        except (OSError, ValueError, AttributeError):
            return []
        rows = []
        for entry in listed if isinstance(listed, list) else []:
            if not isinstance(entry, dict) or not isinstance(entry.get("id"), str) or not isinstance(entry.get("name"), str) or \
                    not isinstance(entry.get("uuids"), list) or any(row["id"] == entry["id"] for row in rows):
                continue
            rows.append({"id": entry["id"], "name": entry["name"], "uuids": [uuid for uuid in entry["uuids"] if isinstance(uuid, str)],
                         "hidden": entry.get("hidden") is True})
        return rows

    def _group_rows(self, mvr_path):
        return [dict(row, count=len(row["uuids"])) for row in self._group_list(mvr_path)]

    def _write_groups(self, mvr_path, rows):
        target = self._groups_path(mvr_path)
        if rows:
            with open(target + ".part", "w", encoding="utf-8") as handle:
                json.dump({"groups": rows}, handle, indent=1)
            os.replace(target + ".part", target)
        elif os.path.exists(target):
            os.remove(target)
        # What the visualizer hides: the fixtures of every hidden group.
        hidden = self._hidden(mvr_path)
        hidden["fixtures"] = sorted({uuid for row in rows if row["hidden"] for uuid in row["uuids"]})
        self._write_hidden(mvr_path, hidden)

    def _known_uuids(self, name, uuids):
        if not isinstance(uuids, list) or not uuids or any(not isinstance(uuid, str) for uuid in uuids) or len(uuids) > 10000:
            raise Problem("Select the fixtures for the group first.")
        known = {row["uuid"].upper(): row["uuid"] for row in self.show_detail(name)["fixtures"] if row.get("uuid")}
        missing = [uuid for uuid in uuids if uuid.upper() not in known]
        if missing:
            raise Problem(f"{len(missing)} fixture{'' if len(missing) == 1 else 's'} of that selection {'is' if len(missing) == 1 else 'are'} not in the show.")
        return list(dict.fromkeys(known[uuid.upper()] for uuid in uuids))

    @staticmethod
    def _group_name(text, rows, but=None):
        if not isinstance(text, str) or not text.strip() or len(text.strip()) > 60 or any(ord(ch) < 32 for ch in text):
            raise Problem("A group needs a name of up to 60 characters.")
        text = text.strip()
        if any(row["name"].casefold() == text.casefold() and row["id"] != but for row in rows):
            raise Problem(f"There is a group named {text} already.")
        return text

    def groups(self, name):
        return {"name": name, "groups": self._group_rows(self._show_path(name))}

    def add_group(self, name, group_name, uuids):
        """Keeps the fixtures given as a group of the show, under a name."""
        path = self._show_path(name)
        chosen = self._known_uuids(name, uuids)
        with self._lock:
            rows = self._group_list(path)
            taken = {row["id"] for row in rows}
            number = next(n for n in range(1, len(rows) + 2) if f"group-{n}" not in taken)
            rows.append({"id": f"group-{number}", "name": self._group_name(group_name, rows), "uuids": chosen, "hidden": False})
            self._write_groups(path, rows)
        return dict(self.groups(name), added=f"group-{number}")

    def set_group(self, name, group_id, changes):
        """Renames a group, hides or shows it, replaces its fixtures or adds to them."""
        path = self._show_path(name)
        if not isinstance(changes, dict) or not changes or any(key not in ("name", "hidden", "uuids", "add") for key in changes):
            raise Problem("A group has a name, its fixtures and whether it is hidden.")
        with self._lock:
            rows = self._group_list(path)
            row = next((row for row in rows if row["id"] == group_id), None)
            if row is None:
                raise Problem("That group is no longer in the show.")
            if "name" in changes:
                row["name"] = self._group_name(changes["name"], rows, but=group_id)
            if "hidden" in changes:
                if not isinstance(changes["hidden"], bool):
                    raise Problem("Hidden must be on or off.")
                row["hidden"] = changes["hidden"]
            if "uuids" in changes:
                row["uuids"] = self._known_uuids(name, changes["uuids"])
            if "add" in changes:
                row["uuids"] = list(dict.fromkeys(row["uuids"] + self._known_uuids(name, changes["add"])))
            self._write_groups(path, rows)
        return self.groups(name)

    def remove_group(self, name, group_id):
        """Takes a group away; its fixtures stay in the show, and are shown again if it was hidden."""
        path = self._show_path(name)
        with self._lock:
            rows = self._group_list(path)
            if not any(row["id"] == group_id for row in rows):
                raise Problem("That group is no longer in the show.")
            self._write_groups(path, [row for row in rows if row["id"] != group_id])
        return self.groups(name)

    def set_hidden(self, name, layers, classes):
        """Chooses which layers and classes of a show are left out the next time it is opened."""
        path = self._show_path(name)
        summary = self._summary(path)
        known = {"layers": {row["name"] for row in summary.get("layer_list", [])}, "classes": {row["name"] for row in summary.get("class_list", [])}}
        wanted = {}
        for key, names in (("layers", layers), ("classes", classes)):
            if not isinstance(names, list) or any(not isinstance(entry, str) for entry in names):
                raise Problem("Layers and classes must be given as lists of names.")
            unknown = [entry for entry in names if entry not in known[key]]
            if unknown:
                raise Problem(f"{name} has no {'layer' if key == 'layers' else 'class'} named {unknown[0]}.")
            wanted[key] = sorted(set(names), key=str.casefold)
        wanted["fixtures"] = self._hidden(path)["fixtures"]    # those of hidden groups stay hidden
        self._write_hidden(path, wanted)
        return {"hidden": self._hidden(path)}

    # ------------------------------------------------------------------- exchange with the console

    def _make_station(self, name, station_uuid, group):
        return mvr_xchange.Station(name, station_uuid, group, self.received_dir, addresses=local_addresses())

    def xchange_apply(self):
        """Starts, stops or restarts this computer's MVR-xchange station to match the settings."""
        with self._lock:
            settings = self.settings()
            wanted = self._xchange_allowed and settings["xchange"] and bool(settings["xchange_group"])
            station = self._station
            name = settings["xchange_name"] or f"Openviz on {socket.gethostname().split('.')[0]}"
            if station and (not wanted or station.group != settings["xchange_group"] or station.name != name):
                station.stop()
                self._station = station = None
            if wanted and not station:
                station_uuid = settings["xchange_uuid"]
                if not station_uuid:
                    station_uuid = str(uuid.uuid4())
                    self.update_settings({"xchange_uuid": station_uuid})
                # What was fetched before is in the shows by now, and no station can ask for it any more.
                shutil.rmtree(self.received_dir, ignore_errors=True)
                station = self._make_station(name, station_uuid, settings["xchange_group"])
                try:
                    station.start()
                except OSError as error:
                    raise Problem(f"MVR-xchange could not be started: {error}") from None
                self._station = station
        return self.xchange_state()

    def xchange_state(self):
        settings = self.settings()
        station = self._station
        state = {"on": settings["xchange"], "group": settings["xchange_group"], "name": settings["xchange_name"],
                 "default_name": f"Openviz on {socket.gethostname().split('.')[0]}", "running": bool(station), "stations": [], "groups": {}, "offers": [], "trouble": ""}
        if station:
            state.update({key: value for key, value in station.state().items() if key in ("stations", "groups", "offers", "trouble")})
        elif settings["xchange"] and not settings["xchange_group"]:
            state["trouble"] = "Name the group to be in."
        return state

    def xchange_set(self, on=None, group=None, name=None):
        changes = {key: value for key, value in (("xchange", on), ("xchange_group", group), ("xchange_name", name)) if value is not None}
        if changes:
            self.update_settings(changes)
        return self.xchange_apply()

    def xchange_groups(self):
        """A fresh look at the MVR-xchange groups on the network: {"groups": {name: other stations},
        "mine": the group this computer is in, or ""}. This computer's own station is not counted,
        so being alone in a group reads as nobody being there, which is the case."""
        station = self._station
        groups = {}
        if not self._xchange_allowed:
            return {"groups": groups, "mine": ""}
        for entry in mvr_xchange.find_stations(1.5, local_addresses(), [self.settings()["xchange_group"]]):
            if station and mvr_xchange.same(entry["uuid"], station.uuid):
                continue
            spelled = station.group if station and mvr_xchange.in_group(entry["group"], station.group) else \
                next((name for name in groups if name.casefold() == entry["group"].casefold()), entry["group"])
            groups[spelled] = groups.get(spelled, 0) + 1
        return {"groups": groups, "mine": station.group if station else ""}

    def xchange_fetch(self, file_uuid):
        """Fetches a rig another station offered, and takes it in as a dropped file would be."""
        station = self._station
        if not station:
            raise Problem("MVR-xchange is not on. Join a group under Connections first.")
        try:
            path = station.fetch(file_uuid)
        except mvr_xchange.Refused as error:
            raise Problem(str(error)) from None
        return self.add_file(os.path.basename(path), path)

    def xchange_send(self, name, comment=""):
        """Offers a show to the group, with the changes made here written into it."""
        station = self._station
        if not station:
            raise Problem("MVR-xchange is not on. Join a group under Connections first.")
        if not isinstance(comment, str) or len(comment) > 500:
            raise Problem("That comment is too long.")
        answer = self.export_show(name)
        offer = station.commit(os.path.join(self.exports_dir, answer["file"]), comment.strip(), file_name=name)
        return {"file": answer["file"], "told": offer["told"], "stations": len(station.state()["stations"])}

    # ----------------------------------------------------------------------------- set pieces

    # The kinds of set piece there are and what each is when nothing more is said: width, depth,
    # height in millimetres, its grey, where a new one is set down, and what its measures are
    # called. The shapes are made by the core (vizcore/pieces.h, default_piece): keep the two alike.
    PIECES = {
        "deck": {"label": "Deck", "size": (2440.0, 2440.0, 600.0), "grey": 0.03, "at": (0.0, 0.0, 0.0)},
        "drape": {"label": "Drape", "size": (12000.0, 40.0, 7000.0), "grey": 0.012, "at": (0.0, 6000.0, 0.0)},
        "truss": {"label": "Truss", "size": (12000.0, 290.0, 290.0), "grey": 0.45, "at": (0.0, 0.0, 6000.0)},
        "box": {"label": "Box", "size": (1000.0, 1000.0, 1000.0), "grey": 0.08, "at": (0.0, 0.0, 0.0)},
        "person": {"label": "Person", "size": (500.0, 300.0, 1750.0), "grey": 0.25, "at": (0.0, 0.0, 0.0)},
        "circle": {"label": "Truss circle", "size": (8000.0, 290.0, 290.0), "grey": 0.45, "at": (0.0, 0.0, 6000.0)},
        "corner": {"label": "Truss corner", "size": (2000.0, 290.0, 290.0), "grey": 0.45, "at": (0.0, 0.0, 6000.0)},
        "triangle": {"label": "Triangle truss", "size": (8000.0, 290.0, 290.0), "grey": 0.45, "at": (0.0, 0.0, 6000.0)},
        "pipe": {"label": "Pipe", "size": (10000.0, 48.0, 48.0), "grey": 0.04, "at": (0.0, 0.0, 6000.0)},
        "boom": {"label": "Boom", "size": (450.0, 48.0, 3000.0), "grey": 0.04, "at": (0.0, 0.0, 0.0)},
        "crowd": {"label": "Crowd", "size": (12000.0, 8000.0, 1750.0), "grey": 0.12, "at": (0.0, -9000.0, 0.0)},
        "stairs": {"label": "Stairs", "size": (1200.0, 1200.0, 600.0), "grey": 0.03, "at": (0.0, -2000.0, 0.0)},
        "model": {"label": "Model", "size": (1000.0, 1000.0, 1000.0), "grey": 0.5, "at": (0.0, 0.0, 0.0)},
    }
    MODEL_UNITS = ("", "mm", "cm", "m", "in", "ft")

    @staticmethod
    def _pieces_path(mvr_path):
        return mvr_path[:-4] + ".pieces.json"    # the set pieces added to a show; read by LvShow, which takes changes up while it runs

    @classmethod
    def _checked_piece(cls, entry):
        """A set piece as it should be, or a Problem saying what is wrong with it. What is missing is as its kind usually is."""
        kind = entry.get("kind")
        if kind not in cls.PIECES:
            raise Problem("A set piece must be a deck, stairs, drape, truss, truss circle, truss corner, triangle truss, pipe, boom, box, person, crowd or model.")
        usual = cls.PIECES[kind]

        def number(key, otherwise, low, high, wrong):
            value = entry.get(key, otherwise)
            if isinstance(value, bool) or not isinstance(value, (int, float)) or value != value or not low <= value <= high:
                raise Problem(wrong)
            return round(float(value), 3)

        name = entry.get("name", "")
        if not isinstance(name, str) or len(name) > 100 or any(ord(character) < 32 for character in name):
            raise Problem("That name cannot be used.")
        colour = entry.get("colour", [usual["grey"]] * 3)
        if not isinstance(colour, (list, tuple)) or len(colour) != 3 or any(isinstance(part, bool) or not isinstance(part, (int, float)) or not 0 <= part <= 1 for part in colour):
            raise Problem("A colour must be three numbers from 0 to 1 (red, green, blue).")
        out = {"kind": kind, "name": name.strip(), "colour": [round(float(part), 4) for part in colour]}
        for key, otherwise in zip(("width", "depth", "height"), usual["size"]):
            out[key] = number(key, otherwise, 1, 100000, "A set piece measures between a millimetre and 100 metres each way.")
        for key, otherwise in zip(("x", "y", "z"), usual["at"]):
            out[key] = number(key, otherwise, -500000, 500000, "A set piece stands within 500 metres of the middle of the stage.")
        out["turn"] = number("turn", 0.0, -360, 360, "A set piece is turned by between -360 and 360 degrees.")
        tracker = entry.get("tracker")
        if tracker not in (None, 0, ""):
            # Where a tracking system's tracker of that number is (PosiStageNet), the piece goes, while a show is watched.
            if isinstance(tracker, bool) or not isinstance(tracker, int) or not 1 <= tracker <= 65535:
                raise Problem("A tracker is a number from 1 to 65535.")
            follow_height = entry.get("follow_height", False)
            if not isinstance(follow_height, bool):
                raise Problem("Following the tracker's height is on or off.")
            out.update(tracker=tracker, follow_height=follow_height)
        picture = entry.get("picture", "")
        if picture:
            # A picture on its side toward the house: a printed backdrop, a sign.
            if kind not in ("drape", "box", "deck") or not isinstance(picture, str) or not os.path.isabs(picture) or \
                    not picture.lower().endswith((".png", ".jpg", ".jpeg", ".bmp")):
                raise Problem("Only drapes, boxes and decks can carry a picture (PNG, JPEG or BMP, given with its full path).")
            out["picture"] = picture
        if kind == "model":
            # A model file of the models folder, as big as it is: what its numbers are and which way is up.
            file, units, up = entry.get("file"), entry.get("units", ""), entry.get("up", "")
            if not isinstance(file, str) or os.path.basename(file) != file or not file.lower().endswith(MODEL_ENDINGS):
                raise Problem("Choose a 3D model file (GLB, 3DS or OBJ) from your models.")
            if units not in cls.MODEL_UNITS:
                raise Problem("Model units must be millimetres, centimetres, metres, inches, feet, or as in the file.")
            if up not in ("", "y", "z"):
                raise Problem("The up axis must be Y, Z, or as in the file.")
            out.update(file=file, units=units, up=up)
        return out

    def _piece_list(self, mvr_path):
        """A show's set pieces: the entries of its pieces file that are as they should be."""
        try:
            with open(self._pieces_path(mvr_path), encoding="utf-8") as handle:
                listed = json.load(handle).get("pieces", [])
        except (OSError, ValueError, AttributeError):
            return []
        rows = []
        for entry in listed if isinstance(listed, list) else []:
            if not isinstance(entry, dict) or not isinstance(entry.get("id"), str) or not entry["id"] or any(row["id"] == entry["id"] for row in rows):
                continue
            try:
                rows.append(dict(self._checked_piece(entry), id=entry["id"]))
            except Problem:
                continue
        return rows

    def _write_pieces(self, mvr_path, rows):
        target = self._pieces_path(mvr_path)
        if not rows:
            if os.path.exists(target):
                os.remove(target)
            return
        with open(target + ".part", "w", encoding="utf-8") as handle:
            json.dump({"pieces": rows}, handle, indent=1)
        os.replace(target + ".part", target)

    def pieces(self, name):
        """The set pieces added to a show, the kinds there are to add, and the model files there are."""
        path = self._show_path(name)
        rows = self._piece_list(path)
        for row in rows:
            if row["kind"] == "model" and not os.path.isfile(os.path.join(self.models_dir, row["file"])):
                row["trouble"] = "its model file is no longer among the models"
        return {"name": name, "pieces": rows, "models": [entry["name"] for entry in self.models()],
                "kinds": [{"kind": kind, "label": usual["label"], "size": list(usual["size"])} for kind, usual in self.PIECES.items()]}

    # -------------------------------------------------------------------------- model files

    def models(self):
        """The model files there are for set pieces, by name."""
        try:
            names = sorted((entry for entry in os.listdir(self.models_dir) if entry.lower().endswith(MODEL_ENDINGS)), key=str.casefold)
        except OSError:
            names = []
        return [{"name": entry, "size": os.path.getsize(os.path.join(self.models_dir, entry))} for entry in names]

    def _add_model(self, name, source_path, extra=()):
        """Takes in a model file (and, for an OBJ chosen from its folder, its MTL file and pictures)."""
        if os.path.getsize(source_path) > 1 << 30:
            raise Problem("Model files larger than 1 GB are not supported.")
        with open(source_path, "rb") as handle:
            head = handle.read(4)
        lower = str(name).lower()
        if lower.endswith(".glb") and head != b"glTF":
            raise Problem(f"{os.path.basename(str(name))} is not a binary glTF file.")
        if lower.endswith(".3ds") and head[:2] != b"MM":
            raise Problem(f"{os.path.basename(str(name))} is not a 3DS file.")
        extension = next(ending for ending in MODEL_ENDINGS if lower.endswith(ending))
        os.makedirs(self.models_dir, exist_ok=True)
        warnings = []
        with self._lock:
            target = os.path.join(self.models_dir, safe_name(name, extension))
            if os.path.exists(target):
                if _same_file(target, source_path):
                    return {"kind": "model", "name": os.path.basename(target), "already_here": True, "warnings": warnings}
                stem, number = os.path.basename(target)[:-len(extension)], 2
                while os.path.exists(os.path.join(self.models_dir, f"{stem} ({number}){extension}")):
                    number += 1
                target = os.path.join(self.models_dir, f"{stem} ({number}){extension}")
            shutil.copyfile(source_path, target + ".part")
            os.replace(target + ".part", target)
            for path in extra:    # what the model names beside itself: kept by their own names, a different one already here is kept
                there = os.path.join(self.models_dir, os.path.basename(path))
                if os.path.exists(there) and not _same_file(there, path):
                    warnings.append(f"A different {os.path.basename(path)} is among the models already: that one was kept.")
                elif not os.path.exists(there):
                    shutil.copyfile(path, there)
        if extension == ".obj" and not extra:
            warnings.append("An OBJ file's colours and pictures are in its MTL file and pictures beside it, which a dropped file does not bring: "
                            "add it with \"Choose a model file\" in a show's Set pieces to have them, or use GLB, which carries them inside.")
        return {"kind": "model", "name": os.path.basename(target), "renamed": os.path.basename(target) != safe_name(name, extension), "warnings": warnings}

    def import_model(self):
        """Has the user choose a model file in the system's file window and takes it in, with an
        OBJ's MTL file and the pictures that names, from the folder it is in."""
        command = oslayer.choose_file_command("model")
        if command is None:
            raise Problem("Drop the model file on this window instead.", code="no_chooser")
        try:
            done = self._run_chooser(command)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise Problem(f"The file browser could not be opened: {error}") from None
        if done.returncode != 0:
            if "-128" in done.stderr or done.returncode == 3:
                return {"name": ""}
            raise Problem(f"The file browser could not be opened: {done.stderr.strip() or 'no reason given'}")
        path = done.stdout.strip()
        if not path.lower().endswith(MODEL_ENDINGS) or not os.path.isfile(path):
            raise Problem("That is not a GLB, 3DS or OBJ file.")
        return self._add_model(os.path.basename(path), path, model_companions(path))

    def add_piece(self, name, kind, changes=None):
        """Adds a set piece to a show: a deck, a drape, a length of truss, a box or a person, as that
        kind usually is unless `changes` says otherwise. A show that is open shows it at once."""
        path = self._show_path(name)
        if not isinstance(changes, (dict, type(None))):
            raise Problem("Specify the set piece's properties.")
        with self._lock:
            rows = self._piece_list(path)
            if len(rows) >= 500:
                raise Problem("A show can have up to 500 set pieces.")
            piece = self._checked_piece(dict(changes or {}, kind=kind))
            if kind == "model" and not os.path.isfile(os.path.join(self.models_dir, piece["file"])):
                raise Problem(f"{piece['file']} is not in your 3D models. Drop the file on this window or browse for it first.")
            if kind == "model" and not piece["name"]:
                piece["name"] = os.path.splitext(piece["file"])[0]
            if not piece["name"]:
                count = 1 + sum(1 for row in rows if row["kind"] == kind)
                while any(row["name"] == f"{self.PIECES[kind]['label']} {count}" for row in rows):
                    count += 1
                piece["name"] = f"{self.PIECES[kind]['label']} {count}"
            number = 1 + max([int(row["id"][6:]) for row in rows if re.fullmatch(r"piece-\d{1,9}", row["id"])], default=0)
            piece["id"] = f"piece-{number}"
            rows.append(piece)
            self._write_pieces(path, rows)
        return dict(self.pieces(name), added=piece["id"])

    def set_piece(self, name, piece_id, changes):
        """Changes a set piece of a show: its name, its measures, where it stands, how it is turned, its colour."""
        path = self._show_path(name)
        if not isinstance(changes, dict) or not changes or any(key not in ("name", "width", "depth", "height", "x", "y", "z", "turn", "colour", "file", "units", "up", "picture", "tracker", "follow_height") for key in changes):
            raise Problem("Set pieces have a name, size, position, rotation, colour and a tracker to follow; models also have a file, units and an up axis.")
        with self._lock:
            rows = self._piece_list(path)
            row = next((row for row in rows if row["id"] == piece_id), None)
            if row is None:
                raise Problem("This set piece is no longer in the show.")
            row.update(self._checked_piece(dict(row, **changes)))
            if "file" in changes and not os.path.isfile(os.path.join(self.models_dir, row["file"])):
                raise Problem(f"{row['file']} is not in your 3D models.")
            if changes.get("picture") and not os.path.isfile(changes["picture"]):
                raise Problem("That picture file could not be found.")
            if "picture" in changes and not changes["picture"]:
                row.pop("picture", None)
            if "tracker" in changes and not changes["tracker"]:
                row.pop("tracker", None)
                row.pop("follow_height", None)
            if not row["name"]:
                raise Problem("A set piece needs a name.")
            self._write_pieces(path, rows)
        return self.pieces(name)

    def remove_piece(self, name, piece_id, copy=False):
        """Takes a set piece out of a show for good; or, with `copy`, sets another like it beside it."""
        path = self._show_path(name)
        with self._lock:
            rows = self._piece_list(path)
            row = next((row for row in rows if row["id"] == piece_id), None)
            if row is None:
                raise Problem("This set piece is no longer in the show.")
            if not copy:
                rows.remove(row)
                self._write_pieces(path, rows)
                return self.pieces(name)
        # Beside it: one width further along its own length, whichever way it is turned.
        angle = math.radians(row["turn"])
        twin = {key: value for key, value in row.items() if key not in ("id", "name")}
        twin.update(x=round(row["x"] + row["width"] * math.cos(angle), 3), y=round(row["y"] + row["width"] * math.sin(angle), 3))
        return self.add_piece(name, row["kind"], twin)

    # ------------------------------------------------------------------- stand-in fixture types

    def _types(self, path):
        """A show's fixture types as its rig file has them, from the core: which the file says little
        about, the modes in use, and what stands in for each."""
        if not os.path.exists(self.inspector):
            raise Problem("The core library is not built, so fixture types cannot be read. See System.", code="no_core")
        try:
            done = subprocess.run([self.inspector, "--types", path, self.fixtures_dir], capture_output=True, text=True, errors="replace", timeout=120)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise Problem(f"The show's fixture types could not be read: {error}") from None
        types = {}
        for line in done.stdout.splitlines():
            parts = line.split("\t")
            try:
                if parts[0] == "type" and len(parts) == 6:
                    types[parts[1]] = {"spec": parts[1], "name": bare_type(parts[1]), "fixtures": int(parts[2]), "bare": parts[3] == "1",
                                       "standin": parts[4], "standin_state": parts[5], "modes": []}
                elif parts[0] == "mode" and len(parts) == 10 and parts[1] in types:
                    # "dimmer": whether the stand-in has its dimmer where the rig file's placeholder has it (None: nothing to go by).
                    types[parts[1]]["modes"].append({"name": parts[2], "fixtures": int(parts[3]), "slots": int(parts[4]), "channels": int(parts[5]),
                                                     "silent": int(parts[6]), "standin": parts[7], "standin_slots": int(parts[8]),
                                                     "dimmer": {"1": True, "0": False}.get(parts[9])})
                elif parts[0] == "removed" and len(parts) == 3 and parts[1] in types:
                    types[parts[1]]["removed"] = int(parts[2])    # fixtures of the type taken out of the show
                elif parts[0] == "moves" and len(parts) == 8 and parts[1] in types:
                    # Which way the type's pan and tilt are read in this show, and whether it has such channels at all.
                    types[parts[1]].update(invert_pan=parts[2] == "1", invert_tilt=parts[3] == "1", pan_offset=float(parts[4]), tilt_offset=float(parts[5]),
                                           has_pan=parts[6] == "1", has_tilt=parts[7] == "1")
                elif parts[0] == "curve" and len(parts) == 3 and parts[1] in types:
                    types[parts[1]]["dimmer_curve"] = parts[2]    # how its dimmer level becomes light in this show
                elif parts[0] == "error" and len(parts) == 2:
                    raise Problem(f"The show's fixture types could not be read: {parts[1]}")
            except ValueError:
                continue
        for entry in types.values():
            for key, empty in (("invert_pan", False), ("invert_tilt", False), ("pan_offset", 0.0), ("tilt_offset", 0.0), ("has_pan", False), ("has_tilt", False), ("removed", 0)):
                entry.setdefault(key, empty)
        return list(types.values())

    @staticmethod
    def _fit(needed, lengths):
        """Whether a fixture file with modes of these lengths could stand in where modes of the `needed` lengths are in use."""
        return bool(needed) and all(slots in lengths for slots in needed)

    def show_types(self, name):
        """A show's fixture types, the placeholders first, each with the files of the fixture library that could stand in for it."""
        path = self._show_path(name)
        types = self._types(path)
        library = [row for row in self.fixtures() if not row.get("error")]
        for entry in types:
            needed = sorted({mode["slots"] for mode in entry["modes"] if mode["slots"]})
            entry["needs"] = needed
            found = []
            for row in library:
                score = likeness(entry["name"], f"{row.get('manufacturer', '')}@{row.get('name', '')}")
                fits = self._fit(needed, {mode["footprint"] for mode in row["modes"]})
                if score >= 0.5 or row["file"] == entry["standin"]:
                    found.append({"file": row["file"], "manufacturer": row.get("manufacturer", ""), "name": row.get("name", ""), "fits": fits, "score": round(score, 3),
                                  "modes": row["modes"]})
            found.sort(key=lambda row: (not row["fits"], -row["score"], row["file"].casefold()))
            entry["library"] = found[:6]
        types.sort(key=lambda entry: (entry["fixtures"] == 0, not (entry["bare"] and entry["standin_state"] != "ok"), not entry["bare"], -entry["fixtures"], entry["name"].casefold()))
        return {"name": name, "types": types, "files": [{"file": row["file"], "manufacturer": row.get("manufacturer", ""), "name": row.get("name", "")} for row in library],
                "logged_in": self._list()[0] is not None}

    def _type_of(self, path, spec):
        if not isinstance(spec, str) or not spec:
            raise Problem("No fixture type was specified.")
        entry = next((entry for entry in self._types(path) if entry["spec"] == spec), None)
        if entry is None:
            raise Problem("This show has no such fixture type.")
        return entry

    def share_for_type(self, name, spec):
        """What GDTF Share has that could stand in for a type of a show: by its name, those first
        whose modes are as long as the ones in use. From the fixture list fetched at the last log-in."""
        path = self._show_path(name)
        entry = self._type_of(path, spec)
        entries, _, fixtures = self._list()
        if entries is None:
            raise Problem("Sign in to GDTF Share once, under Fixture Library, to download its fixture list.", code="login_required", status=401)
        needed = sorted({mode["slots"] for mode in entry["modes"] if mode["slots"]})
        rows = []
        for revisions in fixtures:
            best = revisions[0]
            score = likeness(entry["name"], f"{best['manufacturer']}@{best['fixture']}")
            if score < 0.5:
                continue
            lengths = {mode["dmxfootprint"] for mode in best["modes"] if mode["dmxfootprint"]}
            rows.append({"rid": best["rid"], "manufacturer": best["manufacturer"], "fixture": best["fixture"], "revision": best["revision"],
                         "by_manufacturer": gdtf_share.by_manufacturer(best), "rating": best["rating"], "size": best["filesize"],
                         "modes": [{"name": mode["name"], "footprint": mode["dmxfootprint"]} for mode in best["modes"]],
                         "fits": self._fit(needed, lengths), "score": round(score, 3), "file": gdtf_share.file_name(best),
                         "have": os.path.exists(os.path.join(self.fixtures_dir, gdtf_share.file_name(best)))})
        rows.sort(key=lambda row: (not row["fits"], -row["score"], not row["by_manufacturer"], row["manufacturer"].casefold(), row["fixture"].casefold()))
        return {"rows": rows[:12], "total": len(rows), "needs": needed}

    def set_stand_in(self, name, spec, file):
        """Has a file of the fixture library stand in for a fixture type of a show. The core matches
        the modes by their length and refuses a file that has no mode as long as one in use."""
        path = self._show_path(name)
        if not isinstance(spec, str) or not spec or not isinstance(file, str) or os.path.basename(file) != file or not file.lower().endswith(".gdtf"):
            raise Problem("Specify a fixture type and a file from the fixture library.")
        if not os.path.isfile(os.path.join(self.fixtures_dir, file)):
            raise Problem(f"{file} is not in the fixture library.")
        with self._lock:
            self._inspector_writes(["--stand-in", path, spec, self.fixtures_dir, file], "The fixture file could not be put in the type's place")
        return self.show_types(name)

    def set_type_moves(self, name, spec, invert_pan=False, invert_tilt=False, pan_offset=0.0, tilt_offset=0.0):
        """Says how a fixture type's pan and tilt are read in a show: mirrored where the fixture file
        and the console run an axis opposite ways, and turned by a number of degrees where they
        start it from different angles. For every fixture of the type; kept beside the show with
        its stand-ins, and taken up at once by a show that is open."""
        path = self._show_path(name)
        self._type_of(path, spec)
        if not isinstance(invert_pan, bool) or not isinstance(invert_tilt, bool):
            raise Problem("Inversion must be on or off.")
        for value in (pan_offset, tilt_offset):
            if isinstance(value, bool) or not isinstance(value, (int, float)) or value != value or not -360 <= value <= 360:
                raise Problem("Offsets must be between -360 and 360 degrees.")
        with self._lock:
            self._inspector_writes(["--correct", path, spec, "1" if invert_pan else "0", "1" if invert_tilt else "0", f"{pan_offset:.4f}", f"{tilt_offset:.4f}"],
                                   "How the type's pan and tilt are read could not be saved")
        return self.show_types(name)

    DIMMER_CURVES = ("linear", "square", "inverse-square", "s-curve")

    def set_type_curve(self, name, spec, curve):
        """Says how a fixture type's dimmer level becomes light in a show: as the file has it
        (linear), or bent as a dimmer rack or a console's profile would (square law, inverse square,
        an S-curve). For every fixture of the type; taken up at once by a show that is open."""
        path = self._show_path(name)
        self._type_of(path, spec)
        if curve not in self.DIMMER_CURVES:
            raise Problem("A dimmer curve is linear, square, inverse-square or s-curve.")
        with self._lock:
            self._inspector_writes(["--dimmer-curve", path, spec, curve], "The dimmer curve could not be saved")
        return self.show_types(name)

    def clear_stand_in(self, name, spec):
        """Takes a type's stand-in away again: its fixtures are read from the rig file's own type once more."""
        path = self._show_path(name)
        if not isinstance(spec, str) or not spec:
            raise Problem("No fixture type was specified.")
        with self._lock:
            self._inspector_writes(["--no-stand-in", path, spec], "The stand-in could not be taken away")
        return self.show_types(name)

    def _inspector_writes(self, arguments, failed):
        if not os.path.exists(self.inspector):
            raise Problem("The core library is not built. See System.", code="no_core")
        try:
            done = subprocess.run([self.inspector, *arguments], capture_output=True, text=True, errors="replace", timeout=120)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise Problem(f"{failed}: {error}") from None
        facts = dict(line.split("\t", 1) for line in done.stdout.splitlines() if "\t" in line)
        if done.returncode != 0 or "error" in facts:
            raise Problem(f"{failed}: {facts.get('error', 'the core gave no reason')}")

    def builder(self, name="", spec="", file=""):
        """What the Fixture Builder starts from: a fixture type it made before (`file`, in the fixture
        library), a show's placeholder type (`name`, `spec`: the modes in use, their channel counts and
        the dimmer), or a beam moving head. With the kinds of channel it offers."""
        start = None
        if file:
            path = self._library_file(file)
            with open(path, "rb") as stream:
                start = fixture_builder.read_spec(stream.read())
            if start is None:
                raise Problem(f"{bare_type(file)} was not made with the Fixture Builder, so it cannot be edited there.")
        elif name:
            path = self._show_path(name)
            entry = self._type_of(path, spec)
            start = fixture_builder.placeholder_spec(self._packed_type(path, spec), entry["name"],
                                                     [{"name": mode["name"], "slots": mode["slots"]} for mode in entry["modes"] if mode["fixtures"]])
        return {"catalogue": fixture_builder.catalogue(), "spec": start or fixture_builder.default_spec("moving"), "file": file if start else ""}

    @staticmethod
    def builder_template(key, spec=None, keep_modes=False):
        """A Fixture Builder description with a template's typical figures laid over it."""
        if spec is not None and not isinstance(spec, dict):
            raise Problem("The description must be a set of names and values.")
        try:
            return {"spec": fixture_builder.from_template(key, spec, keep_modes is True)}
        except fixture_builder.BuildError as error:
            raise Problem(str(error)) from None

    def _library_file(self, file):
        if not isinstance(file, str) or os.path.basename(file) != file or not file.lower().endswith(".gdtf"):
            raise Problem("Specify a file from the fixture library.")
        path = os.path.join(self.fixtures_dir, file)
        if not os.path.isfile(path):
            raise Problem(f"{file} is not in the fixture library.")
        return path

    @staticmethod
    def _packed_type(path, spec):
        """The GDTF file of a fixture type as the show's MVR packs it, or b"" where it does not."""
        try:
            with zipfile.ZipFile(path) as archive:
                names = {entry.casefold(): entry for entry in archive.namelist()}
                found = names.get(spec.casefold()) or names.get(f"{bare_type(spec)}.gdtf".casefold())
                return archive.read(found) if found else b""
        except (zipfile.BadZipFile, OSError, KeyError):
            return b""

    def build_fixture(self, description, name="", spec=""):
        """Writes the fixture type a Fixture Builder description makes into the fixture library, and,
        given a show and one of its types, has it replace that type. A file of that name made by the
        builder from the same description is written over; any other is left alone."""
        try:
            checked = fixture_builder.check(description)
        except fixture_builder.BuildError as error:
            raise Problem(str(error)) from None
        file = fixture_builder.file_name(checked)
        target = os.path.join(self.fixtures_dir, file)
        with self._lock:
            if os.path.exists(target):
                with open(target, "rb") as stream:
                    theirs = fixture_builder.read_spec(stream.read())
                if not theirs or theirs.get("id") != checked["id"]:
                    raise Problem(f"Your fixture library already has a fixture type called {checked['manufacturer']} {checked['name']}. Give this one another name.")
            with open(target + ".part", "wb") as stream:
                stream.write(fixture_builder.build_gdtf(checked))
            os.replace(target + ".part", target)
        answer = {"file": file, "spec": checked}
        if name:
            answer["types"] = self.set_stand_in(name, spec, file)
        return answer

    def share_stand_in(self, name, spec, rid):
        """Fetches a fixture from GDTF Share into the library and has it stand in for a type of a show."""
        self._show_path(name)
        fetched = self.share_download(rid)
        return dict(self.set_stand_in(name, spec, fetched["file"]), fetched=fetched["file"])

    def stand_in_all(self, name):
        """For every placeholder of a show that has none yet, takes the likeliest file of the library that fits. Returns what was done."""
        done, left = [], []
        for entry in self.show_types(name)["types"]:
            if not entry["bare"] or entry["standin_state"] == "ok":
                continue
            best = next((row for row in entry["library"] if row["fits"] and row["score"] >= 0.75), None)
            if best is None:
                left.append(entry["name"])
                continue
            try:
                self.set_stand_in(name, entry["spec"], best["file"])
                done.append({"type": entry["name"], "file": best["file"]})
            except Problem:
                left.append(entry["name"])
        return dict(self.show_types(name), done=done, left=left)

    # ------------------------------------------------------------------------------- screens

    @staticmethod
    def _screens_path(mvr_path):
        return mvr_path[:-4] + ".screens.json"    # what a show's screens show, and the screens added here; LvShow reads it

    def _screen_settings(self, mvr_path):
        """What was chosen for a show's screens: the entries of its screens file that are as they should be."""
        try:
            with open(self._screens_path(mvr_path), encoding="utf-8") as handle:
                listed = json.load(handle).get("screens", [])
        except (OSError, ValueError, AttributeError):
            return []
        rows = []
        for entry in listed if isinstance(listed, list) else []:
            if not isinstance(entry, dict) or not isinstance(entry.get("id"), str) or not entry["id"]:
                continue
            added = entry.get("added") is True
            try:
                more = self._checked_more(entry, added)
                row = self._checked_screen(dict(SCREEN_DEFAULTS, **{key: entry[key] for key in SCREEN_DEFAULTS if key in entry}),
                                           added=added, files_must_exist=False, wall=bool(more["replaces"]))
            except Problem:
                continue
            rows.append(dict(row, id=entry["id"], added=added, **more))
        return rows

    @staticmethod
    def _checked_part(part):
        """The part of its picture a screen shows: [left, top, right, bottom] as shares of the whole."""
        if (not isinstance(part, (list, tuple)) or len(part) != 4 or any(isinstance(edge, bool) or not isinstance(edge, (int, float)) or not 0 <= edge <= 1 for edge in part)
                or part[2] - part[0] < 0.001 or part[3] - part[1] < 0.001):
            raise Problem("A crop is given as left, top, right and bottom edges, each from 0 to 1.")
        return [round(float(edge), 6) for edge in part]

    @staticmethod
    def _checked_map(numbers):
        """A part of a picture that need not be upright in it (an LED processor's map may lay panels
        out turned): where the screen's top left corner is in the picture, and how far the picture
        moves across its width and down its height, as shares of the whole: [ox, oy, ux, uy, vx, vy].
        None for none."""
        if numbers is None:
            return None
        if (not isinstance(numbers, list) or len(numbers) != 6
                or any(isinstance(n, bool) or not isinstance(n, (int, float)) or not math.isfinite(n) or abs(n) > 10 for n in numbers)
                or abs(numbers[2] * numbers[5] - numbers[3] * numbers[4]) < 1e-9):
            raise Problem("A map is the picture's corner and its steps across and down: six numbers.")
        return [round(float(n), 7) for n in numbers]

    @classmethod
    def _checked_more(cls, entry, added, files_must_exist=False):
        """What a screen has beyond its picture and its place: the part of the picture it shows, the
        sections it is split into, and, for a screen that lies where fixtures hang, its face and
        the fixtures it stands in for. Raises Problem at the first thing that is not right."""
        more = {"part": cls._checked_part(entry.get("part", [0, 0, 1, 1])), "columns": 0, "rows": 0, "sections": [], "replaces": [], "face": None,
                "map": cls._checked_map(entry.get("map"))}
        replaces, face = entry.get("replaces"), entry.get("face")
        if replaces is not None:
            if (not added or not isinstance(replaces, list) or not 1 <= len(replaces) <= 4096 or any(not isinstance(one, str) or not 0 < len(one) <= 64 for one in replaces)
                    or face is None):
                raise Problem("That screen does not replace any fixtures.")
            more["replaces"] = list(replaces)
        if face is not None:
            # Where it lies, not necessarily upright: its picture's right, up and out, and its middle in
            # millimetres. A screen over fixtures has one; a screen added here may have one too (2026-10-20:
            # LED panels round a raked truss), in place of standing upright at x, y, z, turned.
            if (not added or not isinstance(face, list) or len(face) != 12
                    or any(isinstance(number, bool) or not isinstance(number, (int, float)) or not math.isfinite(number) for number in face)):
                raise Problem("A screen's face is its right, up and out directions and its middle: twelve numbers.")
            right, up, out = face[0:3], face[3:6], face[6:9]
            unit = lambda v: abs(math.sqrt(sum(c * c for c in v)) - 1.0) < 0.01
            square = lambda a, b: abs(sum(x * y for x, y in zip(a, b))) < 0.01
            if not (unit(right) and unit(up) and unit(out) and square(right, up) and square(right, out) and square(up, out)):
                raise Problem("A screen's right, up and out must be at right angles to each other, each of length 1.")
            more["face"] = [float(number) for number in face]
        columns, rows, sections = entry.get("columns", 0), entry.get("rows", 0), entry.get("sections", [])
        if columns or rows or sections:
            if (isinstance(columns, bool) or isinstance(rows, bool) or not isinstance(columns, int) or not isinstance(rows, int) or not 1 <= columns <= MAX_SPLIT
                    or not 1 <= rows <= MAX_SPLIT or columns * rows < 2 or not isinstance(sections, list) or len(sections) != columns * rows):
                raise Problem(f"A screen can be split into at most {MAX_SPLIT} x {MAX_SPLIT} sections.")
            for section in sections:
                if not isinstance(section, dict):
                    raise Problem("That screen section does not exist.")
                content = cls._checked_screen(dict({"source": "pattern", "value": "", "nits": 1000.0}, **{key: section[key] for key in ("source", "value", "nits") if key in section}),
                                              added=False, files_must_exist=files_must_exist, wall=bool(more["replaces"]))
                more["sections"].append(dict(content, part=cls._checked_part(section.get("part", [0, 0, 1, 1]))))
            more["columns"], more["rows"] = columns, rows
        return more

    def _write_screens(self, mvr_path, rows):
        kept = []
        for row in rows:
            keys = ("id", "source", "value", "nits") + (("name", "width", "height", "x", "y", "z", "turn") if row["added"] else ())
            entry = dict({key: row[key] for key in keys}, **({"added": True} if row["added"] else {}))
            # Only what is not as it is by itself: an older Openviz reads the rest as it always did.
            if row.get("part", [0, 0, 1, 1]) != [0, 0, 1, 1]:
                entry["part"] = row["part"]
            if row.get("columns"):
                entry.update(columns=row["columns"], rows=row["rows"], sections=row["sections"])
            if row.get("replaces"):
                entry["replaces"] = row["replaces"]
            if row.get("face"):
                entry["face"] = row["face"]
            if row.get("map"):
                entry["map"] = row["map"]
            kept.append(entry)
        target = self._screens_path(mvr_path)
        with open(target + ".part", "w", encoding="utf-8") as handle:
            json.dump({"screens": kept}, handle, indent=1)
        os.replace(target + ".part", target)

    @staticmethod
    def _checked_screen(row, added, files_must_exist=True, wall=False):
        """A screen's settings, checked and tidied. Raises Problem at the first thing that is not right.
        `wall` says the screen lies where fixtures hang, and may leave its place to them."""
        def number(key, low, high, what):
            value = row[key]
            if isinstance(value, bool) or not isinstance(value, (int, float)) or not low <= value <= high:
                raise Problem(what)
            return float(value)

        out = {}
        if row["source"] not in SCREEN_SOURCES and not (wall and row["source"] == "fixtures"):
            raise Problem("A screen's source must be a test pattern, picture, video file, NDI stream, DMX pixels or off."
                          if row["source"] != "fixtures" else "Only a screen made in the place of fixtures can leave its place to them.")
        out["source"] = row["source"]
        value = row["value"]
        if not isinstance(value, str) or len(value) > 1000 or any(ord(ch) < 32 for ch in value):
            raise Problem("That is not a valid file path or stream name.")
        value = value.strip()
        if out["source"] in ("picture", "film") and value:
            endings = PICTURE_ENDINGS if out["source"] == "picture" else FILM_ENDINGS
            what = "picture" if out["source"] == "picture" else "film"
            if not os.path.isabs(value):
                raise Problem(f"Enter the full path of the {what}.")
            if not value.lower().endswith(endings):
                raise Problem(f"{os.path.basename(value)} is not a supported {what} format. Supported: {', '.join(e[1:].upper() for e in endings)}.")
            if files_must_exist and not os.path.isfile(value):
                raise Problem(f"File not found: {value}")
        if out["source"] == "pixels":
            value = pixel_map(value)["text"]
        out["value"] = value if out["source"] in ("picture", "film", "ndi", "pixels") else ""
        out["nits"] = number("nits", 0, 10000, "Brightness is between 0 and 10,000 nits.")
        if added:
            name = row["name"]
            if not isinstance(name, str) or not name.strip() or len(name) > 80 or any(ord(ch) < 32 for ch in name):
                raise Problem("Screen names can be up to 80 characters.")
            out["name"] = name.strip()
            out["width"] = number("width", 100, 100000, "A screen is between 0.1 and 100 metres wide.")
            out["height"] = number("height", 100, 100000, "A screen is between 0.1 and 100 metres high.")
            for key in ("x", "y", "z"):
                out[key] = number(key, -500000, 500000, "A screen stands within 500 metres of the middle of the stage.")
            out["turn"] = number("turn", -360, 360, "A screen is turned by between -360 and 360 degrees.")
        return out

    def _rig_screens(self, path):
        """The video screens a show's rig file has, from the core."""
        if not os.path.exists(self.inspector):
            return []
        try:
            done = subprocess.run([self.inspector, "--screens", path], capture_output=True, text=True, errors="replace", timeout=120)
        except (OSError, subprocess.TimeoutExpired):
            return []
        rows = []
        for line in done.stdout.splitlines():
            parts = line.split("\t")
            if parts[0] != "screen" or len(parts) != 21:
                continue
            try:
                numbers = [float(part) for part in parts[5:19]]
            except ValueError:
                continue
            rows.append({"id": parts[1], "name": parts[2], "layer": parts[3], "class": parts[4], "width": numbers[0], "height": numbers[1],
                         "x": numbers[11], "y": numbers[12], "z": numbers[13] - numbers[1] / 2.0,    # the foot of an upright screen
                         "named_type": parts[19], "named": parts[20]})
        return rows

    def screens(self, name, want_walls=True):
        """A show's screens: those its rig file has and those added here, each with what it shows;
        and the walls of LED panels among its fixtures that no screen stands in for yet."""
        path = self._show_path(name)
        chosen = self._screen_settings(path)
        rows = []
        for screen in self._rig_screens(path):
            mine = next((row for row in chosen if not row["added"] and row["id"].casefold() == screen["id"].casefold()), None)
            if mine is None:    # as LvShow does without a choice: the stream the file names, else the test pattern
                stream = screen["named_type"] == "NDI" and screen["named"]
                mine = {"source": "ndi" if stream else "pattern", "value": screen["named"] if stream else "", "nits": 1000.0}
            rows.append(dict(screen, added=False, source=mine["source"], value=mine["value"], nits=mine["nits"], turn=None))
        for row in chosen:
            if row["added"]:
                rows.append(dict(row, layer="", named_type="", named=""))

        def judged(content):
            """A screen's or a section's picture, with what there is to say about it."""
            content["pixels"] = None
            if content["source"] == "pixels":
                try:
                    content["pixels"] = pixel_map(content["value"])
                except Problem:
                    pass
            content["trouble"] = "the file is not there" if content["source"] in ("picture", "film") and content["value"] and not os.path.isfile(content["value"]) else \
                "nothing chosen yet" if content["source"] in ("picture", "film", "ndi") and not content["value"] else ""

        walls = self._rig_walls(path) if any(row.get("replaces") for row in rows) or want_walls else []
        taken = set()
        for row in rows:
            row["class"] = row.get("class", "")
            for key, empty in (("part", [0, 0, 1, 1]), ("columns", 0), ("rows", 0), ("sections", []), ("replaces", []), ("face", None), ("map", None)):
                row.setdefault(key, empty)
            judged(row)
            row["sections"] = [dict(section) for section in row["sections"]]
            for index, section in enumerate(row["sections"]):
                judged(section)
                section["own"] = self._own_part(row, index)    # the part of the screen's picture that falls on it
            # A screen in the place of fixtures: how many of them are still in the rig.
            there = {uuid.casefold() for wall in walls for uuid in wall["uuids"]}
            row["stands_for"] = sum(1 for uuid in row["replaces"] if uuid.casefold() in there) if row["replaces"] else 0
            taken.update(uuid.casefold() for uuid in row["replaces"])
            del row["face"]    # the page has no use for it
        free = [dict({key: wall[key] for key in ("type", "count", "width", "height", "first", "last")}, uuids=wall["uuids"])
                for wall in walls if not any(uuid.casefold() in taken for uuid in wall["uuids"])]
        return {"name": name, "screens": rows, "pictures": [e[1:] for e in PICTURE_ENDINGS], "films": [e[1:] for e in FILM_ENDINGS], "walls": free,
                "max_split": MAX_SPLIT}

    @staticmethod
    def _own_part(row, index):
        """The part of a split screen's picture that falls on its section `index`: so that the
        sections together show the picture whole."""
        left, top, right, bottom = row["part"]
        column, line = index % row["columns"], index // row["columns"]
        wide, high = (right - left) / row["columns"], (bottom - top) / row["rows"]
        return [round(left + wide * column, 6), round(top + high * line, 6), round(left + wide * (column + 1), 6), round(top + high * (line + 1), 6)]

    def _rig_walls(self, path):
        """The walls of LED panels among a show's fixtures, from the core: fixtures of one type hung
        edge to edge in one plane, which a screen can stand in for."""
        if not os.path.exists(self.inspector):
            return []
        try:
            done = subprocess.run([self.inspector, "--walls", path, self.fixtures_dir], capture_output=True, text=True, errors="replace", timeout=120)
        except (OSError, subprocess.TimeoutExpired):
            return []
        walls = {}
        for line in done.stdout.splitlines():
            parts = line.split("\t")
            try:
                if parts[0] == "wall" and len(parts) == 18:
                    numbers = [float(part) for part in parts[4:]]
                    if all(math.isfinite(number) for number in numbers):
                        walls[parts[1]] = {"type": parts[2], "count": int(parts[3]), "width": numbers[0], "height": numbers[1], "face": numbers[2:], "uuids": [], "names": []}
                elif parts[0] == "panel" and len(parts) == 5 and parts[1] in walls:
                    walls[parts[1]]["uuids"].append(parts[2])
                    walls[parts[1]]["names"].append(parts[4] or parts[3])
            except ValueError:
                continue
        return [dict(wall, first=wall["names"][0], last=wall["names"][-1]) for wall in walls.values() if wall["uuids"]]

    def set_screen(self, name, ident, changes):
        """Changes what one screen of a show shows; for a screen added here, also its name, size and place."""
        path = self._show_path(name)
        if not isinstance(ident, str) or not isinstance(changes, dict):
            raise Problem("Specify a screen and the changes to make.")
        with self._lock:
            now = self._one_screen(name, ident)
            content = {"source", "value", "nits", "part", "map"}
            # A screen in the place of fixtures is as large as they are and lies where they hang.
            allowed = content | ({"name"} if now["replaces"] else set(SCREEN_DEFAULTS) if now["added"] else set())
            if set(changes) - allowed:
                raise Problem("Screens from the MVR can only be moved in the program that made the MVR." if not now["added"] and set(changes) <= set(SCREEN_DEFAULTS)
                              else "This screen is as large as the fixtures it stands in for, and lies where they hang." if now["replaces"] and set(changes) <= set(SCREEN_DEFAULTS)
                              else "Unknown screen property.")
            wanted = dict({key: now[key] for key in (set(SCREEN_DEFAULTS) if now["added"] else {"source", "value", "nits"})}, **{key: value for key, value in changes.items() if key not in ("part", "map")})
            if "source" in changes and "value" not in changes and changes["source"] != now["source"]:
                wanted["value"] = ""    # a film's file is no picture, and neither is a stream's name
            checked = self._checked_screen(wanted, added=now["added"], wall=bool(now["replaces"]))
            changed = dict(now, **checked)
            if "part" in changes:
                changed["part"] = self._checked_part(changes["part"])
                changed["map"] = None    # an upright part in its place
            if "map" in changes:
                changed["map"] = self._checked_map(changes["map"])
            self._put_screen(path, changed)
        return self.screens(name)

    def _one_screen(self, name, ident):
        """A screen of a show as it is now, with everything kept about it. Raises Problem if there is none."""
        path = self._show_path(name)
        now = next((row for row in self.screens(name, want_walls=False)["screens"] if row["id"] == ident), None)
        if now is None:
            raise Problem("This show has no such screen.")
        kept = next((row for row in self._screen_settings(path) if row["id"] == ident and row["added"] == now["added"]), None)
        now["face"] = kept["face"] if kept else None
        now["map"] = kept["map"] if kept else None
        now["sections"] = [{key: section[key] for key in ("source", "value", "nits", "part")} for section in now["sections"]]
        return now

    def _put_screen(self, path, changed):
        """Writes one screen's settings back where they were; a screen of the rig file chosen for the first time goes last."""
        rows = self._screen_settings(path)
        place = next((index for index, row in enumerate(rows) if row["id"] == changed["id"] and row["added"] == changed["added"]), None)
        if place is None:
            rows.append(changed)
        else:
            rows[place] = changed    # where it was: screens keep their order
        self._write_screens(path, rows)

    def split_screen(self, name, ident, columns, rows):
        """Splits a screen into a grid of sections, each of which can then show something of its
        own; 1 by 1 makes it one piece again. Every section starts with the screen's picture and
        its own part of it, so that splitting changes nothing until a section is changed."""
        path = self._show_path(name)
        for count in (columns, rows):
            if isinstance(count, bool) or not isinstance(count, int) or not 1 <= count <= MAX_SPLIT:
                raise Problem(f"A screen can be split into at most {MAX_SPLIT} x {MAX_SPLIT} sections.")
        with self._lock:
            now = self._one_screen(name, ident)
            if (columns, rows) != (now["columns"] or 1, now["rows"] or 1):
                now.update(columns=0, rows=0, sections=[])
                if columns * rows > 1:
                    now.update(columns=columns, rows=rows)
                    now["sections"] = [{"source": now["source"], "value": now["value"], "nits": now["nits"], "part": self._own_part(now, index)}
                                       for index in range(columns * rows)]
                self._put_screen(path, now)
        return self.screens(name)

    def set_section(self, name, ident, index, changes):
        """Changes what one section of a split screen shows. `part` is "own" (the part of the
        screen's picture that falls on it), "whole", or its four edges."""
        path = self._show_path(name)
        if not isinstance(ident, str) or not isinstance(changes, dict) or isinstance(index, bool) or not isinstance(index, int):
            raise Problem("Specify a screen section and the changes to make.")
        if set(changes) - {"source", "value", "nits", "part"}:
            raise Problem("Unknown screen section property.")
        with self._lock:
            now = self._one_screen(name, ident)
            if not 0 <= index < len(now["sections"]):
                raise Problem("This screen has no such section.")
            section = now["sections"][index]
            wanted = dict({key: section[key] for key in ("source", "value", "nits")}, **{key: value for key, value in changes.items() if key != "part"})
            if "source" in changes and "value" not in changes and changes["source"] != section["source"]:
                wanted["value"] = ""
            checked = self._checked_screen(wanted, added=False, wall=bool(now["replaces"]))
            part = changes.get("part", section["part"])
            part = self._own_part(now, index) if part == "own" else [0, 0, 1, 1] if part == "whole" else self._checked_part(part)
            now["sections"][index] = dict(checked, part=part)
            self._put_screen(path, now)
        return self.screens(name)

    def add_wall_screen(self, name, uuids):
        """Puts a screen in the place of a wall of LED panels: as large as they are, lying where
        they hang. It starts out leaving the place to them, so nothing changes until it is given
        a picture; wherever it shows one, the fixtures behind are left out of the visualizer."""
        path = self._show_path(name)
        if not isinstance(uuids, list) or not uuids or any(not isinstance(one, str) for one in uuids):
            raise Problem("Specify the fixtures the screen replaces.")
        with self._lock:
            asked = {one.casefold() for one in uuids}
            wall = next((wall for wall in self._rig_walls(path) if {one.casefold() for one in wall["uuids"]} == asked), None)
            if wall is None:
                raise Problem("Those fixtures no longer form a single wall. Refresh the list and try again.")
            rows = self._screen_settings(path)
            if any(one.casefold() in asked for row in rows for one in row.get("replaces", [])):
                raise Problem("A screen already replaces those fixtures.")
            if sum(1 for row in rows if row["added"]) >= 64:
                raise Problem("A show can have up to 64 added screens.")
            title = wall["first"] if wall["count"] == 1 else f"{wall['first']} to {wall['last']}"
            face = wall["face"]
            checked = self._checked_screen(dict(SCREEN_DEFAULTS, name=title[:80], width=wall["width"], height=wall["height"], source="fixtures",
                                                # for the page, which lists a screen's place: the middle of its lower edge
                                                x=round(face[9], 1), y=round(face[10], 1), z=round(face[11] - wall["height"] / 2.0 * face[5], 1)),
                                           added=True, wall=True)
            ident = "added-" + secrets.token_hex(8)
            rows.append(dict(checked, id=ident, added=True, part=[0, 0, 1, 1], columns=0, rows=0, sections=[], replaces=list(wall["uuids"]), face=face))
            self._write_screens(path, rows)
        return dict(self.screens(name), added=ident)

    def add_screen(self, name, fields=None):
        """Adds a screen to a show: upright, facing the house unless turned, showing the test pattern."""
        path = self._show_path(name)
        if fields is not None and not isinstance(fields, dict):
            raise Problem("Screens have a name, size and position.")
        fields = dict(fields or {})
        face = fields.pop("face", None)    # lying as it is given, rather than upright
        where = {key: fields.pop(key) for key in ("map", "part") if key in fields}    # which part of its picture
        if set(fields) - set(SCREEN_DEFAULTS):
            raise Problem("Unknown screen property.")
        with self._lock:
            rows = self._screen_settings(path)
            taken = {row["name"] for row in rows if row["added"]}
            wanted = dict(SCREEN_DEFAULTS, **(fields or {}))
            if "name" not in (fields or {}):
                wanted["name"] = next(f"Screen {number}" for number in range(1, 10000) if f"Screen {number}" not in taken)
            checked = self._checked_screen(wanted, added=True)
            more = self._checked_more(dict(where, **({"face": face} if face is not None else {})), added=True)
            if sum(1 for row in rows if row["added"]) >= 64:
                raise Problem("A show can have up to 64 added screens.")
            ident = "added-" + secrets.token_hex(8)
            rows.append(dict(checked, id=ident, added=True, **more))
            self._write_screens(path, rows)
        return dict(self.screens(name), added=ident)

    def rename_stream(self, name, old, new):
        """Points every screen and section of a show that shows the NDI stream `old` at `new`
        instead: for a stream that turns out to be called otherwise on the network."""
        path = self._show_path(name)
        for value in (old, new):
            if not isinstance(value, str) or not value.strip() or len(value) > 1000 or any(ord(ch) < 32 for ch in value):
                raise Problem("That is not a valid stream name.")
        with self._lock:
            rows = self._screen_settings(path)
            changed = 0
            for row in rows:
                for target in [row] + row.get("sections", []):
                    if target["source"] == "ndi" and target["value"] == old:
                        target["value"] = new.strip()
                        changed += 1
            if not changed:
                raise Problem(f"No screen shows {old}.")
            self._write_screens(path, rows)
        return dict(self.screens(name), renamed=changed)

    def remove_screen(self, name, ident):
        """Takes a screen that was added here out of a show again."""
        path = self._show_path(name)
        with self._lock:
            rows = self._screen_settings(path)
            kept = [row for row in rows if not (row["added"] and row["id"] == ident)]
            if len(kept) == len(rows):
                raise Problem("Only screens added in Openviz can be removed. Set a screen from the MVR to Off instead.")
            self._write_screens(path, kept)
        return self.screens(name)

    def ndi_sources(self):
        """The NDI streams on this computer's networks right now, by name: as NDI's own library
        finds them where Unreal brings one (as the visualizer's receiver will), and as multicast
        DNS finds them."""
        library = ndi_find.library_path(self.settings().get("engine"))
        found = set(ndi_find.find_with_library(library, 3.0) or []) if library else set()
        found.update(ndi_find.find(1.5, local_addresses()))
        return {"sources": sorted(found, key=str.casefold)}

    def choose_file(self, kind):
        """Has the user pick a picture or a film in the system's own file window. Returns its path, or none if they thought better of it."""
        if kind not in ("picture", "film", "sound"):
            raise Problem("Choose a picture, a video file or a sound file.")
        command = oslayer.choose_file_command(kind)
        if command is None:
            raise Problem("Type or paste the file's full path into the field.", code="no_chooser")
        try:
            done = self._run_chooser(command)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise Problem(f"The file browser could not be opened: {error}") from None
        if done.returncode != 0:
            if "-128" in done.stderr or done.returncode == 3:    # thought better of it (as macOS and as Windows say so)
                return {"path": ""}
            raise Problem(f"The file browser could not be opened: {done.stderr.strip() or 'no reason given'}")
        return {"path": done.stdout.strip()}

    @staticmethod
    def _run_chooser(command):
        return subprocess.run(command, capture_output=True, text=True, timeout=900)

    @staticmethod
    def _edits_path(mvr_path):
        return mvr_path[:-4] + ".edits.xml"    # where Unreal keeps the changes made to a show (vizcore/show_edits.h)

    def _edited(self, mvr_path):
        """How many fixtures of a show were changed in the Unreal Editor."""
        try:
            with open(self._edits_path(mvr_path), encoding="utf-8", errors="replace") as handle:
                return len(re.findall(r"<(?:Fixture|Added|Removed)\s", handle.read()))    # changed, added, taken out
        except OSError:
            return 0

    def discard_edits(self, name):
        """Puts a show back to how its MVR describes it. The edits are set aside, not destroyed."""
        if not isinstance(name, str) or os.path.basename(name) != name or not name.lower().endswith(".mvr"):
            raise Problem("That show is not in the library.")
        with self._lock:
            edits = self._edits_path(os.path.join(self.shows_dir, name))
            if not os.path.isfile(edits):
                return {"discarded": False}
            kept = f"{edits[:-4]}.discarded-{time.strftime('%Y%m%d-%H%M%S')}.xml"
            os.replace(edits, kept)
        return {"discarded": True, "kept_as": os.path.basename(kept)}

    def export_show(self, name):
        """Writes a show out as MVR with the changes made in the Unreal Editor in it, and the set
        pieces added here, for taking back to the console or the drawing program. The show in the
        library is not touched."""
        path = self._show_path(name)
        if not os.path.exists(self.inspector):
            raise Problem("The core library is not built, so the show cannot be exported. See System.", code="no_core")
        os.makedirs(self.exports_dir, exist_ok=True)
        out = os.path.join(self.exports_dir, f"{name[:-4]} {time.strftime('%Y-%m-%d %H%M%S')}.mvr")
        try:
            done = subprocess.run([self.inspector, "--export", path, out, self.fixtures_dir], capture_output=True, text=True, errors="replace", timeout=300)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise Problem(f"The show could not be exported: {error}") from None
        facts = dict(line.split("\t", 1) for line in done.stdout.splitlines() if "\t" in line)
        if done.returncode != 0 or "exported" not in facts or not os.path.isfile(out):
            raise Problem(f"The show could not be exported: {facts.get('error', 'the core gave no reason')}")
        return {"file": os.path.basename(out), "folder": self.exports_dir, "changed": int(facts["exported"]), "pieces": int(facts.get("pieces", 0))}

    def package_show(self, name):
        """Packs a show into one file among the exports, with everything it needs on another
        computer: its rig file and what is kept beside it, the fixture types of the library it uses,
        and the pictures and films its screens show (package.py). The show is not touched."""
        path = self._show_path(name)
        if not os.path.exists(self.inspector):
            raise Problem("The core library is not built, so the show cannot be packaged. See System.", code="no_core")
        try:
            done = subprocess.run([self.inspector, "--uses", path], capture_output=True, text=True, errors="replace", timeout=120)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise Problem(f"The show could not be packaged: {error}") from None
        lines = [line.split("\t", 1) for line in done.stdout.splitlines() if "\t" in line]
        failed = next((text for kind, text in lines if kind == "error"), None)
        if failed or done.returncode != 0:
            raise Problem(f"The show could not be packaged: {failed or 'the core gave no reason'}.")
        needed = [text for kind, text in lines if kind == "library"]
        present = [os.path.join(self.fixtures_dir, file) for file in needed if os.path.isfile(os.path.join(self.fixtures_dir, file))]
        missing = [file for file in needed if not os.path.isfile(os.path.join(self.fixtures_dir, file))]
        media = [row["picture"] for row in self._piece_list(path) if row.get("picture") and os.path.isfile(row["picture"])]
        for row in self._screen_settings(path):
            for content in row.get("sections") or [row]:
                if content.get("source") in ("picture", "film") and content.get("value") and os.path.isfile(content["value"]):
                    media.append(content["value"])
        models = []
        for row in self._piece_list(path):
            model = os.path.join(self.models_dir, row.get("file", "")) if row["kind"] == "model" else ""
            if model and os.path.isfile(model):
                models += [model, *model_companions(model)]
        os.makedirs(self.exports_dir, exist_ok=True)
        out = os.path.join(self.exports_dir, f"{name[:-4]} {time.strftime('%Y-%m-%d %H%M%S')}{package.EXTENSION}")
        try:
            made = package.write(out, path, present, media, model_files=models)
        except OSError as error:
            if os.path.exists(out + ".part"):
                os.remove(out + ".part")
            raise Problem(f"The show could not be packaged: {error}") from None
        return {"file": os.path.basename(out), "folder": self.exports_dir, "fixtures": len(made["fixtures"]), "media": len(made["media"]),
                "models": len(made["models"]), "missing": missing, "size": os.path.getsize(out)}

    def _unpack(self, name, source_path):
        """Takes in a show package: the show beside the others (under a new name if one has its
        name), the fixture types it uses into the library where they are not there already, its
        pictures and films under library/media/, and its screens pointed at them."""
        try:
            manifest = package.read(source_path)
        except package.PackageError as error:
            raise Problem(f"{os.path.basename(str(name))} cannot be opened: {error}.") from None
        show = manifest["show"]
        warnings = []
        with self._lock:
            target = os.path.join(self.shows_dir, safe_name(show, ".mvr"))
            renamed = os.path.exists(target)
            if renamed:
                stem = os.path.basename(target)[:-4]
                number = 2
                while any(os.path.exists(os.path.join(self.shows_dir, f"{stem} ({number}){ending}")) for ending in (".mvr", *package.SIDE_ENDINGS)):
                    number += 1
                target = os.path.join(self.shows_dir, f"{stem} ({number}).mvr")
            # The fixture types first, so that the show finds them the first time it is read.
            types_added = 0
            for file in manifest["fixtures"]:
                there = os.path.join(self.fixtures_dir, file)
                if os.path.exists(there):
                    with open(there, "rb") as handle:
                        if handle.read() != package.entry_bytes(source_path, f"fixtures/{file}"):
                            warnings.append(f"Your fixture library has a different {file[:-5]}: yours was kept, and the show uses it.")
                    continue
                package.extract(source_path, f"fixtures/{file}", there)
                types_added += 1
            for file in manifest.get("models", []):
                there = os.path.join(self.models_dir, file)
                os.makedirs(self.models_dir, exist_ok=True)
                if os.path.exists(there):
                    with open(there, "rb") as handle:
                        if handle.read() != package.entry_bytes(source_path, f"models/{file}"):
                            warnings.append(f"Your models have a different {file}: yours was kept, and the show uses it.")
                    continue
                package.extract(source_path, f"models/{file}", there)
            moved = {}
            if manifest["media"]:
                folder = os.path.join(self.library, "media", os.path.basename(target)[:-4])
                os.makedirs(folder, exist_ok=True)
                for stored, original in manifest["media"].items():
                    package.extract(source_path, f"media/{stored}", os.path.join(folder, stored))
                    moved[original] = os.path.join(folder, stored)
            for ending in manifest["side_files"]:
                package.extract(source_path, f"show/{show[:-4]}{ending}", target[:-4] + ending)
            if moved and os.path.exists(self._screens_path(target)):
                self._repoint_screens(target, moved)
            if moved and os.path.exists(self._pieces_path(target)):
                self._repoint_pieces(target, moved)
            package.extract(source_path, f"show/{show}", target)    # last: the show appears once all it needs is there
        return {"kind": "package", "name": os.path.basename(target), "renamed": renamed, "types_added": types_added,
                "media": len(moved), "warnings": warnings, "made": manifest.get("made", "")}

    def _repoint_pieces(self, mvr_path, moved):
        """Set pieces that wear a picture from where it was on the computer a package was made on
        are given its new place here."""
        target = self._pieces_path(mvr_path)
        try:
            with open(target, encoding="utf-8") as handle:
                kept = json.load(handle)
        except (OSError, ValueError):
            return
        for entry in kept.get("pieces", []) if isinstance(kept, dict) else []:
            if isinstance(entry, dict) and entry.get("picture") in moved:
                entry["picture"] = moved[entry["picture"]]
        with open(target + ".part", "w", encoding="utf-8") as handle:
            json.dump(kept, handle, indent=1)
        os.replace(target + ".part", target)

    def _repoint_screens(self, mvr_path, moved):
        """Screens that show a picture or film from where it was on the computer a package was made
        on are given its new place here."""
        target = self._screens_path(mvr_path)
        try:
            with open(target, encoding="utf-8") as handle:
                kept = json.load(handle)
        except (OSError, ValueError):
            return
        for entry in kept.get("screens", []) if isinstance(kept, dict) else []:
            for content in [entry, *(entry.get("sections") or [])] if isinstance(entry, dict) else []:
                if isinstance(content, dict) and content.get("value") in moved:
                    content["value"] = moved[content["value"]]
        with open(target + ".part", "w", encoding="utf-8") as handle:
            json.dump(kept, handle, indent=1)
        os.replace(target + ".part", target)

    def _show_path(self, name):
        if not isinstance(name, str) or os.path.basename(name) != name or not name.lower().endswith(".mvr"):
            raise Problem("That show is not in the library.")
        path = os.path.join(self.shows_dir, name)
        if not os.path.isfile(path):
            raise Problem(f"{name} is no longer in the library.")
        return path

    def show_detail(self, name):
        """A show's patch as the visualizer will have it: every fixture, what it occupies, and what
        collides. Saved edits are laid over the MVR by the core, as Unreal does."""
        path = self._show_path(name)
        if not os.path.exists(self.inspector):
            raise Problem("The core library is not built, so the patch cannot be read. See System.", code="no_core")
        try:
            done = subprocess.run([self.inspector, "--patch", path, self.fixtures_dir], capture_output=True, text=True, errors="replace", timeout=120)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise Problem(f"The patch could not be read: {error}") from None
        fixtures = []
        by_uuid = {}
        focus_points = []    # the rig file's own, which fixtures can be aimed at
        for line in done.stdout.splitlines():
            parts = line.split("\t")
            if parts[0] == "error" and len(parts) == 2:
                raise Problem(f"{name} cannot be read: {parts[1]}")
            try:
                if parts[0] == "focuspoint" and len(parts) == 6:
                    focus_points.append({"uuid": parts[1], "name": parts[2] or "Focus point", "place": [round(float(part), 3) for part in parts[3:6]]})
                elif parts[0] == "focus" and len(parts) == 3 and parts[1] in by_uuid:
                    by_uuid[parts[1]]["focus"] = parts[2]    # the focus point the rig file aims it at
                elif parts[0] == "fixture" and len(parts) == 14:
                    row = {"uuid": parts[1], "id": parts[2], "name": parts[3], "layer": parts[4], "type": parts[5],
                           "mode": parts[6], "universe": int(parts[7]), "address": int(parts[8]), "slots": int(parts[9]),
                           "notes": [note for note in parts[10].split(",") if note], "problems": [],
                           "weight": float(parts[11]), "power": float(parts[12]), "power_rated": parts[13] == "rated",
                           "place": None, "invert_pan": False, "invert_tilt": False, "has_pan": False, "has_tilt": False, "modes": []}
                    fixtures.append(row)
                    if row["uuid"]:
                        by_uuid[row["uuid"]] = row
                elif parts[0] == "place" and len(parts) == 12 and parts[1] in by_uuid:
                    # Millimetres and degrees in the rig file's coordinates: x toward stage left, y upstage, z up.
                    by_uuid[parts[1]].update(place=[round(float(part), 4) for part in parts[2:8]], invert_pan=parts[8] == "1", invert_tilt=parts[9] == "1",
                                             has_pan=parts[10] == "1", has_tilt=parts[11] == "1")
                elif parts[0] == "modes" and len(parts) >= 4 and parts[1] in by_uuid:
                    by_uuid[parts[1]]["modes"] = [{"name": parts[k], "slots": int(parts[k + 1])} for k in range(2, len(parts) - 1, 2)]
            except ValueError:
                continue
        return dict(check_patch(fixtures), name=name, edited=self._edited(path), totals=rig_totals(fixtures), focus_points=focus_points,
                    groups=self._group_rows(path),
                    open=bool(self._run and self._run["process"].poll() is None and self._run["name"] == name))

    def _change_show(self, name, arguments, what):
        """Has the core change a show's edits file (adding or removing fixtures). Returns its answer lines."""
        path = self._show_path(name)
        if not os.path.exists(self.inspector):
            raise Problem(f"The core library is not built, so {what}. See System.", code="no_core")
        with self._lock:
            # Also while Unreal has the show open: it looks at the show's files twice a second and takes up what changed in them.
            try:
                done = subprocess.run([self.inspector, arguments[0], path, *arguments[1:]], capture_output=True, text=True, errors="replace", timeout=120)
            except (OSError, subprocess.TimeoutExpired) as error:
                raise Problem(f"The show could not be changed: {error}") from None
        lines = [line.split("\t", 1) for line in done.stdout.splitlines() if "\t" in line]
        failed = next((text for kind, text in lines if kind == "error"), None)
        if failed or done.returncode != 0:
            raise Problem(f"The show could not be changed: {failed or 'the core gave no reason'}.")
        return lines

    def add_fixtures(self, name, fixture, mode="", count=1, universe=0, address=1, first_id=0):
        """Adds fixtures of a type from the fixture library to a show. They are kept in the show's
        edits, set down in front of the stage, and placed in the Unreal Editor like any other."""
        if not isinstance(fixture, str) or os.path.basename(fixture) != fixture or not fixture.lower().endswith(".gdtf") or \
                not os.path.isfile(os.path.join(self.fixtures_dir, fixture)):
            raise Problem("That fixture type is not in your fixture library.")
        numbers = {"count": count, "universe": universe, "address": address, "first id": first_id}
        for label, value in numbers.items():
            if isinstance(value, bool) or not isinstance(value, int) or value < 0:
                raise Problem(f"The {label} must be a whole number.")
        if not 1 <= count <= 512:
            raise Problem("Between 1 and 512 fixtures can be added at a time.")
        if universe > 32767 or not 1 <= address <= 512:
            raise Problem("The universe must be 1 to 32767 (0 leaves them unpatched) and the address 1 to 512.")
        lines = self._change_show(name, ["--add", self.fixtures_dir, fixture, str(mode or ""), str(count), str(universe), str(address), str(first_id)],
                                  "fixtures cannot be added")
        return {"added": [text for kind, text in lines if kind == "added"]}

    # What can be set on fixtures of a show, as the core's --set takes it, and what each is.
    _SETTABLE = {"x": "number", "y": "number", "z": "number", "dx": "number", "dy": "number", "dz": "number",
                 "stepx": "number", "stepy": "number", "stepz": "number", "rx": "angle", "ry": "angle", "rz": "angle", "spin": "angle",
                 "aimx": "number", "aimy": "number", "aimz": "number",
                 "universe": "universe", "address": "address", "mode": "text", "name": "text", "number": "whole", "id": "id",
                 "invertpan": "flag", "inverttilt": "flag", "type": "type", "reset": "reset"}

    def set_fixtures(self, name, uuids, changes):
        """Changes fixtures of a show: where they stand and how they are turned (millimetres and
        degrees, the rig file's coordinates) or the point they are aimed at (`aimx/y/z`), their patch, mode, name, number, whether their own
        pan and tilt are mirrored, and their fixture type (`type`, a file of the fixture library). Several fixtures are taken in the order given: a patch runs on
        from one to the next, `stepx/y/z` sets them out in a row, `number` and `id` count up.
        Everything goes into the show's edits through the core; the rig file is not touched, and a
        show that is open in Unreal takes the change up within half a second."""
        if not isinstance(uuids, list) or not uuids or len(uuids) > 2000 or any(not isinstance(uuid, str) or not re.fullmatch(r"[0-9A-Fa-f-]{1,64}", uuid) for uuid in uuids):
            raise Problem("No fixtures were specified.")
        if not isinstance(changes, dict) or not changes:
            raise Problem("Nothing to change was specified.")
        arguments = []
        for key, value in changes.items():
            kind = self._SETTABLE.get(key)
            if kind is None:
                raise Problem(f"Unknown fixture property: {key}.")
            if kind in ("number", "angle"):
                limit = 360 if kind == "angle" else 5_000_000
                if isinstance(value, bool) or not isinstance(value, (int, float)) or value != value or not -limit <= value <= limit:
                    raise Problem("Angles must be between -360 and 360 degrees." if kind == "angle" else "Places are within five kilometres of the middle of the stage.")
                written = f"{value:.6f}".rstrip("0").rstrip(".")
            elif kind in ("universe", "address", "whole"):
                low, high = {"universe": (0, 32767), "address": (1, 512), "whole": (0, 1_000_000)}[kind]
                if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
                    raise Problem({"universe": "A universe is a whole number from 1 to 32767; 0 unpatches.", "address": "An address is a whole number from 1 to 512.",
                                   "whole": "The first number is a whole number."}[kind])
                written = str(value)
            elif kind == "id":
                if value in ("", None):
                    written = ""
                elif isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 100_000_000:
                    raise Problem("A fixture ID must be a whole number, or empty.")
                else:
                    written = str(value)
            elif kind == "flag":
                if not isinstance(value, bool):
                    raise Problem(f"{key} must be on or off.")
                written = "1" if value else "0"
            elif kind == "reset":
                if value not in ("place", "patch", "mode", "name", "invert", "type", "all"):
                    raise Problem("Reset applies to position, patch, mode, name, inversion, type, or all.")
                written = value
            elif kind == "type":
                # A file of the fixture library as these fixtures' own type; nothing: the rig file's type again.
                if not isinstance(value, str) or (value and (os.path.basename(value) != value or not value.lower().endswith(".gdtf"))):
                    raise Problem("Choose a fixture type from your fixture library.")
                if value and not os.path.isfile(os.path.join(self.fixtures_dir, value)):
                    raise Problem(f"{value} is not in the fixture library.")
                written = value
            else:
                if not isinstance(value, str) or len(value) > 200 or any(ord(character) < 32 for character in value) or (key == "mode" and not value):
                    raise Problem("That name cannot be used.")
                written = value.strip() if key == "name" else value
            arguments.append(f"{key}={written}")
        if ("universe" in changes) != ("address" in changes) and changes.get("universe") != 0:
            raise Problem("A patch needs both a universe and an address.")
        if any(key in changes for key in ("aimx", "aimy", "aimz")) and not all(key in changes for key in ("aimx", "aimy", "aimz")):
            raise Problem("Aiming needs a point: X, Y and Z.")
        lines = self._change_show(name, ["--set", self.fixtures_dir, ",".join(uuids), *arguments], "fixtures cannot be changed")
        answer = {"set": [text for kind, text in lines if kind == "set"]}
        if "aimx" in changes:
            # Which were turned toward the point, and why the others were not: those that pan or tilt are aimed by the console.
            answer["aimed"] = [text for kind, text in lines if kind == "aimed"]
            answer["not_aimed"] = [{"uuid": text.split("\t")[0], "why": text.split("\t")[-1]} for kind, text in lines if kind == "notaimed"]
        return answer

    def remove_fixture(self, name, uuid, restore=False):
        """Takes a fixture out of a show, or puts one of the rig file's back in. The MVR is not touched."""
        return self.remove_fixtures(name, [uuid], restore)

    def remove_fixtures(self, name, uuids, restore=False):
        """The same for several fixtures at once, in one change to the show's edits."""
        if not isinstance(uuids, list) or not uuids or len(uuids) > 5000 or any(not isinstance(uuid, str) or not re.fullmatch(r"[0-9A-Fa-f-]{1,64}", uuid) for uuid in uuids):
            raise Problem("That fixture is not in this show.")
        lines = self._change_show(name, ["--restore" if restore else "--remove", ",".join(uuids)], "fixtures cannot be removed")
        done = sum(1 for kind, _ in lines if kind in ("removed", "restored"))
        return {"done": done > 0, "count": done}

    def remove_type(self, name, spec, restore=False):
        """Takes every fixture of one type out of a show, or puts them all back: for what the
        visualizer can do without, such as dimmer channels and media server layers. They stay in the
        rig file and in an export's patch only as the edits say: taken out, they are not exported."""
        path = self._show_path(name)
        if not isinstance(spec, str) or not spec:
            raise Problem("No fixture type was specified.")
        lines = self._change_show(name, ["--restore-type" if restore else "--remove-type", spec], "the fixtures cannot be taken out")
        done = sum(1 for kind, _ in lines if kind in ("removed", "restored"))
        return dict(self.show_types(name), count=done)

    def export_patch(self, name):
        """Saves a show's patch as a spreadsheet (CSV), with each fixture's weight and power and the totals."""
        detail = self.show_detail(name)
        os.makedirs(self.exports_dir, exist_ok=True)
        out = os.path.join(self.exports_dir, f"{name[:-4]} patch {time.strftime('%Y-%m-%d %H%M%S')}.csv")
        remarks = {"unpatched": "no address", "past_end": "runs past the end of its universe", "overlap": "shares addresses",
                   "notype": "no fixture type: left out", "nomode": "mode not found: first mode used", "moved": "moved",
                   "added": "added in Openviz", "removed": "removed: not on stage",
                   "repatched": "re-patched", "remoded": "mode changed"}
        with open(out + ".part", "w", newline="", encoding="utf-8-sig") as handle:    # with the mark Excel wants for UTF-8
            sheet = csv.writer(handle)
            sheet.writerow(["ID", "Name", "Type", "Mode", "Universe", "Address", "Slots", "Layer", "Weight (kg)", "Power (W)", "Power is", "Remarks"])
            for row in detail["fixtures"]:
                out_of_rig = "notype" in row["notes"] or "removed" in row["notes"]
                sheet.writerow([row["id"], row["name"], row["type"].replace("@", " "), row["mode"],
                                "" if out_of_rig or row["universe"] <= 0 else row["universe"],
                                "" if out_of_rig or row["universe"] <= 0 else row["address"], row["slots"] or "", row["layer"],
                                f"{row['weight']:g}" if row["weight"] else "", f"{row['power']:g}" if row["power"] else "",
                                "" if not row["power"] else "rated" if row["power_rated"] else "light sources only",
                                "; ".join(remarks[key] for key in [*row["problems"], *row["notes"]] if key in remarks)])
            totals = detail["totals"]
            sheet.writerow([])
            sheet.writerow(["", f"Total of {totals['fixtures']} fixtures", "", "", "", "", "", "", f"{totals['weight']:g}", f"{totals['power']:g}"])
            if totals["no_weight"]:
                sheet.writerow(["", f"{totals['no_weight']} of them state no weight"])
            if totals["sources_only"]:
                sheet.writerow(["", f"For {totals['sources_only']} of them the power is that of the light sources only"])
        os.replace(out + ".part", out)
        return {"file": os.path.basename(out), "folder": self.exports_dir}

    def plot_choices(self):
        """The paper and the measures a plot is made with: as set, or as this computer's region has them."""
        settings = self.settings()
        paper, units = settings["plot_paper"], settings["plot_units"]
        if "auto" in (paper, units):
            inches, letter = self._region_measures()
            paper = ("tabloid" if letter else "a3") if paper == "auto" else paper
            units = ("imperial" if inches else "metric") if units == "auto" else units
        return paper, units

    def _region_measures(self):
        """(whether this computer's region measures in feet and inches, whether its paper is Letter and not A4). Asked once."""
        if self._region is None:
            region, inches = oslayer.region()
            self._region = (region in INCH_REGIONS if inches is None else inches, region in LETTER_REGIONS)
        return self._region

    def _plot_data(self, name):
        """Where everything in a show is and how big, from the core."""
        path = self._show_path(name)
        if not os.path.exists(self.inspector):
            raise Problem("The core library is not built, so the rig cannot be measured. See System.", code="no_core")
        try:
            done = subprocess.run([self.inspector, "--plot", path, self.fixtures_dir], capture_output=True, text=True, errors="replace", timeout=300)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise Problem(f"The rig could not be measured: {error}") from None
        failed = next((line.split("\t", 1)[1] for line in done.stdout.splitlines() if line.startswith("error\t")), None)
        if failed or done.returncode != 0:
            raise Problem(f"{name} cannot be read: {failed or 'the core gave no reason'}")
        return paperwork.read_plot(done.stdout)

    def _paper_choice(self, what, paper, units):
        if what not in ("plot", "paperwork"):
            raise Problem("Choose the light plot or the paperwork.")
        chosen = self.plot_choices()
        paper = chosen[0] if paper in (None, "", "auto") else paper
        units = chosen[1] if units in (None, "", "auto") else units
        if paper not in paperwork.PAPERS or units not in paperwork.UNITS:
            raise Problem("Unsupported paper size or units.")
        return paper, units

    def _paper_pdf(self, name, what, paper, units):
        """A show's light plot or its lists as PDF bytes, with what was changed about its paper."""
        detail = self.show_detail(name)
        plot = self._plot_data(name)
        for screen in self.screens(name)["screens"]:
            if screen["added"]:    # the rig file's own screens are among its scenery already
                plot["objects"].append(paperwork.standing_screen(screen["name"], screen["width"], screen["height"], screen["x"], screen["y"], screen["z"], screen["turn"]))
        edits = self._paper_edits(self._show_path(name))
        try:
            if what == "plot":
                return paperwork.plot_pdf(name, detail["fixtures"], plot, paper, units, edits=edits)
            return paperwork.paperwork_pdf(name, detail, plot, paper, units, edits=edits)
        except ValueError as error:
            raise Problem(f"Nothing to draw: {error}. Choose at least one sheet or list under Contents on the Paperwork tab.") from None

    def preview_paper(self, name, what, paper=None, units=None):
        """The same as export_paper, as bytes to look at, saved nowhere."""
        paper, units = self._paper_choice(what, paper, units)
        return PdfBytes(self._paper_pdf(name, what, paper, units))

    # --------------------------------------------------------------- what goes on paper

    @staticmethod
    def _paper_path(mvr_path):
        return mvr_path[:-4] + ".paper.json"    # what was changed about a show's plot and paperwork

    def _paper_edits(self, mvr_path):
        try:
            with open(self._paper_path(mvr_path), encoding="utf-8") as handle:
                return paperwork.paper_edits(json.load(handle))
        except (OSError, ValueError):
            return paperwork.paper_edits()

    def paper_settings(self, name):
        """What was changed about a show's plot and paperwork (title block, sheets, notes, labels,
        names in the key, what is written about each fixture), and the choices there are."""
        path = self._show_path(name)
        return {"name": name, "edits": self._paper_edits(path), "sheets": list(paperwork.PAPER_SHEETS), "lists": list(paperwork.PAPER_LISTS),
                "labels": list(paperwork.PAPER_LABELS), "fields": list(paperwork.FIXTURE_FIELDS), "text": list(paperwork.PAPER_TEXT)}

    def set_paper(self, name, changes):
        """Changes what goes on a show's paper. Text fields, "sheets" and "lists" ({key: on}),
        "labels", "types" ({type name: {"name", "hidden"}}) and "fixtures" ({uuid: {"purpose",
        "colour", "note"}}) are laid over what was there; an empty name or field takes it back."""
        path = self._show_path(name)
        if not isinstance(changes, dict) or not changes:
            raise Problem("Nothing to change was specified.")

        def text(value, longest, what):
            if not isinstance(value, str) or len(value) > longest or any(ord(c) < 32 and c not in "\n\t" for c in value):
                raise Problem(f"That value cannot be used as {what}.")
            return value.strip() if "\n" not in value else value.rstrip()

        with self._lock:
            edits = self._paper_edits(path)
            for key, value in changes.items():
                if key in paperwork.PAPER_TEXT:
                    edits[key] = text(value, 4000 if key == "notes" else 200, "text on paper")
                elif key in ("sheets", "lists"):
                    known = paperwork.PAPER_SHEETS if key == "sheets" else paperwork.PAPER_LISTS
                    if not isinstance(value, dict) or any(part not in known or not isinstance(on, bool) for part, on in value.items()):
                        raise Problem("Sheets and lists must be on or off.")
                    edits[key].update(value)
                elif key == "labels":
                    if value not in paperwork.PAPER_LABELS:
                        raise Problem("Labels must be ID and address, ID, address, purpose or none.")
                    edits["labels"] = value
                elif key == "types":
                    if not isinstance(value, dict) or len(value) > 500:
                        raise Problem("No fixture types were specified.")
                    for type_name, entry in value.items():
                        if not isinstance(type_name, str) or not isinstance(entry, dict) or any(part not in ("name", "hidden") for part in entry):
                            raise Problem("Fixture types on paper have a name and a hide setting.")
                        now = edits["types"].setdefault(type_name, {"name": "", "hidden": False})
                        if "name" in entry:
                            now["name"] = text(entry["name"], 100, "a name")
                        if "hidden" in entry:
                            if not isinstance(entry["hidden"], bool):
                                raise Problem("Hide must be on or off.")
                            now["hidden"] = entry["hidden"]
                elif key == "fixtures":
                    if not isinstance(value, dict) or len(value) > 5000:
                        raise Problem("No fixtures were specified.")
                    for uuid, entry in value.items():
                        if not isinstance(uuid, str) or not re.fullmatch(r"[0-9A-Fa-f-]{1,64}", uuid) or not isinstance(entry, dict) or \
                                any(part not in paperwork.FIXTURE_FIELDS for part in entry):
                            raise Problem("Fixture notes are a purpose, a colour and a note.")
                        now = edits["fixtures"].setdefault(uuid, {field: "" for field in paperwork.FIXTURE_FIELDS})
                        for field, words in entry.items():
                            now[field] = text(words, 200, "a fixture's " + field)
                else:
                    raise Problem(f"Unknown paperwork setting: {key}.")
            # Only what differs from how paper is by default is kept.
            usual = paperwork.paper_edits()
            kept = {key: edits[key] for key in paperwork.PAPER_TEXT if edits[key]}
            for key in ("sheets", "lists"):
                if edits[key] != usual[key]:
                    kept[key] = edits[key]
            if edits["labels"] != usual["labels"]:
                kept["labels"] = edits["labels"]
            types = {type_name: entry for type_name, entry in edits["types"].items() if entry["name"] or entry["hidden"]}
            fixtures = {uuid: entry for uuid, entry in edits["fixtures"].items() if any(entry.values())}
            if types:
                kept["types"] = types
            if fixtures:
                kept["fixtures"] = fixtures
            target = self._paper_path(path)
            if kept:
                with open(target + ".part", "w", encoding="utf-8") as handle:
                    json.dump(kept, handle, indent=1)
                os.replace(target + ".part", target)
            elif os.path.exists(target):
                os.remove(target)
        return self.paper_settings(name)

    def export_paper(self, name, what, paper=None, units=None):
        """Saves a show's light plot ("plot") or its lists ("paperwork") as a PDF among the exports."""
        paper, units = self._paper_choice(what, paper, units)
        data = self._paper_pdf(name, what, paper, units)
        os.makedirs(self.exports_dir, exist_ok=True)
        out = os.path.join(self.exports_dir, f"{name[:-4]} {what} {time.strftime('%Y-%m-%d %H%M%S')}.pdf")
        with open(out + ".part", "wb") as handle:
            handle.write(data)
        os.replace(out + ".part", out)
        return {"file": os.path.basename(out), "folder": self.exports_dir, "paper": paper, "units": units}

    def open_export(self, file):
        """Opens something that was saved among the exports, in whatever this computer reads it with."""
        if not isinstance(file, str) or os.path.basename(file) != file or file.startswith("."):
            raise Problem("That file is not in the exports folder.")
        path = os.path.join(self.exports_dir, file)
        if not os.path.isfile(path):
            raise Problem(f"{file} is no longer in the exports folder.")
        try:
            oslayer.open_path(path)    # a file this program wrote itself
        except OSError as error:
            raise Problem(f"{file} could not be opened: {error}") from None
        return {"file": file}

    def found_exports(self):
        """MVR files sitting in the folders other applications export to, not yet in the library."""
        have = {(row["name"].casefold(), row["size"]) for row in self.shows()}
        rows = []
        for application, folder in self.export_folders:
            try:
                names = sorted(os.listdir(folder), key=str.casefold)
            except OSError:
                continue
            for name in names:
                path = os.path.join(folder, name)
                if name.lower().endswith(".mvr") and os.path.isfile(path) and (name.casefold(), os.path.getsize(path)) not in have:
                    rows.append({"application": application, "name": name, "path": path, "size": os.path.getsize(path),
                                 "modified": int(os.path.getmtime(path))})
        return rows

    def import_export(self, path):
        """Copies in one of the files found_exports listed. Nothing else on disk can be named."""
        if not any(row["path"] == path for row in self.found_exports()):
            raise Problem("That file is not among the exports found on this computer.")
        return self.add_file(os.path.basename(path), path)

    @staticmethod
    def _newer_revision(entry, by_name):
        """The newest revision of the same fixture by the same kind of uploader (the manufacturer, or
        the community) uploaded after this one, or None. `by_name`: the list's revisions by
        (manufacturer, fixture), casefolded."""
        same = by_name.get((entry["manufacturer"].casefold(), entry["fixture"].casefold()), [])
        later = [other for other in same if gdtf_share.by_manufacturer(other) == gdtf_share.by_manufacturer(entry) and other["creationDate"] > entry["creationDate"]]
        return max(later, key=lambda other: (other["creationDate"], other["rid"])) if later else None

    def fixtures(self):
        listed = {}
        entries, _, grouped = self._list()
        for entry in entries or []:
            listed[gdtf_share.file_name(entry)] = entry
        by_name = {(revisions[0]["manufacturer"].casefold(), revisions[0]["fixture"].casefold()): revisions for revisions in grouped if revisions}
        rows = []
        for name in sorted(os.listdir(self.fixtures_dir), key=str.casefold):
            path = os.path.join(self.fixtures_dir, name)
            if not (name.lower().endswith(".gdtf") and os.path.isfile(path)):
                continue
            row = dict(self._summary(path), file=name, size=os.path.getsize(path))
            entry = listed.get(name)
            if entry:
                row.update(rid=entry["rid"], revision=entry["revision"], by_manufacturer=gdtf_share.by_manufacturer(entry))
                # The same comparison the command-line downloader prints.
                theirs = {mode["name"]: mode["dmxfootprint"] for mode in entry["modes"] if mode["dmxfootprint"]}
                ours = {" ".join(mode["name"].split()): mode["footprint"] for mode in row["modes"]}
                if theirs and not row.get("error") and not row.get("unchecked"):
                    row["footprints_differ"] = sorted(name for name, slots in theirs.items() if ours.get(name) != slots)
                    row["footprints_checked"] = len(theirs)
                newer = self._newer_revision(entry, by_name)
                if newer:    # GDTF Share has had a newer upload of it since
                    row["update"] = {"rid": newer["rid"], "revision": newer["revision"], "file": gdtf_share.file_name(newer)}
            try:    # made with the Fixture Builder, so it can be edited there
                with zipfile.ZipFile(path) as archive:
                    row["built"] = fixture_builder.DESCRIPTION_FILE in archive.namelist()
            except (zipfile.BadZipFile, OSError):
                row["built"] = False
            rows.append(row)
        return rows

    def add_file(self, name, source_path):
        """Takes in a dropped file, already saved at `source_path`. Returns what became of it."""
        lower = str(name).lower()
        if lower.endswith(package.EXTENSION):
            return self._unpack(name, source_path)
        if lower.endswith(MODEL_ENDINGS):
            return self._add_model(name, source_path)
        extension = next((known for known in (".mvr", ".gdtf", *NOT_READABLE) if lower.endswith(known)), "")
        if lower.endswith(".show.gz"):
            extension = ".show"
        if extension in NOT_READABLE:
            what, how = NOT_READABLE[extension]
            raise Problem(f"{os.path.basename(str(name))} is a {what}. {how}", code="export_needed")
        if extension not in (".mvr", ".gdtf"):
            raise Problem(f"{os.path.basename(str(name))} is not a supported file. Drop an MVR file (.mvr) with a rig, "
                          "a GDTF file (.gdtf) with one fixture type, or a show packed by Openviz (.openviz). grandMA3, "
                          "Vectorworks and Capture can all export MVR.", code="unknown_type")
        wanted = "GeneralSceneDescription.xml" if extension == ".mvr" else "description.xml"
        try:
            with zipfile.ZipFile(source_path) as archive:
                names = archive.namelist()
        except (zipfile.BadZipFile, OSError):
            raise Problem(f"{os.path.basename(str(name))} is damaged or is not really "
                          f"{'an MVR' if extension == '.mvr' else 'a GDTF'} file: it does not open as an archive.") from None
        if wanted not in names:
            raise Problem(f"{os.path.basename(str(name))} has no {wanted} inside, so it is not "
                          f"{'an MVR' if extension == '.mvr' else 'a GDTF'} file.")

        folder = self.shows_dir if extension == ".mvr" else self.fixtures_dir
        with self._lock:
            target = os.path.join(folder, safe_name(name, extension))
            replaced = False
            if os.path.exists(target):
                if _same_file(target, source_path):
                    return {"kind": extension[1:], "name": os.path.basename(target), "already_here": True}
                # A different file under a name already in use: keep both.
                stem = os.path.basename(target)[:-len(extension)]
                number = 2
                while os.path.exists(os.path.join(folder, f"{stem} ({number}){extension}")):
                    number += 1
                target = os.path.join(folder, f"{stem} ({number}){extension}")
                replaced = True
            shutil.copyfile(source_path, target + ".part")
            os.replace(target + ".part", target)
        answer = {"kind": extension[1:], "name": os.path.basename(target), "renamed": replaced}
        if replaced and extension == ".mvr":
            answer["instead_of"] = safe_name(name, extension)    # the show whose name it would have had
        return answer

    def update_show(self, name, newer):
        """Puts a newer MVR of a show in the old one's place: `newer` is a show in the library (as a
        dropped file with a name already in use becomes), `name` the show it is a newer version of.

        What was done to the show in Openviz stays with it: the changes made in the Unreal Editor,
        the stored views, the hidden layers. They are found by each fixture's id in the file, which
        consoles and drawing programs keep from one export to the next. The old MVR is set aside.
        """
        path = self._show_path(name)
        newer_path = self._show_path(newer)
        if path == newer_path:
            raise Problem("That is the same show.")
        with self._lock:
            if self._run and self._run["process"].poll() is None and self._run["name"] in (name, newer):
                raise Problem("Close Unreal first: it has this show open.", code="running")
            before = self._fixture_ids(path)
            after = self._fixture_ids(newer_path)
            kept = f"{path[:-4]}.replaced-{time.strftime('%Y%m%d-%H%M%S')}.mvr.was"
            os.replace(path, kept)
            os.replace(newer_path, path)
            for extra in (".edits.xml", ".views.txt", ".hidden.json", ".screens.json", ".types.xml", ".pieces.json", ".paper.json", ".groups.json"):    # anything done to the newer copy on its own
                stray = newer_path[:-4] + extra
                if os.path.exists(stray):
                    os.replace(stray, stray + ".was")
        edited = set()
        try:
            with open(self._edits_path(path), encoding="utf-8", errors="replace") as handle:
                edited = {found.upper() for found in re.findall(r'<Fixture\s[^>]*UUID="([^"]+)"', handle.read())}
        except OSError:
            pass
        return {"name": name, "kept_as": os.path.basename(kept),
                "fixtures": len(after), "added": len(after - before), "gone": len(before - after),
                "changes_kept": len(edited & after), "changes_lost": len(edited - after)}

    def _fixture_ids(self, path):
        """The ids (UUIDs, upper case) of the fixtures in an MVR, as the core lists them."""
        if not os.path.exists(self.inspector):
            raise Problem("The core library is not built, so the files cannot be compared. See System.", code="no_core")
        try:
            done = subprocess.run([self.inspector, "--patch", path], capture_output=True, text=True, errors="replace", timeout=120)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise Problem(f"{os.path.basename(path)} could not be read: {error}") from None
        if done.returncode != 0:
            raise Problem(f"{os.path.basename(path)} could not be read.")
        return {parts[1].upper() for parts in (line.split("\t") for line in done.stdout.splitlines()) if parts[0] == "fixture" and len(parts) > 1 and parts[1]}

    # ----------------------------------------------------------------------------- GDTF Share

    def _list(self):
        """GDTF Share's list as (entries, time fetched, fixtures). Read once, until the file changes:
        parsing 13,000 revisions on every keystroke of a search would be felt."""
        path = gdtf_share.list_path(self.fixtures_dir)
        try:
            stamp = (os.path.getmtime(path), os.path.getsize(path))
        except OSError:
            return None, 0, []
        with self._lock:
            if self._list_cache is None or self._list_cache[0] != stamp:
                entries, fetched = gdtf_share.load_list(self.fixtures_dir)
                self._list_cache = (stamp, entries, fetched, gdtf_share.group(entries) if entries else [])
            return self._list_cache[1:]

    def share_state(self):
        entries, fetched, _ = self._list()
        user = self.settings()["share_user"] if self.keychain else ""
        return {"logged_in": self._share is not None, "revisions": len(entries) if entries else 0,
                "list_fetched": fetched, "has_list": entries is not None,
                "can_remember": self.keychain is not None, "remembered": bool(user), "user": user}

    def _forget_login(self):
        if self.keychain:
            self.keychain.forget()
        if self.settings()["share_user"]:
            self.update_settings({"share_user": ""})

    def share_login(self, user, password, remember=False):
        if not isinstance(user, str) or not isinstance(password, str) or not user.strip() or not password:
            raise Problem("Enter your GDTF Share user name and password.", code="login_required", status=401)
        user = user.strip()
        try:
            share = gdtf_share.Share(self.share_url)
            share.login(user, password)
            with self._lock:
                self._share = share
            if self._list()[0] is None:
                self.share_refresh()
        except gdtf_share.ShareError as error:
            if error.status == 401:
                raise Problem(str(error), code="login_failed", status=401) from None
            raise Problem(str(error)) from None
        # Only a login that worked is remembered, and only when asked for.
        state_extra = {}
        if remember and self.keychain and self.keychain.save(password):
            self.update_settings({"share_user": user})
        else:
            self._forget_login()
            if remember:
                state_extra["remember_failed"] = True
        return dict(self.share_state(), **state_extra)

    def share_logout(self):
        with self._lock:
            self._share = None
        self._forget_login()
        return self.share_state()

    def _session(self):
        share = self._share
        if share is not None:
            return share
        # "Stay logged in": log in again with the password kept in the Keychain.
        user = self.settings()["share_user"] if self.keychain else ""
        password = self.keychain.load() if user else None
        if not password:
            if user:
                self._forget_login()    # the user name without its password is no use
            raise Problem("Sign in to GDTF Share first.", code="login_required", status=401)
        try:
            share = gdtf_share.Share(self.share_url)
            share.login(user, password)
        except gdtf_share.ShareError as error:
            if error.status == 401:
                self._forget_login()
                raise Problem("GDTF Share no longer accepts the saved sign-in. Sign in again.", code="login_required", status=401) from None
            raise Problem(str(error)) from None
        with self._lock:
            self._share = share
        return share

    def _share_call(self, call):
        for attempt in (1, 2):
            try:
                return call(self._session())
            except gdtf_share.ShareError as error:
                if error.status != 401:
                    raise Problem(str(error)) from None
                with self._lock:
                    self._share = None    # the session ran out: sessions end after two hours unused
                if attempt == 2 or not (self.keychain and self.settings()["share_user"]):
                    raise Problem("Your GDTF Share session has ended. Sign in again.", code="login_required", status=401) from None

    def share_refresh(self):
        entries, timestamp = self._share_call(lambda share: share.fetch_list())
        gdtf_share.save_list(self.fixtures_dir, entries, timestamp, self.share_url)
        return self.share_state()

    def share_search(self, query="", limit=60):
        entries, _, fixtures = self._list()
        if entries is None:
            raise Problem("Sign in to GDTF Share once to download its fixture list.", code="login_required", status=401)
        terms = str(query).split()
        try:
            limit = max(1, min(int(limit), 500))
        except (TypeError, ValueError):
            limit = 60
        found = [revisions for revisions in fixtures if gdtf_share.matches(revisions[0], terms)]
        # The manufacturers' own uploads first, then by name.
        found.sort(key=lambda revisions: (not gdtf_share.by_manufacturer(revisions[0]),
                                          revisions[0]["manufacturer"].casefold(), revisions[0]["fixture"].casefold()))
        rows = []
        for revisions in found[:limit]:
            best = revisions[0]
            rows.append({"rid": best["rid"], "manufacturer": best["manufacturer"], "fixture": best["fixture"],
                         "revision": best["revision"], "by_manufacturer": gdtf_share.by_manufacturer(best),
                         "rating": best["rating"], "created": best["creationDate"], "size": best["filesize"],
                         "modes": [{"name": mode["name"], "footprint": mode["dmxfootprint"]} for mode in best["modes"]],
                         "more": len(revisions) - 1,
                         "have": os.path.exists(os.path.join(self.fixtures_dir, gdtf_share.file_name(best)))})
        return {"rows": rows, "total": len(found)}

    def share_download(self, rid):
        entries = self._list()[0]
        if entries is None:    # no list yet: nothing can be looked up in it, and fetching it wants the login too
            raise Problem("Sign in to GDTF Share first.", code="login_required", status=401)
        entry = next((entry for entry in entries if entry["rid"] == rid), None)
        if entry is None:
            raise Problem(f"Revision {rid} is not in the fixture list.")
        path = os.path.join(self.fixtures_dir, gdtf_share.file_name(entry))
        if not gdtf_share.already_have(path, entry):
            self._share_call(lambda share: share.download(rid, path))
        return next((row for row in self.fixtures() if row["file"] == os.path.basename(path)), {"file": os.path.basename(path)})

    def fixture_thumbnail(self, file):
        """The picture a fixture type's file carries of the fixture (GDTF's thumbnail: PNG, else SVG)."""
        path = self._library_file(file)
        try:
            with zipfile.ZipFile(path) as archive:
                root = ET.fromstring(archive.read("description.xml"))
                fixture = root.find("FixtureType")
                stem = (fixture.get("Thumbnail") if fixture is not None else "") or "thumbnail"
                names = {name.casefold(): name for name in archive.namelist()}
                for ending, kind in ((".png", "image/png"), (".svg", "image/svg+xml")):
                    found = names.get((stem + ending).casefold())
                    if found:
                        data = archive.read(found)
                        if len(data) <= 4 << 20:
                            picture = PictureBytes(data)
                            picture.content_type = kind
                            return picture
        except (zipfile.BadZipFile, KeyError, OSError, ET.ParseError):
            pass
        raise Problem("This fixture type has no picture.", code="no_picture", status=404)

    def update_fixture(self, file):
        """Fetches the newer revision GDTF Share has of a fixture type of the library, and has it
        replace the older one wherever a show uses that in a placeholder's place. The older file is
        kept, and a show whose modes the newer one does not fit keeps it."""
        self._library_file(file)
        row = next((row for row in self.fixtures() if row["file"] == file), None)
        if not row or not row.get("update"):
            raise Problem(f"GDTF Share's fixture list has no newer revision of {bare_type(file)}. Refresh the list after signing in to look again.")
        new = self.share_download(row["update"]["rid"])["file"]
        replaced, kept = [], []
        for show in self.show_names():
            try:
                types = self._types(self._show_path(show))
            except Problem:
                continue
            for entry in types:
                if entry["standin"] != file:
                    continue
                try:
                    with self.changing(show, "Update Fixture Type"):
                        self.set_stand_in(show, entry["spec"], new)
                    replaced.append({"show": show, "type": entry["name"]})
                except Problem as error:
                    kept.append({"show": show, "type": entry["name"], "why": str(error)})
        return {"file": new, "replaced": replaced, "kept": kept}

    # ---------------------------------------------------------------------------------- Unreal

    def command(self, kind, name, mode="", editor=False, extra=(), render=None):
        """The Unreal command line for a show ("show") or a row of one fixture type ("fixture").
        With `render` ({"recording", "out", "rate", "view"}), the show renders a DMX recording to video."""
        settings = self.settings()
        binary = editor_binary(settings["engine"])
        if not os.path.exists(binary):
            raise Problem(f"Unreal Engine was not found in {settings['engine']}. Set its folder under System.", code="no_engine")
        folder, extension = (self.shows_dir, ".mvr") if kind == "show" else (self.fixtures_dir, ".gdtf")
        if kind not in ("show", "fixture") or not isinstance(name, str) or os.path.basename(name) != name or not name.lower().endswith(extension):
            raise Problem("That is not a show or fixture type in the library.")
        path = os.path.join(folder, name)
        if not os.path.isfile(path):
            raise Problem(f"{name} is no longer in the library.")

        log = os.path.join(self.logs_dir, "visualizer.log")
        command = [binary, self.project]
        if not editor:
            command.append("-game")
        if kind == "show":
            command.append(f"-LvShow={path}")
        else:
            command += [f"-LvShowGdtf={path}", f"-LvShowCount={settings['preview_count']}"]
            if mode:
                command.append(f"-LvShowGdtfMode={mode}")
        if settings["protocol"] == "sacn" or (self.relay_enabled and self.relay_possible(settings)):
            # Openviz receives the console's DMX itself (dmx_relay.py) and passes it to Unreal on
            # this computer as Art-Net: sACN because Unreal cannot be told which network to listen
            # on, and both so that Openviz can show and record what arrives.
            via = "sACN" if settings["protocol"] == "sacn" else "Art-Net"
            command += ["-LvProtocol=artnet", "-LvDmxAddress=127.0.0.1", f"-LvDmxVia={via}@{settings['dmx_address']}"]
        else:
            command += ["-LvProtocol=artnet", f"-LvDmxAddress={settings['dmx_address']}"]
        if settings["psn"] and self.relay_enabled and self.relay_possible(settings):
            command.append(f"-LvPsnPort={self.psn_to_port}")    # what Openviz's receiver passes on of the trackers
        command += [f"-LvExposure={settings['exposure']:g}", f"-LvHaze={settings['haze']:g}",
                    f"-LvAmbient={settings['house_lights']:g}", f"-LvBeams={settings['beams']}", f"-LvBeamRes={settings['beam_detail']}", f"-LvBeamShadows={int(settings['beam_shadows'])}", f"-LvAdaptBeams={int(settings['adapt_beams'])}", f"-LvHazeDrift={settings['haze_drift']:g}", f"-LvHazers={int(settings['rig_haze'])}",
                    f"-LvFloor={settings['floor']:g}", f"-LvFloorShade={settings['floor_shade']:g}", f"-LvBloom={settings['bloom']:g}", f"-LvMotionBlur={int(settings['motion_blur'])}",
                    # The engine keeps what a gobo lets through as brightness only unless told at start to keep colour.
                    *(["-ini:Engine:[/Script/Engine.RendererSettings]:r.LightFunctionAtlas.Format=1"] if settings["surface_colour"] else []),
                    f"-LvMouseLook={settings['mouse_look']}", f"-LvLookSpeed={settings['look_speed']:g}",
                    f"-LvMoveSpeed={settings['move_speed'] * 100:g}", f"-LvInvertY={int(settings['invert_y'])}",
                    f"-LvShadows={int(settings['shadows'])}", f"-LvMegaLights={int(settings['megalights'])}",
                    f"-LvIdleLook={0 if render else int(settings['idle_look'])}", f"-LvPictures={self.pictures_dir}", f"-LvVideos={self.videos_dir}",
                    f"-LvFixtureLibrary={self.fixtures_dir}", f"-LvPicture={self._picture_path}", f"-LvCommands={self._commands_path}"]
        if render:
            # What the recording does not drive is dark, as on the night: no fixture shows its idle look.
            command += [f"-LvRender={render['recording']}", f"-LvRenderOut={render['out']}", f"-LvRenderRate={render['rate']}", f"-LvRenderView={render['view']}"]
            if render.get("path"):
                command.append(f"-LvRenderPath={render['path']}")
        self._write_picture(settings)    # as the command line has them: what is changed from now on is what counts
        if not editor:
            width, _, height = settings["resolution"].partition("x")
            if settings["window"] == "fullscreen":
                # On macOS only exclusive fullscreen lets the frame rate go above the display's.
                command.append(f"-ExecCmds=r.SetRes {width}x{height}f")
            else:
                command += ["-windowed", f"-ResX={width}", f"-ResY={height}"]
        command += [f"-abslog={log}", "-nosplash"]
        command += list(extra)
        return command, log

    def launch(self, kind, name, mode="", editor=False, extra=(), render=None):
        if self.build_when_stale and not (self._build and self._build.get("running")):
            stale, changed = self.visualizer_stale()
            if stale:
                # Its code has changed since it was built: built first, then started, from a thread of its own.
                self.command(kind, name, mode, editor, extra, render)    # what is wrong with the request is said now, not after the build
                self.build_visualizer(then={"kind": kind, "name": name, "mode": mode, "editor": editor, "extra": list(extra), "render": render},
                                      reason=changed)
                return self.run_state()
        with self._lock:
            if self._run and self._run["process"].poll() is None:
                raise Problem("The visualizer is already running. Stop it first, or close its window.", code="running")
            command, log = self.command(kind, name, mode, editor, extra, render)
            if os.path.exists(log):
                os.remove(log)
            try:
                process = oslayer.start(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
            except OSError as error:
                raise Problem(f"Unreal could not be started: {error}") from None
            self._run = {"process": process, "kind": kind, "name": name, "editor": bool(editor), "log": log,
                         "started": time.time(), "render": render}
        if not editor:
            # The movies it saves are made into H.264 as they come, whether this page is looked at or not.
            threading.Thread(target=self._watch_videos, args=(self._run,), daemon=True).start()
        if self.relay_enabled:
            self.ensure_relay()
            # Should Openviz be quit while the visualizer runs, the relay stays for as long as it does.
            self._order(while_pid=process.pid)
        return self.run_state()

    # ------------------------------------------------------------------------------ DMX relay

    @staticmethod
    def relay_possible(settings):
        """Whether the console's DMX can go through Openviz. It cannot when Art-Net is sent to this
        computer's own address (127.0.0.1): Unreal has to listen there itself, on the one port
        Art-Net has, and two cannot share it."""
        return not (settings["protocol"] == "artnet" and settings["dmx_address"] == "127.0.0.1")

    def _relay_config(self):
        settings = self.settings()
        if not self.relay_possible(settings):
            return None
        # sACN has to be asked for universe by universe: every one a show in the library uses,
        # by its fixtures or by a screen lit pixel by pixel, and no fewer than 64 so that a
        # console patched lower is heard too.
        universes = 0
        if settings["protocol"] == "sacn":
            rows = self.shows()
            universes = min(2000, max([64] + [row.get("universes", 0) + 1 for row in rows] + [self._pixel_universes(row["name"]) + 1 for row in rows]))
        return settings["protocol"], settings["dmx_address"], universes, (settings["psn_group"], settings["psn_port"]) if settings["psn"] else None

    def _pixel_universes(self, name):
        """The highest universe a show's screens take pixels from; 0 if none does."""
        last = 0
        for row in self._screen_settings(os.path.join(self.shows_dir, name)):
            for content in row["sections"] or [row]:    # split, a screen's sections show in its place
                if content["source"] == "pixels":
                    try:
                        found = pixel_map(content["value"])
                    except Problem:
                        continue
                    last = max(last, found["universe"] + found["universes"] - 1)
        return last

    def ensure_relay(self):
        """Starts the relay, or starts it again if what it should listen to has changed. True if it runs."""
        if not self.relay_enabled:
            return False
        with self._lock:
            wanted = self._relay_config()
            relay = self._relay
            if relay and relay["process"].poll() is None and relay["config"] == wanted:
                return True
            self._stop_relay()
            if wanted is None:
                self._relay_error = ""
                return False
            os.makedirs(self.logs_dir, exist_ok=True)
            for stale in (self._monitor_path, self._control_path):
                if os.path.exists(stale):
                    os.remove(stale)
            self._control = {}
            self._relay_take = None
            protocol, address, universes, tracking = wanted
            command = [sys.executable, RELAY, "--protocol", protocol, "--interface", address, "--universes", str(max(universes, 1)),
                       "--parent-pid", str(os.getpid()), "--monitor", self._monitor_path, "--control", self._control_path, *self.relay_options]
            if tracking:
                command += ["--psn", f"{tracking[0]}:{tracking[1]}", "--psn-to", str(self.psn_to_port)]
            run = self._run
            if run and run["process"].poll() is None:
                command += ["--while-pid", str(run["process"].pid)]
            try:
                with open(os.path.join(self.logs_dir, "dmx-relay.log"), "w") as log:
                    process = oslayer.start(command, stdout=subprocess.PIPE, stderr=log, stdin=subprocess.DEVNULL, text=True)
            except OSError as error:
                self._relay_error = f"The part of Openviz that receives DMX could not be started: {error}"
                return False
            # It says READY once it is listening; if it cannot listen it says why and ends.
            ready = process.stdout.readline().strip() == "READY"
            if not ready:
                process.wait(timeout=5)
                process.stdout.close()
                try:
                    with open(os.path.join(self.logs_dir, "dmx-relay.log"), encoding="utf-8", errors="replace") as log:
                        said = log.read().strip().splitlines()
                except OSError:
                    said = []
                reason = said[-1][7:] if said and said[-1].startswith("error: ") else "it ended without saying why"
                self._relay_error = f"Openviz cannot receive DMX: {reason}. Another program may be holding the port, or the address under Listen on is not one of this computer's."
                return False
            self._relay = {"process": process, "config": wanted, "started": time.time()}
            self._relay_error = ""
            return True

    def _stop_relay(self):
        relay, self._relay = self._relay, None
        if relay:
            if relay["process"].poll() is None:
                relay["process"].terminate()
                try:
                    relay["process"].wait(timeout=5)
                except subprocess.TimeoutExpired:
                    relay["process"].kill()
            relay["process"].stdout.close()

    def _order(self, **changes):
        """Tells the relay something through its control file (see dmx_relay.py)."""
        with self._lock:
            before = dict(self._control)
            for key, value in changes.items():
                if value is None:
                    self._control.pop(key, None)
                else:
                    self._control[key] = value
            if self._control == before and os.path.exists(self._control_path):
                return
            os.makedirs(self.logs_dir, exist_ok=True)
            with open(self._control_path + ".part", "w", encoding="utf-8") as handle:
                json.dump(self._control, handle)
            os.replace(self._control_path + ".part", self._control_path)

    def _relay_running(self):
        relay = self._relay
        return bool(relay and relay["process"].poll() is None)

    def relay_state(self):
        """Whether the console's DMX goes through Openviz, and if not, why not."""
        settings = self.settings()
        state = {"enabled": self.relay_enabled, "possible": self.relay_possible(settings), "running": self._relay_running(),
                 "protocol": settings["protocol"], "address": settings["dmx_address"]}
        if self._relay_error:
            state["error"] = self._relay_error
        return state

    def _monitor(self):
        """What the relay last reported, or None if it is not running or has gone quiet."""
        if not self._relay_running():
            return None
        try:
            with open(self._monitor_path, encoding="utf-8") as handle:
                seen = json.load(handle)
        except (OSError, ValueError):
            return None
        return seen if isinstance(seen, dict) and time.time() - seen.get("written", 0) < 5 else None

    def dmx_monitor(self, universe=None):
        """What is arriving right now, for the live view; with `universe`, that one's levels too."""
        if isinstance(universe, int) and not isinstance(universe, bool) and 0 < universe <= 32767:
            self._order(watch=universe)
        elif universe is None and "watch" in self._control:
            self._order(watch=None)
        return {"relay": self.relay_state(), "monitor": self._monitor(), "record": self.record_state()}

    def close(self, force=False):
        """For when Openviz ends. The relay is left running if the visualizer still is, so that
        its DMX does not stop; it ends by itself when the visualizer does."""
        self.play_stop()
        station, self._station = self._station, None
        if station:
            station.stop()
        listener, self._osc = self._osc, None
        if listener:
            listener.stop()
        recorder = self._recorder
        if recorder:
            recorder.stop()
        run = self._run
        if force or not (run and run["process"].poll() is None):
            self._stop_relay()

    def run_state(self):
        run = self._run
        building = self.build_state()
        if not run or (building and building["running"]):
            return {"running": False, **({"building": building} if building else {})}
        code = run["process"].poll()
        lines = []
        try:
            with open(run["log"], encoding="utf-8", errors="replace") as handle:
                for line in handle:
                    found = re.search(r"LogLvShow: (\w+): LvShow: (.*)", line)
                    if found:
                        text = found.group(2).strip()
                        if text.startswith("screenshot requested"):
                            continue
                        lines.append({"text": text[5:].strip() if text.startswith("note:") else text,
                                      "note": text.startswith("note:") or found.group(1) != "Display"})
        except OSError:
            pass
        # A live recording runs from "video: recording" to "video: saved" or "not saved"; a render says how far it has got.
        filming = None
        progress = None
        metering = False    # light levels shown over the stage: from "light levels on" to "light levels off"
        for line in lines:
            text = line["text"]
            if text.startswith("light levels on"):
                metering = True
            elif text.startswith("light levels off"):
                metering = False
            if text.startswith("video: recording to "):
                filming = time.time()
            elif text.startswith("video: saved ") or text.startswith("video: not "):
                filming = None
            found = re.match(r"render: frame (\d+) of (\d+)", text)
            if found:
                progress = [int(found.group(1)), int(found.group(2))]
        self._follow_videos(run)
        self._follow_panoramas(run)
        render = run.get("render")
        crash = self._crash(run) if code not in (None, 0) and not run.get("stopped") else None
        return {"crash": crash, "running": code is None, "exit_code": code, "kind": run["kind"], "name": run["name"],
                "editor": run["editor"], "started": int(run["started"]), "lines": lines[-60:],
                "stopped": bool(run.get("stopped")), "views": self.stored_views(run["name"]) if run["kind"] == "show" else [],
                "filming": filming is not None and code is None, "metering": metering and code is None,
                "rendering": os.path.basename(render["recording"]) if render else "",
                "progress": progress, "videos": [dict(entry, made=path) for path, entry in self._videos.items() if entry.get("run") == run["started"]],
                "panoramas": [dict(entry) for entry in self._panoramas.values() if entry.get("run") == run["started"]]}

    # ------------------------------------------------------------------------- building Unreal

    def visualizer_stale(self):
        """Whether the visualizer's code (the plugin's sources, and the core's, which the plugin
        compiles in) is newer than its build. (stale, the file that changed last, or why)."""
        project_dir = os.path.dirname(self.project)
        binaries = os.path.join(project_dir, "Plugins", "LightingVisualizer", "Binaries")
        built = [os.path.join(folder, entry) for folder, _, entries in os.walk(binaries) for entry in entries if "LvShow" in entry and not entry.endswith((".pdb", ".modules", ".target"))]
        if not built:
            return True, "it has not been built"
        built_at = min(os.path.getmtime(path) for path in built)
        newest, newest_at = None, 0.0
        sources = [os.path.join(project_dir, "Plugins", "LightingVisualizer", "Source"), os.path.join(REPO, "core", "src"), os.path.join(REPO, "core", "include"),
                   os.path.join(project_dir, "Plugins", "LightingVisualizer", "LightingVisualizer.uplugin")]
        for source in sources:
            files = [source] if os.path.isfile(source) else [os.path.join(folder, entry) for folder, _, entries in os.walk(source) for entry in entries]
            for path in files:
                if path.endswith((".cpp", ".h", ".c", ".hpp", ".cs", ".uplugin", ".inl")):
                    when = os.path.getmtime(path)
                    if when > newest_at:
                        newest, newest_at = path, when
        if newest and newest_at > built_at + 1.0:
            return True, os.path.relpath(newest, REPO) if newest.startswith(REPO) else os.path.basename(newest)
        return False, ""

    def build_visualizer(self, then=None, reason=""):
        """Builds the Unreal project in the background (the engine's own Build script), into
        library/logs/build.log, and when it succeeds starts what `then` says (launch's arguments).
        Refused while any Unreal has the project open."""
        with self._lock:
            if self._build and self._build.get("running"):
                return self.build_state()
            if self._run and self._run["process"].poll() is None:
                raise Problem("Close the visualizer first: its files are in use while it runs.", code="running")
            if oslayer.project_open(self.project):
                raise Problem("Unreal has Openviz's project open (a visualizer or the editor started earlier): close it and try again. "
                              "The visualizer's code has changed since it was built, so it is rebuilt before it starts.", code="project_open")
            command = oslayer.build_arguments(self.settings()["engine"], self.project)
            if not os.path.exists(command[0]):
                raise Problem(f"The engine's build script is not at {command[0]}. Set the engine's folder under System.", code="no_engine")
            os.makedirs(self.logs_dir, exist_ok=True)
            log = os.path.join(self.logs_dir, "build.log")
            self._build = {"running": True, "ok": None, "error": "", "started": int(time.time()), "reason": reason, "log": log,
                           "then": (then or {}).get("name", "")}
            build = self._build
        threading.Thread(target=self._run_build, args=(build, command, then), daemon=True).start()
        return self.build_state()

    def _run_build(self, build, command, then):
        try:
            with open(build["log"], "w") as out:
                code = oslayer.start(command, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL).wait(timeout=3600)
        except (OSError, subprocess.TimeoutExpired) as error:
            code, build["error"] = -1, f"The build could not be run: {error}"
        try:
            with open(build["log"], encoding="utf-8", errors="replace") as handle:
                text = handle.read()
        except OSError:
            text = ""
        ok = code == 0 and "Result: Succeeded" in text
        if not ok and not build["error"]:
            first = next((line.strip() for line in text.splitlines() if re.search(r"\berror\b", line, re.IGNORECASE)), "")
            build["error"] = f"The build failed{': ' + first[:300] if first else ''}. The whole of it is in library/logs/build.log."
        build.update(running=False, ok=ok, finished=int(time.time()))
        if ok and then:
            try:
                self.launch(then["kind"], then["name"], then.get("mode", ""), then.get("editor", False), tuple(then.get("extra", ())), then.get("render"))
            except Problem as problem:
                build["error"] = f"Built, but it could not be started: {problem}"

    def build_state(self):
        """The build in progress or last made: whether it runs, how it ended, and its last step."""
        build = self._build
        if not build:
            return None
        step = ""
        try:
            with open(build["log"], encoding="utf-8", errors="replace") as handle:
                steps = [line.strip() for line in handle if re.match(r"\s*\[\d+/\d+\]", line)]
            step = steps[-1] if steps else ""
        except OSError:
            pass
        return {key: build.get(key) for key in ("running", "ok", "error", "started", "finished", "reason", "then")} | {"step": step}

    # ----------------------------------------------------------------------------------- crashes

    # Unreal's words for a crash in its log, and the lines of the call stack that follow them.
    CRASH_WORDS = ("Critical error", "Fatal error", "Assertion failed", "Unhandled Exception", "appError called", "Ensure condition failed")

    def _crash(self, run):
        """What the log of a visualizer that ended badly says went wrong: the first line saying so,
        and the call stack's first lines in Openviz's own code, if any. None if it says nothing."""
        message = ""
        stack = []
        try:
            with open(run["log"], encoding="utf-8", errors="replace") as handle:
                for line in handle:
                    text = re.sub(r"^\[[^\]]*\]\[[^\]]*\]", "", line.rstrip()).strip()
                    if "[Callstack]" in text:
                        frame = text.split("[Callstack]", 1)[1].strip()
                        frame = re.sub(r"^0x[0-9a-fA-F]+\s+", "", frame)
                        if any(module in frame for module in ("LvShow", "LvFixtures", "LvBenchmark", "VizCore", "LvShowEditor")):
                            stack.append(frame)
                    elif any(word in text for word in self.CRASH_WORDS) or (message.startswith("===") and ": Error: " in text):
                        said = re.sub(r"^\w+: (Error|Fatal|Warning): ", "", text)
                        # "=== Critical error: ===" is only the banner: the line after it says what happened.
                        if not message or (message.startswith("===") and said and not said.startswith("===")):
                            message = said
        except OSError:
            return None
        if not message and not stack:
            return None
        return {"message": message[:400], "where": stack[:4]}

    def crash_report(self):
        """Saves what there is to know about the visualizer's last run, for sending to whoever looks
        into it: its log, and the crash files Unreal and macOS wrote during it. Nothing else of the
        library goes in. Returns the file's name among the exports."""
        run = self._run
        if not run:
            raise Problem("The visualizer has not run since Openviz was opened.")
        started = run["started"] - 5
        found = []
        if os.path.isfile(run["log"]):
            found.append((run["log"], "visualizer.log"))
        crashes = os.path.join(os.path.dirname(self.project), "Saved", "Crashes")
        for folder in (os.path.join(crashes, entry) for entry in (os.listdir(crashes) if os.path.isdir(crashes) else [])):
            if os.path.isdir(folder) and os.path.getmtime(folder) >= started:
                for entry in os.listdir(folder):
                    path = os.path.join(folder, entry)
                    if os.path.isfile(path) and os.path.getsize(path) < 200 << 20:
                        found.append((path, f"Crashes/{os.path.basename(folder)}/{entry}"))
        reports = os.path.expanduser("~/Library/Logs/DiagnosticReports")
        for entry in (os.listdir(reports) if os.path.isdir(reports) else []):
            path = os.path.join(reports, entry)
            if entry.startswith("UnrealEditor") and os.path.isfile(path) and os.path.getmtime(path) >= started:
                found.append((path, f"macOS/{entry}"))
        if not found:
            raise Problem("No crash report can be made: the visualizer left no log.")
        os.makedirs(self.exports_dir, exist_ok=True)
        out = os.path.join(self.exports_dir, f"Openviz report {time.strftime('%Y-%m-%d %H%M%S')}.zip")
        crash = self._crash(run)
        about = {"show": run["name"], "kind": run["kind"], "editor": run["editor"], "started": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(run["started"])),
                 "exit_code": run["process"].poll(), "crash": crash, "system": sys.platform, "files": [name for _, name in found]}
        with zipfile.ZipFile(out + ".part", "w", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("about.json", json.dumps(about, indent=1))
            for path, name in found:
                archive.write(path, name)
        os.replace(out + ".part", out)
        return {"file": os.path.basename(out), "folder": self.exports_dir, "files": len(found)}

    # ----------------------------------------------------------------------------------- videos

    RENDER_RATES = (24, 25, 30, 50, 60)

    @staticmethod
    def camera_path(text, start_view=0):
        """A render's camera path as written in Openviz, "1 1@4 3@8" (view 1, held until four seconds,
        then easing to view 3 by eight), as [(view, seconds), ...]. A view without a time comes first,
        at 0; times only go forward. Empty text: the camera stays at `start_view`."""
        keys = []
        for token in re.split(r"[\s,]+", str(text or "").strip()):
            if not token:
                continue
            found = re.fullmatch(r"(\d)(?:@(\d+(?:[.,]\d+)?)s?)?", token)
            if not found or (found.group(2) is None and keys):
                raise Problem(f"Camera moves are views with times, such as \"1 3@8 5@15\". \"{token}\" is not valid.")
            seconds = float(found.group(2).replace(",", ".")) if found.group(2) else 0.0
            if keys and seconds < keys[-1][1]:
                raise Problem("Times in camera moves must increase.")
            if seconds > 4 * 3600:
                raise Problem("Recordings can be at most four hours long.")
            keys.append((int(found.group(1)), seconds))
        if not keys or keys[0][1] > 0:
            keys.insert(0, (start_view, 0.0))
        return keys

    SOUND_ENDINGS = (".wav", ".mp3", ".m4a", ".aac", ".aif", ".aiff", ".flac", ".caf")

    def render(self, name, recording, rate=30, view=0, path="", sound="", sound_start=0.0):
        """Renders a DMX recording to video on a show: the visualizer opens it, plays the recording
        frame by frame at a steady clock, however long each frame takes to draw, writes every frame
        into a movie, and closes at the end. The movie is the window's size (Picture, Resolution).
        The camera stays at `view`, or follows `path` (camera_path) from one stored view to the next.
        With `sound` (a sound file's full path), the video carries it, from `sound_start` seconds
        into it at the recording's start: the song the recording was made to."""
        self._show_path(name)
        if not isinstance(recording, str) or os.path.basename(recording) != recording or not recording.endswith(".dmxrec"):
            raise Problem("That recording does not exist.")
        take = os.path.join(self.recordings_dir, recording)
        if not os.path.isfile(take):
            raise Problem(f"{recording} is no longer among the recordings.")
        if isinstance(rate, bool) or rate not in self.RENDER_RATES:
            raise Problem("The frame rate must be 24, 25, 30, 50 or 60 fps.")
        if isinstance(view, bool) or not isinstance(view, int) or not 0 <= view <= 9:
            raise Problem("Views are numbered 0 (start) to 9.")
        keys = self.camera_path(path, view)
        stored = set(self.stored_views(name))
        missing = sorted({key for key, _ in keys if key and key not in stored})
        if missing:
            raise Problem(f"{name[:-4]} has no view {' or '.join(str(key) for key in missing)} stored. Store views while watching (hold a number key), then render.")
        if sound:
            if not isinstance(sound, str) or not os.path.isabs(sound) or not sound.lower().endswith(self.SOUND_ENDINGS):
                raise Problem("Choose a sound file (WAV, MP3, AAC, AIFF or FLAC).")
            if not os.path.isfile(sound):
                raise Problem(f"{os.path.basename(sound)} is no longer there.")
            if isinstance(sound_start, bool) or not isinstance(sound_start, (int, float)) or sound_start != sound_start or not -3600 <= sound_start <= 3600:
                raise Problem("Where the sound starts is a number of seconds, within an hour.")
        os.makedirs(self.videos_dir, exist_ok=True)
        stamp = time.strftime('%Y-%m-%d %H%M%S')
        out = os.path.join(self.videos_dir, f"{name[:-4]} - {recording[:-7]} {stamp}.mov")
        settings = {"recording": take, "out": out, "rate": rate, "view": keys[0][0], "path": ",".join(f"{key}@{seconds:g}" for key, seconds in keys)}
        if sound:
            settings.update(sound=self._sound_for_video(sound, os.path.join(self.videos_dir, f".sound {stamp}.wav")), sound_start=float(sound_start))
        return self.launch("show", name, render=settings)

    @staticmethod
    def _sound_for_video(source, target):
        """The sound as a WAV of 16-bit samples, which is what goes into a movie: made by the system's
        converter, or the file itself where it is a WAV and there is none."""
        command = oslayer.sound_converter(source, target)
        if command is None:
            if source.lower().endswith(".wav"):
                return source
            raise Problem("Only WAV files can be used here: Openviz converts other sound files with FFmpeg, which is not installed (winget install ffmpeg).")
        try:
            done = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=600)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise Problem(f"The sound could not be read: {error}") from None
        if done.returncode != 0 or not os.path.isfile(target):
            raise Problem(f"The sound could not be read: {(done.stderr or done.stdout).strip()[-300:] or 'the converter gave no reason'}")
        return target

    def show_names(self):
        """The shows in the library, by name, for choosing one to render on."""
        try:
            return sorted((entry for entry in os.listdir(self.shows_dir) if entry.lower().endswith(".mvr")), key=str.casefold)
        except OSError:
            return []

    def _watch_videos(self, run):
        while run["process"].poll() is None:
            time.sleep(1.0)
            self._follow_videos(run)
            self._follow_panoramas(run)
        self._follow_videos(run)
        self._follow_panoramas(run)
        render = run.get("render")
        if render and os.path.exists(render["out"] + ".part"):
            os.remove(render["out"] + ".part")    # a render that ended before its movie was whole leaves no half a movie

    def _follow_videos(self, run):
        """Finds the movies the visualizer has said it saved, and has each made into H.264 once."""
        try:
            with open(run["log"], encoding="utf-8", errors="replace") as handle:
                saved = [found.group(1) for found in (re.search(r"LvShow: video: saved (.+\.mov): \d+ frames", line) for line in handle) if found]
        except OSError:
            return
        for path in saved:
            with self._lock:
                if path in self._videos:
                    continue
                self._videos[path] = {"state": "converting", "file": os.path.basename(path), "error": "", "run": run["started"]}
            threading.Thread(target=self._convert_video, args=(path, run.get("render")), daemon=True).start()

    def _follow_panoramas(self, run):
        """Finds the six views the visualizer has said it took for a 360-degree picture, and has each set stitched once."""
        try:
            with open(run["log"], encoding="utf-8", errors="replace") as handle:
                saved = [found.groups() for found in (re.search(r"LvShow: panorama: views saved: (.+) \((\d+) wide\)", line) for line in handle) if found]
        except OSError:
            return
        for prefix, width in saved:
            with self._lock:
                if prefix in self._panoramas:
                    continue
                self._panoramas[prefix] = {"state": "stitching", "file": "", "error": "", "run": run["started"]}
            threading.Thread(target=self._stitch_panorama, args=(prefix, int(width)), daemon=True).start()

    def _stitch_panorama(self, prefix, width):
        """The six views made into one 360-degree picture by the core; where the system can, a JPEG
        that says what it is, so that photo sites and players show it as one to look round in."""
        entry = self._panoramas[prefix]
        base = prefix.rstrip()
        base = base[:-len(" view")] if base.endswith(" view") else base
        png = base + ".png"
        try:
            done = subprocess.run([self.inspector, "--panorama", prefix, png, str(width)], capture_output=True, text=True, errors="replace", timeout=600)
            stitched = done.returncode == 0 and os.path.isfile(png)
        except (OSError, subprocess.TimeoutExpired):
            stitched = False
        if not stitched:
            entry.update(state="done", error="The six views could not be made into one picture; they are kept in the pictures folder.")
            return
        for face in range(6):
            with contextlib.suppress(OSError):
                os.remove(f"{prefix}{face}.png")
        made = png
        command = oslayer.jpeg_converter(png, base + ".jpg")
        if command:
            try:
                converted = subprocess.run(command, capture_output=True, timeout=600).returncode == 0 and os.path.isfile(base + ".jpg")
            except (OSError, subprocess.TimeoutExpired):
                converted = False
            if converted:
                with open(base + ".jpg", "rb") as handle:
                    marked = panorama_jpeg(handle.read(), width, width // 2)
                with open(base + ".jpg", "wb") as handle:
                    handle.write(marked)
                os.remove(png)
                made = base + ".jpg"
        entry.update(state="done", file=os.path.basename(made))

    def _convert_video(self, path, render=None):
        entry = self._videos[path]
        target = path[:-4] + ".mp4"
        if render and render.get("sound") and render.get("out") == path and os.path.isfile(path):
            # The render's sound, put in beside the pictures by the core, before the movie is made smaller.
            entry["state"] = "adding sound"
            loud = path[:-4] + " sound.mov"
            try:
                done = subprocess.run([self.inspector, "--movie-sound", path, render["sound"], f"{render.get('sound_start', 0.0):g}", loud],
                                      capture_output=True, text=True, errors="replace", timeout=6 * 3600)
                added = done.returncode == 0 and os.path.isfile(loud)
            except (OSError, subprocess.TimeoutExpired):
                added = False
            if added:
                os.replace(loud, path)
            else:
                entry["error"] = "The sound could not be added, so the video is silent."
                if os.path.exists(loud):
                    os.remove(loud)
            if os.path.basename(render["sound"]).startswith(".sound "):
                os.remove(render["sound"])    # the converted copy made for this render
            entry["state"] = "converting"
        command = oslayer.video_converter(path, target) if os.path.isfile(path) else None
        if not command:    # kept as it was made
            entry.update(state="done", error="" if oslayer.MAC or not os.path.isfile(path) else
                         "Openviz makes MP4s with FFmpeg here, and FFmpeg is not installed (winget install ffmpeg), so the movie is kept "
                         "as it was made: JPEG pictures in a QuickTime file, which VLC plays.")
            return
        try:
            done = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=6 * 3600)
            made = done.returncode == 0 and os.path.isfile(target) and os.path.getsize(target) > 0
        except (OSError, subprocess.TimeoutExpired):
            made = False
        if made:
            os.remove(path)
            entry.update(state="done", file=os.path.basename(target))
        else:
            if os.path.exists(target):
                os.remove(target)
            entry.update(state="done", error=(entry.get("error", "") + " " if entry.get("error") else "") +
                         "It could not be made smaller, so it is kept as it was made (JPEG pictures, which take more room).")

    def videos(self):
        """The videos made, newest first, and those being made into H.264 now."""
        rows = []
        try:
            names = os.listdir(self.videos_dir)
        except OSError:
            names = []
        busy = {os.path.basename(path) for path, entry in self._videos.items() if entry["state"] in ("converting", "adding sound")}
        for entry in names:
            path = os.path.join(self.videos_dir, entry)
            if not entry.lower().endswith((".mp4", ".mov")) or not os.path.isfile(path):
                continue
            stat_result = os.stat(path)
            rows.append({"name": entry, "size": stat_result.st_size, "made": int(stat_result.st_mtime), "converting": entry in busy})
        rows.sort(key=lambda row: -row["made"])
        return rows

    # What can be asked of a visualizer that is open: a view (a stored one, the start, or one of
    # the flat ones), storing the present view, a picture at the window's size or larger.
    PICTURE_SIZES = ("1920x1080", "2560x1440", "3840x2160", "7680x4320")

    def visualizer(self, command, value=""):
        """Asks the visualizer that is open for a view or a picture (LvShow's -LvCommands file)."""
        value = "" if value is None else value
        if command == "view":
            ok = value in ("top", "front", "side") or (isinstance(value, str) and len(value) == 1 and value.isdigit())
        elif command == "store":
            ok = isinstance(value, str) and len(value) == 1 and value in "123456789"
        elif command == "picture":
            ok = value == "" or value in self.PICTURE_SIZES
        elif command == "record":
            ok = value in ("start", "stop")
        elif command == "panorama":
            ok = value in ("4096", "8192")
        elif command == "meter":
            ok = value in ("on", "off", "toggle")
        else:
            ok = False
        if not ok:
            raise Problem("The visualizer does not support that command.")
        with self._lock:
            run = self._run
            if not run or run["process"].poll() is not None or run["kind"] != "show" or run["editor"]:
                raise Problem("Open a show with Watch Live first.", code="not_running")
            self._command_serial += 1
            with open(self._commands_path + ".part", "w", encoding="utf-8") as handle:
                json.dump({"serial": self._command_serial, "command": command, "value": value}, handle)
            os.replace(self._commands_path + ".part", self._commands_path)
        return {"asked": command, "value": value}

    # ------------------------------------------------------------------------------------- OSC

    # Openviz's addresses, and what each takes. Numbers may come as integers or floats; a control
    # surface's buttons often send 1.0 when pressed and 0.0 when let go, and the letting go is ignored.
    # Whatever stands in front of "/openviz/" is ignored, so a prefix the sender adds does no harm.
    OSC_ADDRESSES = [
        ("/openviz/view", "1 to 9: a stored view; 0: the start; or the word top, front or side"),
        ("/openviz/view/top", "a button: the plan from above"), ("/openviz/view/front", "a button: the front elevation"),
        ("/openviz/view/side", "a button: the side elevation"),
        ("/openviz/store", "1 to 9: store the present view there"),
        ("/openviz/picture", "a button: a picture as on screen; 4 or 8: at 4K or 8K"),
        ("/openviz/panorama", "a button: a 360-degree picture from where the camera stands, 8192 wide; 4: 4096 wide"),
        ("/openviz/record", "a toggle: 1 starts recording a video, 0 stops it"),
        ("/openviz/meter", "a toggle: 1 shows the light levels on stage in lux, 0 hides them"),
        ("/openviz/exposure", "stops, -16 to 8"),
        ("/openviz/haze", "0 to 1 as a fader, or 2 to 250 as the percentage the Visualizer page shows"),
        ("/openviz/drift", "0 (even) to 1 (heavy)"),
        ("/openviz/houselights", "0 to 1 as a fader, or 2 to 100 percent"),
        ("/openviz/beams", "the word shafts or glow; or 1 for shafts, 0 for glow"),
        ("/openviz/beamdetail", "50, 75 or 100 percent (the nearest of them is taken)"),
    ]

    def osc_apply(self):
        """Starts or stops listening for OSC to match the settings."""
        with self._lock:
            settings = self.settings()
            wanted = self._osc_allowed and settings["osc"]
            listener = self._osc
            if listener and (not wanted or listener.port != settings["osc_port"]):
                listener.stop()
                self._osc = listener = None
            self._osc_trouble = ""
            if wanted and not listener:
                listener = osc.Listener(settings["osc_port"], self.osc_handle)
                try:
                    listener.start()
                    self._osc = listener
                except OSError as error:
                    self._osc_trouble = f"Port {settings['osc_port']} cannot be listened on ({error.strerror or error}): something else has it. Choose another."
        return self.osc_state()

    def psn_state(self):
        """PosiStageNet: whether it is heard, where, and the trackers and systems heard just now."""
        settings = self.settings()
        monitor = self._monitor() if settings["psn"] else None
        relay = self.relay_state()
        state = {"on": settings["psn"], "group": settings["psn_group"], "port": settings["psn_port"], "relay": relay,
                 "running": bool(settings["psn"] and relay["running"]), "trackers": [], "systems": []}
        if monitor and isinstance(monitor.get("psn"), dict):
            state["trackers"] = monitor["psn"].get("trackers", [])
            state["systems"] = monitor["psn"].get("systems", [])
        return state

    def osc_state(self):
        settings = self.settings()
        listener = self._osc
        return {"on": settings["osc"], "port": settings["osc_port"], "running": bool(listener and listener.running), "trouble": self._osc_trouble,
                "addresses": [{"address": address, "takes": takes} for address, takes, *_ in self.OSC_ADDRESSES], "heard": list(self._osc_heard[-12:]),
                "local": [address for address in local_addresses() if not address.startswith("127.")]}

    def osc_handle(self, address, arguments, sender=("", 0)):
        """Does what an OSC message asks. Returns what came of it, as a line for the page."""
        def first_number():
            for value in arguments:
                if isinstance(value, bool):
                    return 1.0 if value else 0.0
                if isinstance(value, (int, float)):
                    return float(value)
                if isinstance(value, str):
                    try:
                        return float(value)
                    except ValueError:
                        continue
            return None

        def first_word():
            for value in arguments:
                if isinstance(value, str):
                    return value.strip().lower()
            return None

        path = address.strip().lower().rstrip("/")
        if "/openviz/" in path:
            path = path[path.index("/openviz/"):]    # grandMA3 puts its line's Prefix in front, "/gma3" say
        number = first_number()
        word = first_word()
        try:
            if not path.startswith("/openviz/"):
                result = "not for Openviz: its addresses begin /openviz/"
            elif number == 0.0 and arguments and len(arguments) == 1 and not isinstance(arguments[0], str) and path in (
                    "/openviz/view/top", "/openviz/view/front", "/openviz/view/side", "/openviz/picture", "/openviz/panorama"):
                result = "a button let go: nothing to do"
            elif path in ("/openviz/view/top", "/openviz/view/front", "/openviz/view/side"):
                self.visualizer("view", path.rsplit("/", 1)[1])
                result = f"view {path.rsplit('/', 1)[1]}"
            elif path == "/openviz/view":
                if word in ("top", "front", "side"):
                    value = word
                elif word in ("start", "home"):
                    value = "0"
                elif number is not None and number == int(number) and 0 <= number <= 9:
                    value = str(int(number))
                else:
                    raise Problem("Views are 0 to 9, top, front or side.")
                self.visualizer("view", value)
                result = f"view {value}"
            elif path == "/openviz/store":
                if number is None or number != int(number) or not 1 <= number <= 9:
                    raise Problem("Views can be stored under 1 to 9.")
                self.visualizer("store", str(int(number)))
                result = f"view stored as {int(number)}"
            elif path == "/openviz/record":
                if word in ("start", "stop"):
                    value = word
                elif number is not None:
                    value = "start" if number >= 0.5 else "stop"
                else:
                    raise Problem("Send 1 to start recording and 0 to stop.")
                self.visualizer("record", value)
                result = "recording a video" if value == "start" else "video stopped"
            elif path == "/openviz/picture":
                size = {4.0: "3840x2160", 8.0: "7680x4320"}.get(number, "") if number not in (None, 1.0) else ""
                if word and word in self.PICTURE_SIZES:
                    size = word
                self.visualizer("picture", size)
                result = f"picture {size or 'as on screen'}"
            elif path == "/openviz/meter":
                if word in ("on", "off", "toggle"):
                    value = word
                elif number is not None:
                    value = "on" if number >= 0.5 else "off"
                else:
                    raise Problem("Send 1 to show the light levels and 0 to hide them.")
                self.visualizer("meter", value)
                result = f"light levels {value}"
            elif path == "/openviz/panorama":
                width = "4096" if number == 4.0 or word == "4096" else "8192"
                self.visualizer("panorama", width)
                result = f"360-degree picture {width} wide"
            elif path in ("/openviz/exposure", "/openviz/haze", "/openviz/drift", "/openviz/houselights", "/openviz/beamdetail"):
                if number is None:
                    raise Problem("A number is expected.")
                key = {"/openviz/exposure": "exposure", "/openviz/haze": "haze", "/openviz/drift": "haze_drift", "/openviz/houselights": "house_lights",
                       "/openviz/beamdetail": "beam_detail"}[path]
                # Faders send 0 to 1; the Visualizer page shows haze and work light as percentages.
                # Haze's 100 percent there is a density of 0.2, and the most it takes is 0.5.
                if key == "haze":
                    number = number * 0.2 if number <= 1.0 else number / 100.0 * 0.2
                    number = max(0.0, min(0.5, number))
                if key == "house_lights":
                    number = max(0.0, min(1.0, number if number <= 1.0 else number / 100.0))
                if key == "haze_drift":
                    number = max(0.0, min(1.0, number))
                if key == "beam_detail":
                    number = min((100, 75, 50), key=lambda step: abs(step - number))
                self.update_settings({key: int(number) if key == "beam_detail" else round(number, 4)})
                now = self.settings()[key]
                result = {"haze": f"haze {now * 500:.0f}%", "house_lights": f"house lights {now * 100:.0f}%", "haze_drift": f"drifts {now:g}",
                          "beam_detail": f"beam detail {now}%", "exposure": f"exposure {now:g} stops"}[key]
            elif path == "/openviz/beams":
                value = {"shafts": "cone", "cone": "cone", "glow": "fog", "fog": "fog"}.get(word or "")
                if value is None and number is not None:
                    value = "cone" if number >= 0.5 else "fog"
                if value is None:
                    raise Problem("Beams must be volumetric or fog.")
                self.update_settings({"beams": value})
                result = f"beams as {'shafts' if value == 'cone' else 'glow'}"
            else:
                result = "an address Openviz does not have"
        except Problem as problem:
            result = f"not done: {problem}"
        shown = ", ".join(str(round(value, 3)) if isinstance(value, float) else str(value) for value in arguments if not isinstance(value, bytes))
        self._osc_heard.append({"address": address, "arguments": shown, "from": sender[0] if sender else "", "at": time.strftime("%H:%M:%S"), "result": result})
        del self._osc_heard[:-50]
        return result

    def stored_views(self, name):
        """Which of 1 to 9 a show has a view stored under (the visualizer keeps them beside the show)."""
        try:
            with open(self._show_path(name)[:-4] + ".views.txt", encoding="utf-8", errors="replace") as handle:
                return sorted({int(line.split()[0]) for line in handle if line.split() and line.split()[0].isdigit() and 1 <= int(line.split()[0]) <= 9})
        except (OSError, Problem):
            return []

    def stop(self):
        self.play_stop()    # nothing to play to once the visualizer is closed
        run = self._run
        if run and run["process"].poll() is None:
            run["stopped"] = True    # ended from here, whatever exit code that leaves
            run["process"].terminate()
            try:
                run["process"].wait(timeout=8)
            except subprocess.TimeoutExpired:
                run["process"].kill()
                run["process"].wait(timeout=8)
            render = run.get("render")
            if render and os.path.exists(render["out"] + ".part"):
                os.remove(render["out"] + ".part")    # a render stopped half-way leaves no half a movie
        return self.run_state()

    # ------------------------------------------------------------------------------------- DMX

    def dmx_check(self, seconds=2.0, port=None):
        """Listens the way the visualizer will and reports which universes have data arriving."""
        seen = self._monitor() if port is None else None
        if seen is not None:
            # Openviz is receiving already: say what it sees, without opening the port a second time.
            rows = [{"universe": row["universe"], "packets": 0, "rate": row["rate"], "slots_in_use": row["slots_in_use"],
                     "sources": [source["address"] for source in row["sources"]]} for row in seen["universes"]]
            return {"protocol": seen["protocol"], "address": seen["interface"], "seconds": 0.5, "universes": rows, "other_packets": seen["other"]}
        with self._lock:
            if self._run and self._run["process"].poll() is None:
                raise Problem("Close Unreal first: it is using the DMX port.", code="running")
        settings = self.settings()
        protocol = settings["protocol"]
        seconds = max(0.2, min(float(seconds), 10.0))
        port = port or (verify_capture.ARTNET_PORT if protocol == "artnet" else verify_capture.SACN_PORT)
        # sACN is sent to one multicast group per universe; a receiver has to ask for each. Asked
        # for here: every universe a show in the library uses, and no fewer than the first 64.
        highest = max([64] + [row.get("universes", 0) for row in self.shows()])
        groups = [f"239.255.{u >> 8}.{u & 255}" for u in range(1, min(highest, 512) + 1)] if protocol == "sacn" else []
        try:
            # The wildcard address, so broadcast and unicast both arrive, as they will for Unreal.
            sock = verify_capture.open_receiver(port, bind="", multicast_groups=groups, interface=settings["dmx_address"])
        except OSError as error:
            raise Problem(f"This computer's {'Art-Net' if protocol == 'artnet' else 'sACN'} port ({port}) cannot be opened: {error}. "
                          "Another program may be holding it, or the address under Network Interface on is not one of this computer's.",
                          code="port_busy") from None
        seen = {}
        other = 0
        deadline = time.monotonic() + seconds
        try:
            while True:
                left = deadline - time.monotonic()
                if left <= 0:
                    break
                if not select.select([sock], [], [], min(left, 0.1))[0]:
                    continue
                while True:
                    try:
                        data, source = sock.recvfrom(2048)
                    except BlockingIOError:
                        break
                    found = dmx_record.levels_of(protocol, data)    # as a fixture would read it
                    if not found:
                        other += 1    # ArtPoll, sync and so on: traffic, but not levels
                        continue
                    row = seen.setdefault(found[0], {"universe": found[0], "packets": 0, "sources": [], "slots_in_use": 0})
                    row["packets"] += 1
                    if source[0] not in row["sources"]:
                        row["sources"].append(source[0])
                    row["slots_in_use"] = sum(1 for level in found[1] if level)
        finally:
            sock.close()
        universes = [dict(row, rate=round(row["packets"] / seconds, 1)) for row in sorted(seen.values(), key=lambda row: row["universe"])]
        return {"protocol": protocol, "address": settings["dmx_address"], "seconds": seconds, "universes": universes, "other_packets": other}

    # ------------------------------------------------------------------- recording and playback

    def record_start(self, seconds=600.0, port=None):
        """Starts recording the DMX that arrives, as the visualizer would receive it."""
        settings = self.settings()
        protocol = settings["protocol"]
        if port is None and self._relay_running():
            # The relay sees everything that is passed to the visualizer: it keeps the recording,
            # whether or not a show is open.
            with self._lock:
                if self._relay_take:
                    raise Problem("A recording is already in progress.", code="recording")
                os.makedirs(self.recordings_dir, exist_ok=True)
                self._relay_take = os.path.join(self.recordings_dir, f"{time.strftime('%Y-%m-%d %H%M%S')}.dmxrec")
                number = 2
                while os.path.exists(self._relay_take):
                    self._relay_take = os.path.join(self.recordings_dir, f"{time.strftime('%Y-%m-%d %H%M%S')} ({number}).dmxrec")
                    number += 1
                self._relay_take_started = time.time()
            self._order(record=self._relay_take)
            return self.record_state()
        with self._lock:
            if self._recorder and self._recorder.is_alive():
                raise Problem("A recording is already in progress.", code="recording")
            if protocol == "artnet" and port is None and self._run and self._run["process"].poll() is None:
                # sACN is multicast and reaches every listener; Art-Net to this computer reaches only one.
                raise Problem("Close Unreal first: while it is open, Art-Net sent to this computer goes to it and not to a recording.", code="running")
            port = port or (verify_capture.ARTNET_PORT if protocol == "artnet" else verify_capture.SACN_PORT)
            highest = max([64] + [row.get("universes", 0) for row in self.shows()])
            groups = [f"239.255.{u >> 8}.{u & 255}" for u in range(1, min(highest, 512) + 1)] if protocol == "sacn" else []
            try:
                sock = verify_capture.open_receiver(port, bind="", multicast_groups=groups, interface=settings["dmx_address"])
            except OSError as error:
                raise Problem(f"This computer's {'Art-Net' if protocol == 'artnet' else 'sACN'} port ({port}) cannot be opened: {error}.",
                              code="port_busy") from None
            os.makedirs(self.recordings_dir, exist_ok=True)
            path = os.path.join(self.recordings_dir, f"{time.strftime('%Y-%m-%d %H%M%S')}.dmxrec")
            number = 2
            while os.path.exists(path):    # two within one second
                path = os.path.join(self.recordings_dir, f"{time.strftime('%Y-%m-%d %H%M%S')} ({number}).dmxrec")
                number += 1
            self._recorder = dmx_record.Recorder(sock, protocol, path, max(1.0, min(float(seconds), dmx_record.LONGEST)))
            self._recorder.start()
        return self.record_state()

    def record_stop(self):
        take = self._relay_take
        if take:
            self._order(record=None)
            deadline = time.time() + 5    # the relay finishes the file when it next reads its orders
            while not os.path.exists(take) and time.time() < deadline and self._relay_running():
                time.sleep(0.05)
            self._relay_take = None
            if not os.path.exists(take):
                raise Problem("The recording could not be saved: Openviz's DMX receiver did not finish it.")
            facts = dmx_record.summary(take)
            return {"recording": False, "saved": os.path.basename(take), "seconds": facts["seconds"], "universes": facts["universes"],
                    "frames": facts["frames"]}
        with self._lock:
            recorder = self._recorder
        if not recorder:
            return {"recording": False}
        recorder.stop()
        with self._lock:
            self._recorder = None
        if recorder.error:
            raise Problem(f"The recording could not be saved: {recorder.error}")
        return {"recording": False, "saved": os.path.basename(recorder.path), "seconds": round(recorder.seconds(), 1),
                "universes": len(recorder.universes), "frames": recorder.frames}

    def record_state(self):
        recorder = self._recorder
        state = {"recording": bool(recorder and recorder.is_alive())}
        if self._relay_take:
            seen = self._monitor() or {}
            taking = seen.get("recording") or {}
            state.update(recording=True, name=os.path.basename(self._relay_take), seconds=taking.get("seconds", round(time.time() - self._relay_take_started, 1)),
                         universes=taking.get("universes", 0), frames=taking.get("frames", 0), limit=dmx_record.LONGEST)
        if recorder:
            state.update(name=os.path.basename(recorder.path), seconds=round(recorder.seconds(), 1), universes=len(recorder.universes),
                         frames=recorder.frames, limit=recorder.limit)
        player = self._player
        state["playing"] = self._playing if player and player.is_alive() else ""
        if state["playing"]:
            state.update(position=round(player.position, 1), length=round(player.length, 1), rounds=player.rounds)
        return state

    def recordings(self):
        """The recordings in the library, newest first, with how long each is and what it holds."""
        rows = []
        try:
            names = sorted(os.listdir(self.recordings_dir), reverse=True)
        except OSError:
            names = []
        for name in names:
            path = os.path.join(self.recordings_dir, name)
            if not name.endswith(".dmxrec") or not os.path.isfile(path):
                continue
            stat = os.stat(path)
            known = self._takes.get(path)
            if not known or known[:2] != (stat.st_mtime, stat.st_size):
                try:
                    facts = dmx_record.summary(path)
                except (OSError, ValueError):
                    facts = {"error": "this is not a recording Openviz can read"}
                known = (stat.st_mtime, stat.st_size, facts)
                self._takes[path] = known
            rows.append(dict(known[2], name=name, size=stat.st_size))
        return rows

    def trim_recording(self, name, start, end):
        """Keeps a part of a recording as a recording of its own, `start` to `end` seconds: the
        chorus of a song, say, to render. The recording itself is left as it is."""
        if not isinstance(name, str) or os.path.basename(name) != name or not name.endswith(".dmxrec"):
            raise Problem("That recording is not in the library.")
        path = os.path.join(self.recordings_dir, name)
        if not os.path.isfile(path):
            raise Problem(f"{name} is no longer in the library.")
        for value in (start, end):
            if isinstance(value, bool) or not isinstance(value, (int, float)) or value != value or value < 0:
                raise Problem("The start and the end are numbers of seconds, 0 or more.")
        if end <= start:
            raise Problem("The end must come after the start.")
        stem = name[:-len(".dmxrec")]
        target = os.path.join(self.recordings_dir, f"{stem} {start:g}-{end:g}s.dmxrec")
        try:
            facts = dmx_record.trim(path, target, float(start), float(end))
        except (OSError, ValueError) as error:
            raise Problem(f"The part could not be kept: {error}") from None
        return dict(facts, name=os.path.basename(target))

    def play_start(self, name, loop=True, to=None):
        """Plays a recording to the visualizer, as Art-Net, the way a console would send it."""
        if not isinstance(name, str) or os.path.basename(name) != name or not name.endswith(".dmxrec"):
            raise Problem("That recording is not in the library.")
        path = os.path.join(self.recordings_dir, name)
        if not os.path.isfile(path):
            raise Problem(f"{name} is no longer in the library.")
        self.play_stop()
        settings = self.settings()
        # Behind Openviz's relay the visualizer listens on this computer only.
        behind = settings["protocol"] == "sacn" or (self.relay_enabled and self.relay_possible(settings))
        to = to or ("127.0.0.1" if behind else settings["dmx_address"], verify_capture.ARTNET_PORT)
        try:
            player = dmx_record.Player(path, to, loop=bool(loop))
        except (OSError, ValueError) as error:
            raise Problem(f"{name} cannot be played: {error}") from None
        if not player.records:
            raise Problem(f"{name} is empty: nothing arrived while it was recorded.")
        with self._lock:
            self._player = player
            self._playing = name
        player.start()
        return self.record_state()

    def play_stop(self):
        with self._lock:
            player, self._player, self._playing = self._player, None, ""
        if player and player.is_alive():
            player.stop()
        return self.record_state()

    # ---------------------------------------------------------------------------------- parity

    def parity(self, tracker=PARITY_TRACKER):
        """Where the project stands against Depence and Capture, read from docs/parity-tracker.md."""
        sections = []
        try:
            with open(tracker, encoding="utf-8") as handle:
                lines = handle.read().splitlines()
        except OSError:
            raise Problem("The parity tracker (docs/parity-tracker.md) is not there.") from None
        for line in lines:
            if line.startswith("## "):
                sections.append({"name": line[3:].strip(), "rows": []})
            elif line.startswith("|") and sections:
                cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
                if len(cells) < 4 or cells[0] == "Feature" or not cells[0].strip("-: "):
                    continue
                status = "|".join(cells[3:]).strip()
                state, _, note = status.partition(":")
                state = state.strip().lower()
                if state not in ("todo", "stock", "wip", "done"):
                    state, note = "todo", status
                sections[-1]["rows"].append({"feature": cells[0].replace("`", ""), "has": cells[1], "phase": cells[2],
                                             "state": state, "note": note.strip().replace("`", "")})
        sections = [section for section in sections if section["rows"]]
        totals = {state: sum(1 for section in sections for row in section["rows"] if row["state"] == state)
                  for state in ("done", "wip", "stock", "todo")}
        return {"sections": sections, "totals": totals}

    def reveal(self, what):
        folder = {"shows": self.shows_dir, "fixtures": self.fixtures_dir, "logs": self.logs_dir, "exports": self.exports_dir,
                  "pictures": self.pictures_dir, "recordings": self.recordings_dir, "videos": self.videos_dir, "models": self.models_dir}.get(what)
        if folder:
            os.makedirs(folder, exist_ok=True)    # exports and pictures only exist once something was saved
        if not folder:
            raise Problem("There is no such folder.")
        opener = "open" if sys.platform == "darwin" else "explorer" if sys.platform == "win32" else "xdg-open"
        try:
            subprocess.Popen([opener, folder], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except OSError as error:
            raise Problem(f"The folder could not be opened: {error}") from None
        return {"folder": folder}


def panorama_jpeg(jpeg, width, height):
    """A JPEG with the XMP that says it is a 360-degree picture (Google's GPano, which photo sites,
    players and headsets read), put just after its start."""
    if jpeg[:2] != b"\xff\xd8":
        return jpeg
    xmp = ('<?xpacket begin="\ufeff" id="W5M0MpCehiHzreSzNTczkc9d"?><x:xmpmeta xmlns:x="adobe:ns:meta/">'
           '<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"><rdf:Description rdf:about="" '
           'xmlns:GPano="http://ns.google.com/photos/1.0/panorama/" GPano:ProjectionType="equirectangular" GPano:UsePanoramaViewer="True" '
           f'GPano:FullPanoWidthPixels="{width}" GPano:FullPanoHeightPixels="{height}" GPano:CroppedAreaImageWidthPixels="{width}" '
           f'GPano:CroppedAreaImageHeightPixels="{height}" GPano:CroppedAreaLeftPixels="0" GPano:CroppedAreaTopPixels="0"/>'
           '</rdf:RDF></x:xmpmeta><?xpacket end="w"?>').encode("utf-8")
    body = b"http://ns.adobe.com/xap/1.0/\x00" + xmp
    return jpeg[:2] + b"\xff\xe1" + (len(body) + 2).to_bytes(2, "big") + body + jpeg[2:]


def bare_type(spec):
    """A fixture type's name as the rig file writes it, without its ".gdtf"."""
    return spec[:-5] if spec.lower().endswith(".gdtf") else spec


def type_words(text):
    """The words of a fixture's name for telling like from like: lower case, letters and figures
    apart ("MH6" and "MH 6" are the same two words), without the words that say nothing."""
    words = re.findall(r"[a-z]+|\d+", text.lower())
    return [word for word in words if word not in ("dummy", "gdtf", "professional", "lighting", "pro", "the")]


def likeness(wanted, offered):
    """How like a fixture's name another is, 0 to 1: the share of the wanted name's words the other
    has, the words of the fixture's own name (after the "@") counting double those of its maker."""
    maker, _, model = wanted.partition("@")
    their = set(type_words(offered))
    if not model:
        maker, model = "", wanted
    weights = [(word, 2.0) for word in type_words(model)] + [(word, 1.0) for word in type_words(maker)]
    total = sum(weight for _, weight in weights)
    if not total or not their:
        return 0.0
    # Words the other name has and this one lacks count a little against it: "MAC Viper" is not "MAC Viper AirFX".
    extra = len(their - {word for word, _ in weights})
    return max(0.0, sum(weight for word, weight in weights if word in their) / total - 0.05 * extra)


# What a screen can show, and the files the visualizer can put on one: pictures through the
# engine's image readers, films through the system's own player (H.264, HEVC and ProRes on a Mac).
SCREEN_SOURCES = ("pattern", "picture", "film", "ndi", "pixels", "off")
PIXEL_WIRINGS = ("rows", "snake", "columns", "snake-columns")
MAX_SPLIT = 16    # a screen is split into at most this many sections each way


def pixel_map(text):
    """A screen lit pixel by pixel over DMX, read from its line: "<universe>.<address> <across>x<down>
    [order] [wiring]", as the visualizer reads it (vizcore/screens.h: keep the two alike). Returns
    its parts, the universes it reaches over and the line as it is kept. Raises Problem."""
    words = text.split() if isinstance(text, str) else []
    where = re.fullmatch(r"(\d{1,7})\.(\d{1,7})", words[0]) if words else None
    if not where:
        raise Problem("Enter the first pixel's patch as universe.address, for example 20.1.")
    universe, address = int(where.group(1)), int(where.group(2))
    if not 1 <= universe <= 32767 or not 1 <= address <= 512:
        raise Problem("The universe must be 1 to 32767 and the address 1 to 512.")
    size = re.fullmatch(r"(\d{1,7})[xX](\d{1,7})", words[1]) if len(words) > 1 else None
    if not size:
        raise Problem("Enter the pixel count as across x down, for example 40x10.")
    across, down = int(size.group(1)), int(size.group(2))
    if not 1 <= across <= 1024 or not 1 <= down <= 1024 or across * down > 262144:
        raise Problem("A screen has 1 to 1024 pixels each way and no more than 262,144 in all.")
    order, wiring = "RGB", "rows"
    for word in words[2:]:
        if word.lower() in PIXEL_WIRINGS:
            wiring = word.lower()
        elif len(word) in (3, 4) and sorted(word.upper()) in (sorted("RGB"), sorted("RGBW")):
            order = word.upper()
        else:
            raise Problem(f"\"{word}\" is neither an order of colours (RGB, GRB, RGBW and so on) nor a wiring ({', '.join(PIXEL_WIRINGS)}).")
    each = len(order)
    first = (512 - (address - 1)) // each     # how many pixels fit in the first universe: a pixel is never split over two
    universes = 1 if across * down <= first else 1 + -(-(across * down - first) // (512 // each))
    if universe + universes - 1 > 32767:
        raise Problem("The pixels run past the last universe.")
    return {"universe": universe, "address": address, "across": across, "down": down, "order": order, "wiring": wiring, "universes": universes,
            "text": f"{universe}.{address} {across}x{down} {order} {wiring}"}

PICTURE_ENDINGS = (".png", ".jpg", ".jpeg", ".bmp", ".tga")
FILM_ENDINGS = (".mp4", ".mov", ".m4v") + ((".avi", ".wmv") if sys.platform == "win32" else ())
# A screen as it is added: 16 to 9, four metres wide, its foot a metre up, three metres upstage, facing the house.
SCREEN_DEFAULTS = {"name": "Screen", "width": 4000.0, "height": 2250.0, "x": 0.0, "y": 3000.0, "z": 1000.0, "turn": 0.0,
                   "source": "pattern", "value": "", "nits": 1000.0}

# Where lengths are in feet and inches, and where paper is Letter and Tabloid rather than A4 and A3.
INCH_REGIONS = {"US", "LR", "MM"}
LETTER_REGIONS = {"US", "CA", "MX", "PH", "CL", "CO", "VE", "GT", "SV", "HN", "NI", "CR", "PA", "DO", "PR"}


def region_of(locale_name):
    """The country in a locale's name: "US" from "en_US"; "" if it names none."""
    return oslayer.region_of(locale_name)


def rig_totals(fixtures):
    """What the fixtures that will be on stage weigh and draw together, and how far that can be trusted.

    A fixture type whose file states no weight counts as nothing ("no_weight" says how many fixtures
    that is). Where a file rates the fixture's power draw that is used; where it only gives what the
    light sources consume, that is ("sources_only"), and the real draw is higher.
    """
    on_stage = [row for row in fixtures if "notype" not in row["notes"] and "removed" not in row["notes"]]
    return {"fixtures": len(on_stage),
            "weight": round(sum(row["weight"] for row in on_stage), 3),
            "power": round(sum(row["power"] for row in on_stage), 1),
            "no_weight": sum(1 for row in on_stage if not row["weight"]),
            "sources_only": sum(1 for row in on_stage if row["power"] and not row["power_rated"]),
            "no_power": sum(1 for row in on_stage if not row["power"])}


def check_patch(fixtures):
    """Marks what is wrong with a patch and sums up each universe.

    Each fixture gains "end" (its last slot) and "problems": "unpatched", "past_end" (it runs off
    the end of its universe, where the rest is not received), "overlap" (it shares slots with
    another fixture). Returns {"fixtures", "universes", "conflicts"}.
    """
    by_universe = {}
    for fixture in fixtures:
        fixture["end"] = fixture["address"] + max(fixture["slots"], 1) - 1
        if "notype" in fixture["notes"] or "removed" in fixture["notes"]:
            continue    # it will not be on stage, so it occupies nothing
        if fixture["universe"] <= 0:
            fixture["problems"].append("unpatched")
            continue
        if fixture["end"] > 512:
            fixture["problems"].append("past_end")
        by_universe.setdefault(fixture["universe"], []).append(fixture)

    conflicts = []
    universes = []
    for universe in sorted(by_universe):
        members = sorted(by_universe[universe], key=lambda fixture: (fixture["address"], fixture["end"]))
        used = set()
        clashing = False
        for index, fixture in enumerate(members):
            last = min(fixture["end"], 512)
            used.update(range(fixture["address"], last + 1))
            for other in members[index + 1:]:
                if other["address"] > last:
                    break    # sorted by address: nothing later can reach back
                clashing = True
                conflicts.append({"universe": universe, "a": fixture["name"], "b": other["name"],
                                  "from": other["address"], "to": min(last, other["end"], 512)})
                for both in (fixture, other):
                    if "overlap" not in both["problems"]:
                        both["problems"].append("overlap")
        universes.append({"universe": universe, "fixtures": len(members), "used": len(used), "conflict": clashing})
    return {"fixtures": fixtures, "universes": universes, "conflicts": conflicts}


def _same_file(a, b):
    if os.path.getsize(a) != os.path.getsize(b):
        return False
    with open(a, "rb") as first, open(b, "rb") as second:
        while True:
            one, two = first.read(1 << 20), second.read(1 << 20)
            if one != two:
                return False
            if not one:
                return True


# -------------------------------------------------------------------------------------------- HTTP

STATIC = {"/": ("index.html", "text/html; charset=utf-8"), "/app.js": ("app.js", "text/javascript; charset=utf-8"),
          "/style.css": ("style.css", "text/css; charset=utf-8"), "/icon.png": ("icon.png", "image/png")}


# The page's requests that change a show, and what each is called when it is offered for undoing.
UNDOABLE = {
    "/api/edits/discard": "Discard Edits", "/api/show/add": "Add Fixtures", "/api/show/set": "Edit Fixtures",
    "/api/types/remove": "Remove Fixture Type", "/api/types/set": "Replace Fixture Type", "/api/types/clear": "Revert Fixture Type",
    "/api/types/moves": "Fixture Type Movement", "/api/types/curve": "Dimmer Curve", "/api/types/share": "Replace Fixture Type", "/api/types/all": "Auto-Match Fixture Types",
    "/api/builder/build": "Build Fixture Type",
    "/api/screen/stream": "Rename NDI Stream", "/api/group/add": "Add Group", "/api/group/set": "Change Group", "/api/group/remove": "Remove Group",
    "/api/piece/add": "Add Set Piece", "/api/piece/set": "Edit Set Piece", "/api/piece/remove": "Remove Set Piece",
    "/api/screen/set": "Edit Screen", "/api/screen/add": "Add Screen", "/api/screen/remove": "Remove Screen",
    "/api/screen/split": "Split Screen", "/api/screen/section": "Edit Screen Section", "/api/screen/wall": "Convert LED Wall to Screen",
    "/api/show/hidden": "Layer Visibility", "/api/paper/set": "Paperwork Settings",
}


def undo_label(path, body):
    """What a request is called in the show's history, or None if it changes no show."""
    if path == "/api/show/remove":
        return "Restore Fixtures" if body.get("restore") else "Remove Fixtures"
    if path == "/api/types/remove":
        return "Restore Fixture Type" if body.get("restore") else "Remove Fixture Type"
    if path == "/api/piece/remove":
        return "Duplicate Set Piece" if body.get("copy") else "Remove Set Piece"
    return UNDOABLE.get(path)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "OpenvizLauncher"

    def log_message(self, *args):
        if self.server.verbose:
            super().log_message(*args)

    def _send(self, status, body, content_type="application/json"):
        if not isinstance(body, bytes):
            body = json.dumps(body).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        # A preview of a PDF is shown in a frame from a blob of the page's own making.
        self.send_header("Content-Security-Policy", "default-src 'self'; img-src 'self' blob:; frame-src 'self' blob:; frame-ancestors 'none'")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _allowed(self):
        """Only this computer's own launcher page may talk to the launcher."""
        port = self.server.server_address[1]
        if self.headers.get("Host", "") not in (f"127.0.0.1:{port}", f"localhost:{port}"):
            return False    # a page elsewhere that resolves its own name to 127.0.0.1
        origin = self.headers.get("Origin")
        if origin and origin not in (f"http://127.0.0.1:{port}", f"http://localhost:{port}"):
            return False
        return True

    def _token_ok(self):
        return hmac.compare_digest(self.headers.get("X-Launcher-Token", ""), self.server.token)

    def _json_body(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length > 1 << 20:
            raise Problem("That request is too large.")
        try:
            body = json.loads(self.rfile.read(length) or b"{}")
        except ValueError:
            raise Problem("That request is not valid JSON.") from None
        if not isinstance(body, dict):
            raise Problem("That request is not a set of names and values.")
        return body

    def _upload(self, launcher, name):
        length = int(self.headers.get("Content-Length") or 0)
        if length <= 0:
            raise Problem("The file is empty.")
        if length > MAX_UPLOAD:
            raise Problem(f"The file is larger than {MAX_UPLOAD >> 30} GB")
        handle, path = tempfile.mkstemp(prefix="drop-", dir=launcher.library)
        try:
            with os.fdopen(handle, "wb") as out:
                left = length
                while left:
                    chunk = self.rfile.read(min(left, 1 << 20))
                    if not chunk:
                        raise Problem("The upload was interrupted.")
                    out.write(chunk)
                    left -= len(chunk)
            return launcher.add_file(name, path)
        finally:
            if os.path.exists(path):
                os.remove(path)

    def _route(self, method, path, query):
        launcher = self.server.launcher
        if method == "GET":
            if path == "/api/state":
                return launcher.state()
            if path == "/api/here":    # the page saying it is still open (see _exit_with_window)
                return {"here": True}
            if path == "/api/shows":
                return {"shows": launcher.shows(), "found": launcher.found_exports()}
            if path == "/api/fixtures":
                return {"fixtures": launcher.fixtures()}
            if path == "/api/run":
                return launcher.run_state()
            if path == "/api/dmx/monitor":
                watched = query.get("universe", [""])[0]
                return launcher.dmx_monitor(int(watched) if watched.isdigit() else None)
            if path == "/api/recordings":
                return {"recordings": launcher.recordings(), "state": launcher.record_state(), "videos": launcher.videos(), "shows": launcher.show_names()}
            if path == "/api/show/screens":
                return launcher.screens(query.get("name", [""])[0])
            if path == "/api/osc":
                return launcher.osc_state()
            if path == "/api/psn":
                return launcher.psn_state()
            if path == "/api/paper":
                return launcher.paper_settings(query.get("name", [""])[0])
            if path == "/api/export/preview":
                return launcher.preview_paper(query.get("name", [""])[0], query.get("what", [""])[0], query.get("paper", [""])[0], query.get("units", [""])[0])
            if path == "/api/show/groups":
                return launcher.groups(query.get("name", [""])[0])
            if path == "/api/show/pieces":
                return launcher.pieces(query.get("name", [""])[0])
            if path == "/api/show/types":
                return launcher.show_types(query.get("name", [""])[0])
            if path == "/api/fixtures/thumbnail":
                return launcher.fixture_thumbnail(query.get("file", [""])[0])
            if path == "/api/builder":
                return launcher.builder(query.get("name", [""])[0], query.get("type", [""])[0], query.get("file", [""])[0])
            if path == "/api/types/find":
                return launcher.share_for_type(query.get("name", [""])[0], query.get("type", [""])[0])
            if path == "/api/ndi":
                return launcher.ndi_sources()
            if path == "/api/xchange":
                return launcher.xchange_state()
            if path == "/api/xchange/groups":
                return launcher.xchange_groups()
            if path == "/api/show":
                return launcher.show_detail(query.get("name", [""])[0])
            if path == "/api/parity":
                return launcher.parity()
            if path == "/api/share/search":
                return launcher.share_search(query.get("q", [""])[0], query.get("limit", ["60"])[0])
        elif method == "POST":
            if path == "/api/files":
                return self._upload(launcher, query.get("name", [""])[0])
            body = self._json_body()
            what = undo_label(path, body)
            if what and isinstance(body.get("name"), str):
                # A change to a show made from this page: kept, so that it can be undone.
                with launcher.changing(body["name"], what):
                    return self._post(launcher, path, query, body)
            return self._post(launcher, path, query, body)
        raise Problem("Not found.", status=404)

    def _post(self, launcher, path, query, body):
        if path == "/api/settings":
            return {"settings": launcher.update_settings(body)}
        if path == "/api/share/login":
            return launcher.share_login(body.get("user"), body.get("password"), body.get("remember") is True)
        if path == "/api/share/logout":
            return launcher.share_logout()
        if path == "/api/share/refresh":
            return launcher.share_refresh()
        if path == "/api/share/download":
            if isinstance(body.get("rid"), bool) or not isinstance(body.get("rid"), int):
                raise Problem("No revision was specified.")
            return launcher.share_download(body["rid"])
        if path == "/api/launch":
            return launcher.launch(body.get("kind"), body.get("name"), str(body.get("mode") or ""), bool(body.get("editor")))
        if path == "/api/dmx/check":
            return launcher.dmx_check()
        if path == "/api/record/start":
            return launcher.record_start()
        if path == "/api/record/stop":
            return launcher.record_stop()
        if path == "/api/recording/trim":
            return launcher.trim_recording(body.get("name"), body.get("start"), body.get("end"))
        if path == "/api/play/start":
            return launcher.play_start(body.get("name"), bool(body.get("loop", True)))
        if path == "/api/play/stop":
            return launcher.play_stop()
        if path == "/api/edits/discard":
            return launcher.discard_edits(body.get("name"))
        if path == "/api/export":
            return launcher.export_show(body.get("name"))
        if path == "/api/package":
            return launcher.package_show(body.get("name"))
        if path == "/api/paper/set":
            return launcher.set_paper(body.get("name"), body.get("changes"))
        if path == "/api/report":
            return launcher.crash_report()
        if path == "/api/build":
            return launcher.build_visualizer(reason="asked for")
        if path == "/api/export/patch":
            return launcher.export_patch(body.get("name"))
        if path == "/api/export/paper":
            return launcher.export_paper(body.get("name"), body.get("what"), body.get("paper"), body.get("units"))
        if path == "/api/export/open":
            return launcher.open_export(body.get("file"))
        if path == "/api/show/add":
            return launcher.add_fixtures(body.get("name"), body.get("fixture"), body.get("mode") or "", body.get("count", 1),
                                         body.get("universe", 0), body.get("address", 1), body.get("first_id", 0))
        if path == "/api/show/remove":
            if "uuids" in body:
                return launcher.remove_fixtures(body.get("name"), body.get("uuids"), bool(body.get("restore")))
            return launcher.remove_fixture(body.get("name"), body.get("uuid"), bool(body.get("restore")))
        if path == "/api/types/remove":
            return launcher.remove_type(body.get("name"), body.get("type"), bool(body.get("restore")))
        if path == "/api/show/update":
            return launcher.update_show(body.get("name"), body.get("newer"))
        if path == "/api/fixtures/update":
            return launcher.update_fixture(body.get("file"))
        if path == "/api/builder/template":
            return launcher.builder_template(body.get("template"), body.get("spec"), body.get("keep_modes"))
        if path == "/api/builder/build":
            return launcher.build_fixture(body.get("spec"), body.get("name") or "", body.get("type") or "")
        if path == "/api/types/set":
            return launcher.set_stand_in(body.get("name"), body.get("type"), body.get("file"))
        if path == "/api/types/clear":
            return launcher.clear_stand_in(body.get("name"), body.get("type"))
        if path == "/api/types/curve":
            return launcher.set_type_curve(body.get("name"), body.get("type"), body.get("curve"))
        if path == "/api/types/moves":
            return launcher.set_type_moves(body.get("name"), body.get("type"), body.get("invert_pan", False), body.get("invert_tilt", False),
                                           body.get("pan_offset", 0.0), body.get("tilt_offset", 0.0))
        if path == "/api/osc":
            launcher.update_settings({key: body[key] for key in ("osc", "osc_port") if key in body})
            return launcher.osc_apply()
        if path == "/api/psn":
            launcher.update_settings({key: body[key] for key in ("psn", "psn_group", "psn_port") if key in body})
            return launcher.psn_state()
        if path == "/api/visualizer":
            return launcher.visualizer(body.get("command"), body.get("value", ""))
        if path == "/api/show/set":
            return launcher.set_fixtures(body.get("name"), body.get("uuids"), body.get("changes"))
        if path == "/api/screen/stream":
            return launcher.rename_stream(body.get("name"), body.get("old"), body.get("new"))
        if path == "/api/group/add":
            return launcher.add_group(body.get("name"), body.get("group"), body.get("uuids"))
        if path == "/api/group/set":
            return launcher.set_group(body.get("name"), body.get("id"), body.get("changes"))
        if path == "/api/group/remove":
            return launcher.remove_group(body.get("name"), body.get("id"))
        if path == "/api/piece/add":
            return launcher.add_piece(body.get("name"), body.get("kind"), body.get("changes"))
        if path == "/api/piece/set":
            return launcher.set_piece(body.get("name"), body.get("id"), body.get("changes"))
        if path == "/api/piece/remove":
            return launcher.remove_piece(body.get("name"), body.get("id"), bool(body.get("copy")))
        if path == "/api/types/share":
            return launcher.share_stand_in(body.get("name"), body.get("type"), body.get("rid"))
        if path == "/api/types/all":
            return launcher.stand_in_all(body.get("name"))
        if path == "/api/screen/split":
            return launcher.split_screen(body.get("name"), body.get("id"), body.get("columns"), body.get("rows"))
        if path == "/api/screen/section":
            return launcher.set_section(body.get("name"), body.get("id"), body.get("section"), body.get("changes"))
        if path == "/api/screen/wall":
            return launcher.add_wall_screen(body.get("name"), body.get("fixtures"))
        if path == "/api/xchange/set":
            return launcher.xchange_set(body.get("on"), body.get("group"), body.get("name"))
        if path == "/api/xchange/fetch":
            return launcher.xchange_fetch(body.get("uuid"))
        if path == "/api/xchange/send":
            return launcher.xchange_send(body.get("name"), body.get("comment") or "")
        if path == "/api/screen/set":
            return launcher.set_screen(body.get("name"), body.get("id"), body.get("changes"))
        if path == "/api/screen/add":
            return launcher.add_screen(body.get("name"), body.get("screen"))
        if path == "/api/screen/remove":
            return launcher.remove_screen(body.get("name"), body.get("id"))
        if path == "/api/sound/choose":
            return launcher.choose_file("sound")
        if path == "/api/screen/choose":
            return launcher.choose_file(body.get("kind"))
        if path == "/api/model/choose":
            return launcher.import_model()
        if path == "/api/show/hidden":
            return launcher.set_hidden(body.get("name"), body.get("layers"), body.get("classes"))
        if path == "/api/import":
            return launcher.import_export(body.get("path"))
        if path == "/api/stop":
            return launcher.stop()
        if path == "/api/render":
            return launcher.render(body.get("name"), body.get("recording"), body.get("rate", 30), body.get("view", 0), body.get("path", ""),
                                   body.get("sound") or "", body.get("sound_start", 0.0))
        if path == "/api/reveal":
            return launcher.reveal(body.get("what"))
        if path == "/api/quit":
            threading.Thread(target=self.server.shutdown, daemon=True).start()
            return {"quitting": True}
        if path == "/api/undo":
            return launcher.undo(body.get("name"))
        if path == "/api/redo":
            return launcher.redo(body.get("name"))
        raise Problem("Not found.", status=404)

    def _handle(self, method):
        self.server.last_request = time.time()
        parts = urllib.parse.urlsplit(self.path)
        if not self._allowed():
            return self._send(403, {"error": "this launcher only answers its own page", "code": "forbidden"})
        if method == "GET" and parts.path in STATIC:
            name, content_type = STATIC[parts.path]
            with open(os.path.join(WEB, name), "rb") as handle:
                return self._send(200, handle.read(), content_type)
        if not parts.path.startswith("/api/"):
            return self._send(404, {"error": "there is no such page", "code": "not_found"})
        if not self._token_ok():
            return self._send(403, {"error": "open the launcher from its own window", "code": "forbidden"})
        try:
            result = self._route(method, parts.path, urllib.parse.parse_qs(parts.query))
            if isinstance(result, PdfBytes):
                self._send(200, bytes(result), "application/pdf")
            elif isinstance(result, PictureBytes):
                self._send(200, bytes(result), result.content_type)
            else:
                self._send(200, result)
        except Problem as problem:
            self._send(problem.status, {"error": str(problem), "code": problem.code})
        except Exception as error:    # a bug: say so on the page instead of dropping the connection
            self._send(500, {"error": f"the launcher hit a bug: {type(error).__name__}: {error}", "code": "bug"})

    def do_GET(self):
        self._handle("GET")

    def do_POST(self):
        self._handle("POST")


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, launcher, port=0, token=None, verbose=False):
        super().__init__(("127.0.0.1", port), Handler)
        self.launcher = launcher
        self.token = token or secrets.token_urlsafe(24)
        self.verbose = verbose
        self.last_request = time.time()    # when the page last asked anything: see _exit_with_window

    @property
    def url(self):
        return f"http://127.0.0.1:{self.server_address[1]}/#{self.token}"


def _exit_with_parent(parent, server):
    """The Mac app starts this as a child; if the app goes away, so does this."""
    while True:
        time.sleep(1.0)
        if not oslayer.process_alive(parent):
            server.shutdown()
            return


def _exit_with_window(window, server, started):
    """With --window: Openviz ends when its window is closed, as the Mac app does.

    The window is an Edge of its own (see oslayer.app_window_command), so its process ends when the
    window closes. An Edge that ends at once has handed the window to an Edge that was already
    running with that profile; then the page itself is the sign of life (it asks every half a
    minute, a hidden window at least every minute), and Openviz ends when it has not been heard
    from for two and a half minutes."""
    window.wait()
    if time.time() - started > 10:
        server.shutdown()
        return
    while time.time() - server.last_request < 150:
        time.sleep(5)
    server.shutdown()


def _running_file(library):
    return os.path.join(library, "logs", "launcher-running.json")


def _already_running(library):
    """The address of an Openviz that is already running on this library, or None. Two would both
    take the console's DMX and write the same files."""
    try:
        with open(_running_file(library), encoding="utf-8") as file:
            running = json.load(file)
        pid, url = int(running["pid"]), str(running["url"])
        port = urllib.parse.urlsplit(url).port
    except (OSError, ValueError, KeyError, TypeError):
        return None
    if pid == os.getpid() or not port or not oslayer.process_alive(pid):
        return None
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=2):
            return url
    except OSError:
        return None


def _open_window(url, library):
    """The page in a window of its own where the system has one (Edge on Windows), else in the
    browser. The window's process, or None."""
    command = oslayer.app_window_command(url, os.path.join(os.path.abspath(library), "logs", "window"))
    if command:
        try:
            return subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except OSError:
            pass
    webbrowser.open(url)
    return None


def _tell(message):
    """A message to someone who started Openviz without a console: a Windows message box."""
    print(message, file=sys.stderr)
    if oslayer.WINDOWS:
        try:
            import ctypes
            ctypes.windll.user32.MessageBoxW(None, message, "Openviz", 0x10)
        except (OSError, AttributeError):
            pass


def main(argv=None):
    if oslayer.WINDOWS and argv is None and not sys.flags.utf8_mode:
        # Windows reads and writes text in its own code page unless told otherwise, and Openviz's
        # files, the page's words and the core's output are UTF-8 everywhere. So Openviz runs again
        # in Python's UTF-8 mode, and the helpers it starts inherit it.
        os.environ["PYTHONUTF8"] = "1"
        return subprocess.call([sys.executable, "-X", "utf8", os.path.abspath(__file__)] + sys.argv[1:])
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--library", default=DEFAULT_LIBRARY, help="folder for shows, fixtures and settings (default library/)")
    p.add_argument("--port", type=int, default=0, help="port to listen on (default: any free one)")
    p.add_argument("--no-open", action="store_true", help="do not open the page in the browser")
    p.add_argument("--parent", type=int, help="quit when the process with this id is gone")
    p.add_argument("--token", help=argparse.SUPPRESS)        # a fixed token, for development and tests
    p.add_argument("--share-url", default=gdtf_share.DEFAULT_URL, help=argparse.SUPPRESS)    # for the tests
    p.add_argument("--verbose", action="store_true", help="log every request")
    p.add_argument("--no-relay", action="store_true", help="do not receive DMX here: Unreal listens itself (no live monitor)")
    p.add_argument("--window", action="store_true", help="show the page in a window of its own and quit when it is closed (Windows: an Edge app window)")
    args = p.parse_args(argv)
    if sys.stdout is None or sys.stderr is None:
        # Started without a console (pythonw, from the Windows shortcut): what would have been
        # printed goes into a file, as the Mac app's does, and the programs Openviz runs do not
        # each open a console window.
        os.makedirs(os.path.join(args.library, "logs"), exist_ok=True)
        log = open(os.path.join(args.library, "logs", "launcher.log"), "a", encoding="utf-8", buffering=1)    # noqa: SIM115
        sys.stdout, sys.stderr = sys.stdout or log, sys.stderr or log
        oslayer.hide_console_windows()
    if args.window:
        url = _already_running(args.library)
        if url:
            _open_window(url, args.library)
            return 0

    launcher = Launcher(args.library, share_url=args.share_url, relay=not args.no_relay, xchange=True, osc_allowed=True, build_when_stale=True)
    try:
        launcher.xchange_apply()    # if it was on when Openviz was last open
    except Problem as error:
        print(f"MVR-xchange: {error}", file=sys.stderr)
    launcher.osc_apply()    # likewise
    try:
        server = Server(launcher, args.port, args.token, args.verbose)
    except OSError as error:
        _tell(f"Openviz cannot listen on port {args.port}: {error}")
        return 1
    launcher.ensure_relay()    # from the start: what the console sends is seen before any show is opened
    print(f"URL {server.url}", flush=True)
    signal.signal(signal.SIGTERM, lambda *_: threading.Thread(target=server.shutdown, daemon=True).start())
    if args.parent:
        threading.Thread(target=_exit_with_parent, args=(args.parent, server), daemon=True).start()
    running = _running_file(args.library)
    if args.window:
        window = _open_window(server.url, args.library)
        if window:
            threading.Thread(target=_exit_with_window, args=(window, server, time.time()), daemon=True).start()
        try:
            with open(running, "w", encoding="utf-8") as file:
                json.dump({"pid": os.getpid(), "url": server.url}, file)
        except OSError:
            running = None
    elif not args.no_open:
        webbrowser.open(server.url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        launcher.close()
        if args.window and running:
            with contextlib.suppress(OSError):
                os.remove(running)
    return 0


if __name__ == "__main__":
    sys.exit(main())
