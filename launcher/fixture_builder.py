"""The Fixture Builder: a GDTF file made from a description, for a fixture nobody has published.

grandMA3 sends the fixtures of its own library as placeholders, and some fixtures have no GDTF file
anywhere (GDTF Share has none for the Martin RUSH MH 11 Beam, for one). Here a fixture is described
the way its manual does: what it is, its beam, its colour wheel, gobos and prism, and what each DMX
channel of each mode does. `build_gdtf` writes a GDTF 1.2 file of it, laid out as manufacturers'
files are (checked against Clay Paky's, Robe's and Ayrton's): a moving head as base, yoke (pan) and
head (tilt) with the beam at its front, or a static fixture as one body; 16-bit channels as one
channel of two slots; wheels chosen through channel sets; prisms with facets. It carries simple 3D
models of its parts (GLB), gobo pictures drawn here (PNG), and the description itself
(`openviz-builder.json`) so that it can be opened and changed again. Standard library only.
"""

from __future__ import annotations

import io
import json
import math
import re
import struct
import uuid
import xml.etree.ElementTree as ET
import zipfile
import zlib

DESCRIPTION_FILE = "openviz-builder.json"

# What a DMX channel can do: key -> (label, group, GDTF attribute, the key of the channel it is the fine half of).
FUNCTIONS = {
    "unused": ("Not Used", "General", "NoFeature", None),
    "control": ("Control / Reset", "General", "Control1", None),
    "dimmer": ("Dimmer", "Intensity", "Dimmer", None),
    "dimmer_fine": ("Dimmer Fine", "Intensity", "Dimmer", "dimmer"),
    "shutter": ("Shutter / Strobe", "Intensity", "Shutter1", None),
    "strobe": ("Strobe Rate", "Intensity", "Shutter1Strobe", None),
    "pan": ("Pan", "Position", "Pan", None),
    "pan_fine": ("Pan Fine", "Position", "Pan", "pan"),
    "tilt": ("Tilt", "Position", "Tilt", None),
    "tilt_fine": ("Tilt Fine", "Position", "Tilt", "tilt"),
    "speed": ("Pan/Tilt Speed", "Position", "PositionMSpeed", None),
    "color_wheel": ("Colour Wheel", "Colour", "Color1", None),
    "red": ("Red", "Colour", "ColorAdd_R", None),
    "green": ("Green", "Colour", "ColorAdd_G", None),
    "blue": ("Blue", "Colour", "ColorAdd_B", None),
    "white": ("White", "Colour", "ColorAdd_W", None),
    "amber": ("Amber", "Colour", "ColorAdd_A", None),
    "uv": ("UV", "Colour", "ColorAdd_UV", None),
    "lime": ("Lime", "Colour", "ColorAdd_GY", None),
    "cyan": ("Cyan", "Colour", "ColorSub_C", None),
    "magenta": ("Magenta", "Colour", "ColorSub_M", None),
    "yellow": ("Yellow", "Colour", "ColorSub_Y", None),
    "cto": ("CTO", "Colour", "CTO", None),
    "gobo": ("Gobo Wheel", "Gobo", "Gobo1", None),
    "gobo_rotation": ("Gobo Rotation", "Gobo", "Gobo1PosRotate", None),
    "prism": ("Prism", "Beam", "Prism1", None),
    "prism_rotation": ("Prism Rotation", "Beam", "Prism1PosRotate", None),
    "frost": ("Frost", "Beam", "Frost1", None),
    "iris": ("Iris", "Beam", "Iris", None),
    "zoom": ("Zoom", "Beam", "Zoom", None),
    "zoom_fine": ("Zoom Fine", "Beam", "Zoom", "zoom"),
    "focus": ("Focus", "Beam", "Focus1", None),
    "focus_fine": ("Focus Fine", "Beam", "Focus1", "focus"),
}
FINE = {key: entry[3] for key, entry in FUNCTIONS.items() if entry[3]}
SINGLE = [key for key in FUNCTIONS if key not in ("unused", "control")]    # at most once in a mode

# The usual layouts, for a start: the order is the one most fixtures of the kind follow, to be corrected from the manual.
LAYOUTS = {
    "beam": ["color_wheel", "shutter", "dimmer", "gobo", "prism", "prism_rotation", "frost", "focus", "pan", "pan_fine", "tilt", "tilt_fine",
             "speed", "control"],
    "spot": ["pan", "pan_fine", "tilt", "tilt_fine", "speed", "color_wheel", "cyan", "magenta", "yellow", "cto", "gobo", "gobo_rotation",
             "prism", "prism_rotation", "frost", "iris", "zoom", "focus", "shutter", "dimmer", "dimmer_fine", "control"],
    "wash": ["pan", "pan_fine", "tilt", "tilt_fine", "speed", "red", "green", "blue", "white", "cto", "zoom", "shutter", "dimmer", "dimmer_fine", "control"],
    "par": ["dimmer", "red", "green", "blue", "white", "shutter"],
    "dimmer": ["dimmer"],
}

# Fixtures to start from, with typical figures for their kind: to be corrected from the manual. The
# conventionals are what a console's generic dimmer channels usually are.
TEMPLATES = [
    ("beam", "Beam Moving Head", {"kind": "moving", "lamp": "Discharge", "beam_type": "Beam", "lumens": 6000, "beam_angle": 3.5, "field_angle": 4.5,
                                  "zoom_min": 0, "zoom_max": 0, "color_temperature": 7500, "width": 0.34, "depth": 0.26, "height": 0.52, "weight": 18, "power": 280,
                                  "colors": 8, "gobos": 8, "prism_facets": 8, "layout": "beam"}),
    ("spot", "Spot Moving Head", {"kind": "moving", "lamp": "LED", "beam_type": "Spot", "lumens": 20000, "beam_angle": 12, "field_angle": 18,
                                  "zoom_min": 6, "zoom_max": 45, "color_temperature": 7000, "width": 0.42, "depth": 0.3, "height": 0.65, "weight": 26, "power": 650,
                                  "colors": 7, "gobos": 7, "prism_facets": 4, "layout": "spot"}),
    ("wash", "Wash Moving Head", {"kind": "moving", "lamp": "LED", "beam_type": "Wash", "lumens": 12000, "beam_angle": 10, "field_angle": 20,
                                  "zoom_min": 6, "zoom_max": 55, "color_temperature": 6500, "width": 0.4, "depth": 0.3, "height": 0.5, "weight": 20, "power": 500,
                                  "colors": 0, "gobos": 0, "prism_facets": 0, "layout": "wash"}),
    ("par", "LED Par (RGBW)", {"kind": "static", "lamp": "LED", "beam_type": "Wash", "lumens": 3000, "beam_angle": 18, "field_angle": 25, "zoom_min": 0, "zoom_max": 0,
                               "color_temperature": 6500, "width": 0.25, "depth": 0.2, "height": 0.28, "weight": 4, "power": 150, "colors": 0, "gobos": 0, "prism_facets": 0,
                               "layout": "par"}),
    ("par64", "PAR 64, 1 kW", {"kind": "static", "lamp": "Tungsten", "beam_type": "Wash", "lumens": 12000, "beam_angle": 14, "field_angle": 28, "zoom_min": 0,
                               "zoom_max": 0, "color_temperature": 3200, "width": 0.25, "depth": 0.25, "height": 0.4, "weight": 3.5, "power": 1000, "colors": 0, "gobos": 0,
                               "prism_facets": 0, "layout": "dimmer"}),
    ("profile", "Profile Spot, 575 W", {"kind": "static", "lamp": "Tungsten", "beam_type": "Spot", "lumens": 8000, "beam_angle": 18, "field_angle": 26,
                                        "zoom_min": 0, "zoom_max": 0, "color_temperature": 3200, "width": 0.3, "depth": 0.3, "height": 0.65, "weight": 8, "power": 575,
                                        "colors": 0, "gobos": 0, "prism_facets": 0, "layout": "dimmer"}),
    ("fresnel", "Fresnel, 1 kW", {"kind": "static", "lamp": "Tungsten", "beam_type": "Wash", "lumens": 10000, "beam_angle": 30, "field_angle": 50,
                                  "zoom_min": 0, "zoom_max": 0, "color_temperature": 3200, "width": 0.3, "depth": 0.3, "height": 0.35, "weight": 6, "power": 1000,
                                  "colors": 0, "gobos": 0, "prism_facets": 0, "layout": "dimmer"}),
    ("blinder", "Blinder, 2-Lite", {"kind": "static", "lamp": "Tungsten", "beam_type": "Wash", "lumens": 30000, "beam_angle": 40, "field_angle": 70,
                                    "zoom_min": 0, "zoom_max": 0, "color_temperature": 3000, "width": 0.6, "depth": 0.2, "height": 0.3, "weight": 6, "power": 1300,
                                    "colors": 0, "gobos": 0, "prism_facets": 0, "layout": "dimmer"}),
]

DEFAULT_COLORS = [("Red", "#e8182b"), ("Orange", "#ff7a12"), ("Yellow", "#ffd60a"), ("Green", "#21c25e"), ("Cyan", "#16c7e8"),
                  ("Blue", "#1f3fff"), ("Magenta", "#e01ce0"), ("Pink", "#ff6fae"), ("Lavender", "#9b7bff"), ("CTO", "#ffb46b")]

GOBO_PATTERNS = ["Dot", "Ring", "Three Dots", "Bars", "Star", "Cross", "Breakup", "Triangle", "Rays", "Ring of Dots", "Half Moon", "Square",
                 "Tunnel", "Flower"]


class BuildError(ValueError):
    """What is wrong with a description, said so that the page can show it."""


# --------------------------------------------------------------------------------- description

def default_spec(kind="moving"):
    """A description to start from: a beam moving head, or an RGBW par."""
    moving = kind == "moving"
    return {
        "manufacturer": "", "name": "", "kind": "moving" if moving else "static",
        "lamp": "Discharge" if moving else "LED", "beam_type": "Beam" if moving else "Wash",
        "lumens": 6000 if moving else 3000, "beam_angle": 3.5 if moving else 18.0, "field_angle": 4.5 if moving else 25.0,
        "zoom_min": 0, "zoom_max": 0, "color_temperature": 7500 if moving else 6500,
        "pan_range": 540, "tilt_range": 270,
        "width": 0.34 if moving else 0.25, "depth": 0.26 if moving else 0.2, "height": 0.52 if moving else 0.28,
        "weight": 18.0 if moving else 4.0, "power": 280 if moving else 150,
        "colors": [{"name": name, "hex": hex_} for name, hex_ in DEFAULT_COLORS[:8]] if moving else [],
        "gobos": 8 if moving else 0, "prism_facets": 8 if moving else 0,
        "modes": [{"name": "Standard", "channels": list(LAYOUTS["beam" if moving else "par"])}],
    }


def _number(spec, key, low, high, what):
    try:
        value = float(spec.get(key, 0))
    except (TypeError, ValueError):
        raise BuildError(f"{what} must be a number.") from None
    if not math.isfinite(value) or value < low or value > high:
        raise BuildError(f"{what} must be between {low:g} and {high:g}.")
    return value


def check(spec):
    """The description as it will be built, with every number in range; raises BuildError."""
    if not isinstance(spec, dict):
        raise BuildError("The description must be a set of names and values.")
    out = {}
    for key, what in (("manufacturer", "Manufacturer"), ("name", "Name")):
        text = str(spec.get(key, "")).strip()
        if not text:
            raise BuildError(f"{what} is required.")
        if len(text) > 80 or any(ch in text for ch in "@/\\:*?\"<>|"):
            raise BuildError(f"{what} can be up to 80 characters, without @ / \\ : * ? \" < > |.")
        out[key] = text
    out["kind"] = "static" if spec.get("kind") == "static" else "moving"
    out["lamp"] = spec.get("lamp") if spec.get("lamp") in ("LED", "Discharge", "Tungsten") else "LED"
    out["beam_type"] = spec.get("beam_type") if spec.get("beam_type") in ("Spot", "Wash", "Beam") else "Spot"
    out["lumens"] = _number(spec, "lumens", 1, 500000, "Luminous flux")
    out["beam_angle"] = _number(spec, "beam_angle", 0.5, 120, "Beam angle")
    out["field_angle"] = _number(spec, "field_angle", 0.5, 160, "Field angle")
    if out["field_angle"] < out["beam_angle"]:
        raise BuildError("The field angle cannot be narrower than the beam angle.")
    out["zoom_min"] = _number(spec, "zoom_min", 0, 120, "Narrowest zoom")
    out["zoom_max"] = _number(spec, "zoom_max", 0, 160, "Widest zoom")
    out["color_temperature"] = _number(spec, "color_temperature", 1500, 20000, "Colour temperature")
    out["pan_range"] = _number(spec, "pan_range", 90, 720, "Pan range")
    out["tilt_range"] = _number(spec, "tilt_range", 90, 360, "Tilt range")
    for key, what in (("width", "Width"), ("depth", "Depth"), ("height", "Height")):
        out[key] = _number(spec, key, 0.03, 3, what)
    out["weight"] = _number(spec, "weight", 0, 500, "Weight")
    out["power"] = _number(spec, "power", 0, 10000, "Power")
    colors = spec.get("colors") or []
    if not isinstance(colors, list) or len(colors) > 30:
        raise BuildError("A colour wheel has up to 30 colours.")
    out["colors"] = []
    for entry in colors:
        name = str((entry or {}).get("name", "")).strip()[:40] or "Colour"
        hex_ = str((entry or {}).get("hex", "")).strip()
        if not re.fullmatch(r"#[0-9a-fA-F]{6}", hex_):
            raise BuildError(f"The colour of {name} must be written as #rrggbb.")
        out["colors"].append({"name": name, "hex": hex_.lower()})
    out["gobos"] = int(_number(spec, "gobos", 0, len(GOBO_PATTERNS), "Number of gobos"))
    facets = int(_number(spec, "prism_facets", 0, 16, "Prism facets"))
    if facets not in (0, 3, 4, 5, 6, 8):
        raise BuildError("A prism has 3, 4, 5, 6 or 8 facets, or none.")
    out["prism_facets"] = facets
    modes = spec.get("modes") or []
    if not isinstance(modes, list) or not modes or len(modes) > 12:
        raise BuildError("A fixture type has between one and twelve DMX modes.")
    out["modes"] = []
    names = set()
    for mode in modes:
        name = str((mode or {}).get("name", "")).strip()[:40]
        if not name:
            raise BuildError("Every mode needs a name.")
        if name.lower() in names:
            raise BuildError(f"Two modes are called {name}.")
        names.add(name.lower())
        channels = (mode or {}).get("channels") or []
        if not isinstance(channels, list) or not channels or len(channels) > 512:
            raise BuildError(f"The mode {name} needs between 1 and 512 channels.")
        for key in channels:
            if key not in FUNCTIONS:
                raise BuildError(f"The mode {name} has a channel of an unknown kind: {key}.")
        for key in SINGLE:
            if channels.count(key) > 1:
                raise BuildError(f"The mode {name} has {FUNCTIONS[key][0]} on more than one channel.")
        for fine, coarse in FINE.items():
            if fine in channels and coarse not in channels:
                raise BuildError(f"The mode {name} has {FUNCTIONS[fine][0]} without {FUNCTIONS[coarse][0]}.")
        if out["kind"] == "static" and any(key in channels for key in ("pan", "tilt", "speed")):
            raise BuildError(f"The mode {name} has pan or tilt, but the fixture is static. Make it a moving head.")
        if "color_wheel" in channels and not out["colors"]:
            raise BuildError(f"The mode {name} has a colour wheel channel: add the wheel's colours.")
        if ("gobo" in channels or "gobo_rotation" in channels) and not out["gobos"]:
            raise BuildError(f"The mode {name} has a gobo channel: set the number of gobos.")
        if ("prism" in channels or "prism_rotation" in channels) and not out["prism_facets"]:
            raise BuildError(f"The mode {name} has a prism channel: set the prism's facets.")
        if "zoom" in channels and not (out["zoom_max"] > out["zoom_min"] > 0):
            raise BuildError(f"The mode {name} has a zoom channel: set the narrowest and widest zoom.")
        out["modes"].append({"name": name, "channels": list(channels)})
    out["id"] = str(spec.get("id") or "")
    if not re.fullmatch(r"[0-9A-Fa-f-]{36}", out["id"]):
        out["id"] = str(uuid.uuid4()).upper()
    return out


def file_name(spec):
    """The library file name a built fixture gets: as GDTF Share names files, with Openviz as its revision."""
    return f"{spec['manufacturer']}@{spec['name']}@Openviz.gdtf"


# --------------------------------------------------------------------------------------- colour

def _srgb_to_xyY(hex_):
    """#rrggbb to CIE 1931 xyY, Y in percent: as GDTF writes a filter's colour."""
    channels = [int(hex_[i:i + 2], 16) / 255.0 for i in (1, 3, 5)]
    r, g, b = (c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4 for c in channels)
    x_ = 0.4124 * r + 0.3576 * g + 0.1805 * b
    y_ = 0.2126 * r + 0.7152 * g + 0.0722 * b
    z_ = 0.0193 * r + 0.1192 * g + 0.9505 * b
    total = x_ + y_ + z_
    if total <= 1e-9:
        return "0.312700,0.329000,0.000000"
    return f"{x_ / total:.6f},{y_ / total:.6f},{max(y_ * 100.0, 0.5):.6f}"


WHITE = "0.312700,0.329000,100.000000"

# --------------------------------------------------------------------------------- gobo pictures

def _png(width, height, rows):
    """An 8-bit greyscale PNG of `rows` (each a bytes of `width`)."""
    raw = b"".join(b"\x00" + row for row in rows)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def _inside(pattern, x, y):
    """Whether the point (x, y), each -1 to 1, lets light through the gobo `pattern`."""
    r = math.hypot(x, y)
    a = math.atan2(y, x)
    if r > 0.96:
        return False
    if pattern == "Dot":
        return r < 0.28
    if pattern == "Ring":
        return 0.55 < r < 0.78
    if pattern == "Three Dots":
        return any(math.hypot(x - 0.5 * math.cos(t), y - 0.5 * math.sin(t)) < 0.22 for t in (math.pi / 2, math.pi / 2 + 2.094, math.pi / 2 + 4.189))
    if pattern == "Bars":
        return any(abs(x - c) < 0.11 for c in (-0.5, 0.0, 0.5))
    if pattern == "Star":
        edge = 0.32 + 0.5 * (0.5 + 0.5 * math.cos(5 * (a - math.pi / 2))) ** 3
        return r < edge
    if pattern == "Cross":
        return abs(x) < 0.16 or abs(y) < 0.16
    if pattern == "Breakup":
        seeds = [(0.42, 0.18, 0.2), (-0.35, 0.4, 0.17), (-0.5, -0.25, 0.22), (0.1, -0.55, 0.16), (0.55, -0.35, 0.14), (-0.05, 0.05, 0.13),
                 (0.2, 0.65, 0.12), (-0.7, 0.05, 0.1), (0.72, 0.42, 0.09), (-0.25, -0.72, 0.1)]
        return any(math.hypot(x - sx, y - sy) < sr for sx, sy, sr in seeds)
    if pattern == "Triangle":
        return all(x * math.cos(t) + y * math.sin(t) < 0.32 for t in (math.pi / 2 + math.pi, math.pi / 2 + math.pi + 2.094, math.pi / 2 + math.pi + 4.189))
    if pattern == "Rays":
        return r > 0.2 and abs(((a / (2 * math.pi) * 8) % 1.0) - 0.5) < 0.16
    if pattern == "Ring of Dots":
        return any(math.hypot(x - 0.62 * math.cos(k * math.pi / 4), y - 0.62 * math.sin(k * math.pi / 4)) < 0.14 for k in range(8))
    if pattern == "Half Moon":
        return r < 0.7 and math.hypot(x - 0.3, y) > 0.55
    if pattern == "Square":
        return abs(x) < 0.45 and abs(y) < 0.45
    if pattern == "Tunnel":
        return any(lo < r < hi for lo, hi in ((0.12, 0.22), (0.38, 0.48), (0.64, 0.74)))
    if pattern == "Flower":
        return r < 0.25 + 0.45 * abs(math.cos(3 * a))
    return False


def gobo_png(pattern, size=128):
    """A gobo's picture: white lets light through, black holds it back, as manufacturers draw them."""
    rows = []
    for j in range(size):
        row = bytearray(size)
        for i in range(size):
            hits = 0
            for sj in (0.25, 0.75):    # four samples a pixel for soft edges
                for si in (0.25, 0.75):
                    x = (i + si) / size * 2 - 1
                    y = 1 - (j + sj) / size * 2
                    hits += _inside(pattern, x, y)
            row[i] = hits * 255 // 4
        rows.append(bytes(row))
    return _png(size, size, rows)


def thumbnail_png(spec, size=128):
    """A small picture of the fixture for lists: a moving head's base, yoke and head, or a static
    fixture's can, dark on white, its lens lit."""
    moving = spec["kind"] == "moving"

    def shade(x, y):    # x, y from -1 to 1, y up; 0 is black, 255 white
        if moving:
            lens = math.hypot(x, y + 0.1) < 0.2
            head = math.hypot(x, y + 0.1) < 0.42
            arms = (0.5 < abs(x) < 0.66) and -0.25 < y < 0.45
            bar = abs(x) < 0.66 and 0.33 < y < 0.45
            base = abs(x) < 0.55 and 0.45 < y < 0.72
        else:
            lens = abs(x) < 0.32 and -0.72 < y < -0.58
            head = abs(x) < 0.4 and -0.6 < y < 0.45
            arms = (0.4 < abs(x) < 0.5) and -0.1 < y < 0.35
            bar = abs(x) < 0.5 and 0.35 < y < 0.45
            base = abs(x) < 0.12 and 0.45 < y < 0.7
        if lens:
            return 215
        if head or arms or bar or base:
            return 45
        return 255

    rows = []
    for j in range(size):
        row = bytearray(size)
        for i in range(size):
            hits = 0
            for sj in (0.25, 0.75):
                for si in (0.25, 0.75):
                    hits += shade((i + si) / size * 2 - 1, 1 - (j + sj) / size * 2)
            row[i] = hits // 4
        rows.append(bytes(row))
    return _png(size, size, rows)


# ------------------------------------------------------------------------------------- 3D models

def _box(x0, x1, y0, y1, z0, z1):
    """Triangles (each three (x, y, z) corners, outward facing) of a box."""
    c = [(x, y, z) for z in (z0, z1) for y in (y0, y1) for x in (x0, x1)]
    quads = [(0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)]
    return [tri for a, b, cc, d in quads for tri in ((c[a], c[b], c[cc]), (c[a], c[cc], c[d]))]


def _cylinder(radius, z0, z1, sides=28):
    """Triangles of a closed cylinder along Z."""
    ring = [(radius * math.cos(2 * math.pi * k / sides), radius * math.sin(2 * math.pi * k / sides)) for k in range(sides)]
    tris = []
    for k in range(sides):
        (ax, ay), (bx, by) = ring[k], ring[(k + 1) % sides]
        tris += [((ax, ay, z0), (bx, by, z0), (bx, by, z1)), ((ax, ay, z0), (bx, by, z1), (ax, ay, z1))]
        tris += [((0, 0, z1), (ax, ay, z1), (bx, by, z1)), ((0, 0, z0), (bx, by, z0), (ax, ay, z0))]
    return tris


def glb(parts):
    """A binary glTF of `parts`, each (triangles in Z-up metres, (r, g, b)): one primitive a part, flat
    normals, Y up as glTF has it (the core turns it back to Z up)."""
    blob = bytearray()
    views, accessors, primitives, materials = [], [], [], []
    for triangles, colour in parts:
        positions, normals = [], []
        for a, b, c in triangles:
            u = [b[k] - a[k] for k in range(3)]
            v = [c[k] - a[k] for k in range(3)]
            n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
            length = math.sqrt(sum(k * k for k in n)) or 1.0
            n = [k / length for k in n]
            for p in (a, b, c):
                positions.append((p[0], p[2], -p[1]))    # Z up to glTF's Y up
                normals.append((n[0], n[2], -n[1]))
        for data, kind in ((positions, "pos"), (normals, "nor")):
            offset = len(blob)
            for vertex in data:
                blob += struct.pack("<3f", *vertex)
            views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(blob) - offset})
            accessor = {"bufferView": len(views) - 1, "componentType": 5126, "count": len(data), "type": "VEC3"}
            if kind == "pos":
                accessor["min"] = [min(p[k] for p in data) for k in range(3)]
                accessor["max"] = [max(p[k] for p in data) for k in range(3)]
            accessors.append(accessor)
        materials.append({"pbrMetallicRoughness": {"baseColorFactor": [colour[0], colour[1], colour[2], 1.0], "metallicFactor": 0.2, "roughnessFactor": 0.6}})
        primitives.append({"attributes": {"POSITION": len(accessors) - 2, "NORMAL": len(accessors) - 1}, "material": len(materials) - 1})
    document = {"asset": {"version": "2.0", "generator": "Openviz Fixture Builder"}, "scene": 0, "scenes": [{"nodes": [0]}],
                "nodes": [{"mesh": 0}], "meshes": [{"primitives": primitives}], "materials": materials,
                "accessors": accessors, "bufferViews": views, "buffers": [{"byteLength": len(blob)}]}
    text = json.dumps(document, separators=(",", ":")).encode()
    text += b" " * (-len(text) % 4)
    blob += b"\x00" * (-len(blob) % 4)
    total = 12 + 8 + len(text) + 8 + len(blob)
    return (struct.pack("<4sII", b"glTF", 2, total) + struct.pack("<I4s", len(text), b"JSON") + text
            + struct.pack("<I4s", len(blob), b"BIN\x00") + bytes(blob))


BODY = (0.035, 0.036, 0.04)      # housings are near black, as most are
LENS = (0.6, 0.62, 0.66)


def _layout(spec):
    """Where each part of the fixture goes, in metres: the sizes of its models and the places of its parts.
    A moving head hangs: base at the top, yoke below it, the head between the arms with room to turn."""
    w, d, h = spec["width"], spec["depth"], spec["height"]
    if spec["kind"] == "static":
        radius = min(w, d) / 2
        return {"body": (w, d, h), "radius": radius, "lens": radius * 0.72}
    arm = w * 0.12
    head_radius = min((w - 2 * arm) / 2 * 0.94, d / 2)
    base_height, head_length = h * 0.18, h * 0.42
    pivot = max(h - base_height - head_length / 2 - 0.006, h * 0.06 + math.hypot(head_radius, head_length / 2) + 0.01)
    return {"base": (w * 0.86, d, base_height), "yoke": (w, d * 0.42, pivot + head_radius * 0.6), "bar": h * 0.06, "arm": arm,
            "head_radius": head_radius, "head_length": head_length, "pivot": pivot, "lens": head_radius * 0.62}


def _models(spec):
    """The GLB files of the fixture's parts, by model name."""
    layout = _layout(spec)
    if spec["kind"] == "static":
        radius = layout["radius"]
        h = layout["body"][2]
        return {"Body": glb([(_cylinder(radius, -h, 0), BODY), (_cylinder(layout["lens"], -h - 0.004, -h + 0.001), LENS)])}
    bw, bd, bh = layout["base"]
    yw, yd, yh = layout["yoke"]
    arm = layout["arm"]
    r, hl = layout["head_radius"], layout["head_length"]
    base = _box(-bw / 2, bw / 2, -bd / 2, bd / 2, -bh, 0)
    yoke = (_box(-yw / 2, yw / 2, -yd / 2, yd / 2, -layout["bar"], 0) + _box(-yw / 2, -yw / 2 + arm, -yd / 2, yd / 2, -yh, 0)
            + _box(yw / 2 - arm, yw / 2, -yd / 2, yd / 2, -yh, 0))
    head = [(_cylinder(r, -hl / 2, hl / 2), BODY), (_cylinder(layout["lens"], -hl / 2 - 0.004, -hl / 2 + 0.001), LENS)]
    return {"Base": glb([(base, BODY)]), "Yoke": glb([(yoke, BODY)]), "Head": glb(head)}


# ---------------------------------------------------------------------------------- GDTF writing

def _matrix(z=0.0):
    return "{1.000000,0.000000,0.000000,0.000000}{0.000000,1.000000,0.000000,0.000000}{0.000000,0.000000,1.000000,%.6f}{0,0,0,1}" % z


def _dmx(value, size):
    return f"{value}/{size}"


def _sets(function, entries):
    for name, start, slot, low, high in entries:
        attributes = {"Name": name, "DMXFrom": start, "WheelSlotIndex": str(slot)}
        if low is not None:
            attributes.update(PhysicalFrom=f"{low:.6f}", PhysicalTo=f"{high:.6f}")
        ET.SubElement(function, "ChannelSet", attributes)


def _function(logical, name, attribute, start, default, low, high, wheel=""):
    attributes = {"Name": name, "Attribute": attribute, "OriginalAttribute": "", "DMXFrom": start, "Default": default,
                  "PhysicalFrom": f"{low:.6f}", "PhysicalTo": f"{high:.6f}", "RealFade": "0.000000", "RealAcceleration": "0.000000"}
    if wheel:
        attributes["Wheel"] = wheel
    return ET.SubElement(logical, "ChannelFunction", attributes)


def _even(count, size=256):
    """Where each of `count` equal ranges of a channel starts."""
    return [round(k * size / count) for k in range(count)]


def _channel(channels, spec, key, offsets, geometry, wheels, number=1):
    """One DMX channel of a mode: `offsets` its slots (two for 16 bits); `number` counts control channels."""
    attribute = FUNCTIONS[key][2] if key != "control" else f"Control{number}"
    bytes_ = len(offsets)
    full = 256 ** bytes_
    channel = ET.SubElement(channels, "DMXChannel", {"DMXBreak": "1", "Offset": ",".join(str(o) for o in offsets), "Highlight": "None",
                                                     "Geometry": geometry})
    logical = ET.SubElement(channel, "LogicalChannel", {"Attribute": attribute, "Snap": "No", "Master": "None", "MibFade": "0.000000",
                                                        "DMXChangeTimeLimit": "0.000000"})
    first = None
    if key == "dimmer":
        first = _function(logical, "Dimmer", "Dimmer", _dmx(0, bytes_), _dmx(0, bytes_), 0, 1)
    elif key == "shutter":
        # Closed, open, strobe slow to fast, open: the usual order. It starts open, so that a console
        # that never touches it still sees light, as with most fixtures' files.
        _function(logical, "Closed", "Shutter1", "0/1", "0/1", 0, 0)
        first = _function(logical, "Open", "Shutter1", "32/1", "32/1", 1, 1)
        _function(logical, "Strobe", "Shutter1Strobe", "64/1", "64/1", 1, 20)
        _function(logical, "Open 2", "Shutter1", "224/1", "224/1", 1, 1)
    elif key == "strobe":
        first = _function(logical, "Strobe Off", "Shutter1", "0/1", "0/1", 1, 1)
        _function(logical, "Strobe", "Shutter1Strobe", "10/1", "10/1", 1, 25)
    elif key in ("pan", "tilt"):
        span = spec["pan_range"] if key == "pan" else spec["tilt_range"]
        first = _function(logical, key.capitalize(), attribute, _dmx(0, bytes_), _dmx(full // 2, bytes_), -span / 2, span / 2)
    elif key == "speed":
        first = _function(logical, "Speed", attribute, "0/1", "0/1", 0, 1)
    elif key in ("red", "green", "blue", "white", "amber", "uv", "lime", "cyan", "magenta", "yellow", "frost"):
        first = _function(logical, FUNCTIONS[key][0], attribute, _dmx(0, bytes_), _dmx(0, bytes_), 0, 1)
    elif key == "cto":
        first = _function(logical, "CTO", "CTO", "0/1", "0/1", spec["color_temperature"], 2800)
    elif key == "iris":
        first = _function(logical, "Iris", "Iris", "0/1", "0/1", 1, 0.15)
    elif key == "zoom":
        first = _function(logical, "Zoom", "Zoom", _dmx(0, bytes_), _dmx(full // 2, bytes_), spec["zoom_min"], spec["zoom_max"])
    elif key == "focus":
        first = _function(logical, "Focus", "Focus1", _dmx(0, bytes_), _dmx(full // 2, bytes_), 0, 1)
    elif key == "color_wheel":
        slots = 1 + len(spec["colors"])
        first = _function(logical, "Colour", "Color1", "0/1", "0/1", 0, 1, wheels["color"])
        starts = _even(slots)
        _sets(first, [((["Open"] + [c["name"] for c in spec["colors"]])[k], f"{starts[k]}/1", k + 1, None, None) for k in range(slots)])
    elif key == "gobo":
        slots = 1 + spec["gobos"]
        first = _function(logical, "Gobo", "Gobo1", "0/1", "0/1", 0, 1, wheels["gobo"])
        starts = _even(slots)
        _sets(first, [((["Open"] + GOBO_PATTERNS[:spec["gobos"]])[k], f"{starts[k]}/1", k + 1, None, None) for k in range(slots)])
    elif key == "gobo_rotation":
        first = _function(logical, "Index", "Gobo1Pos", "0/1", "0/1", 0, 360, wheels["gobo"])
        _function(logical, "Rotation", "Gobo1PosRotate", "128/1", "192/1", -240, 240, wheels["gobo"])
    elif key == "prism":
        first = _function(logical, "Prism", "Prism1", "0/1", "0/1", 0, 1, wheels["prism"])
        _sets(first, [("Open", "0/1", 1, None, None), ("Prism", "128/1", 2, None, None)])
    elif key == "prism_rotation":
        first = _function(logical, "Index", "Prism1Pos", "0/1", "0/1", 0, 360, wheels["prism"])
        _function(logical, "Rotation", "Prism1PosRotate", "128/1", "192/1", -240, 240, wheels["prism"])
    else:    # unused, control: says nothing the visualizer acts on
        first = _function(logical, FUNCTIONS[key][0] if key == "unused" else f"Control {number}", attribute, "0/1", "0/1", 0, 1)
    attribute = first.get("Attribute")    # a logical channel is named after its first function, as manufacturers' files have it
    logical.set("Attribute", attribute)
    channel.set("InitialFunction", f"{geometry}_{attribute}.{attribute}.{first.get('Name')}")


def build_description(spec):
    """description.xml of the fixture type, as text."""
    moving = spec["kind"] == "moving"
    layout = _layout(spec)
    gdtf = ET.Element("GDTF", {"DataVersion": "1.2"})
    fixture = ET.SubElement(gdtf, "FixtureType", {
        "Name": spec["name"], "ShortName": spec["name"][:20], "LongName": spec["name"], "Manufacturer": spec["manufacturer"],
        "Description": "Made with the Openviz Fixture Builder.", "FixtureTypeID": spec["id"], "RefFT": "", "Thumbnail": "thumbnail",
        "CanHaveChildren": "Yes"})
    definitions = ET.SubElement(fixture, "AttributeDefinitions")
    groups = ET.SubElement(definitions, "ActivationGroups")
    for name in ("PanTilt", "Gobo1", "Prism"):
        ET.SubElement(groups, "ActivationGroup", {"Name": name})
    features = ET.SubElement(definitions, "FeatureGroups")
    for group, feature in (("Dimmer", "Dimmer"), ("Position", "PanTilt"), ("Gobo", "Gobo"), ("Color", "Color"), ("Color", "RGB"), ("Beam", "Beam"),
                           ("Focus", "Focus"), ("Control", "Control")):
        found = features.find(f"FeatureGroup[@Name='{group}']")
        if found is None:
            found = ET.SubElement(features, "FeatureGroup", {"Name": group, "Pretty": group})
        ET.SubElement(found, "Feature", {"Name": feature})
    attributes = ET.SubElement(definitions, "Attributes")
    defined = [("Dimmer", "Dimmer.Dimmer", "LuminousIntensity", "")]
    defined += [("Pan", "Position.PanTilt", "Angle", "PanTilt"), ("Tilt", "Position.PanTilt", "Angle", "PanTilt"),
                ("PositionMSpeed", "Control.Control", "None", ""), ("Shutter1", "Beam.Beam", "None", ""), ("Shutter1Strobe", "Beam.Beam", "Frequency", ""),
                ("Color1", "Color.Color", "None", ""), ("ColorAdd_R", "Color.RGB", "ColorComponent", ""), ("ColorAdd_G", "Color.RGB", "ColorComponent", ""),
                ("ColorAdd_B", "Color.RGB", "ColorComponent", ""), ("ColorAdd_W", "Color.RGB", "ColorComponent", ""), ("ColorAdd_A", "Color.RGB", "ColorComponent", ""),
                ("ColorAdd_UV", "Color.RGB", "ColorComponent", ""), ("ColorAdd_GY", "Color.RGB", "ColorComponent", ""),
                ("ColorSub_C", "Color.Color", "ColorComponent", ""), ("ColorSub_M", "Color.Color", "ColorComponent", ""), ("ColorSub_Y", "Color.Color", "ColorComponent", ""),
                ("CTO", "Color.Color", "Temperature", ""), ("Gobo1", "Gobo.Gobo", "None", "Gobo1"), ("Gobo1Pos", "Gobo.Gobo", "Angle", "Gobo1"),
                ("Gobo1PosRotate", "Gobo.Gobo", "AngularSpeed", "Gobo1"), ("Prism1", "Beam.Beam", "None", "Prism"), ("Prism1Pos", "Beam.Beam", "Angle", "Prism"),
                ("Prism1PosRotate", "Beam.Beam", "AngularSpeed", "Prism"), ("Frost1", "Beam.Beam", "None", ""), ("Iris", "Beam.Beam", "None", ""),
                ("Zoom", "Focus.Focus", "Angle", ""), ("Focus1", "Focus.Focus", "None", "")]
    defined += [(f"Control{n}", "Control.Control", "None", "") for n in range(1, 1 + max([1] + [mode["channels"].count("control") for mode in spec["modes"]]))]
    for name, feature, unit, group in defined:
        entry = {"Name": name, "Pretty": name, "Feature": feature, "PhysicalUnit": unit}
        if group:
            entry["ActivationGroup"] = group
        ET.SubElement(attributes, "Attribute", entry)
    wheels = ET.SubElement(fixture, "Wheels")
    names = {"color": "Colour Wheel", "gobo": "Gobo Wheel", "prism": "Prism Wheel"}
    if spec["colors"]:
        wheel = ET.SubElement(wheels, "Wheel", {"Name": names["color"]})
        ET.SubElement(wheel, "Slot", {"Name": "Open", "Color": WHITE})
        for colour in spec["colors"]:
            ET.SubElement(wheel, "Slot", {"Name": colour["name"], "Color": _srgb_to_xyY(colour["hex"])})
    if spec["gobos"]:
        wheel = ET.SubElement(wheels, "Wheel", {"Name": names["gobo"]})
        ET.SubElement(wheel, "Slot", {"Name": "Open", "Color": WHITE})
        for pattern in GOBO_PATTERNS[:spec["gobos"]]:
            ET.SubElement(wheel, "Slot", {"Name": pattern, "Color": WHITE, "MediaFileName": f"Openviz Gobo {pattern}"})
    if spec["prism_facets"]:
        wheel = ET.SubElement(wheels, "Wheel", {"Name": names["prism"]})
        ET.SubElement(wheel, "Slot", {"Name": "Open", "Color": WHITE})
        slot = ET.SubElement(wheel, "Slot", {"Name": f"{spec['prism_facets']}-Facet Prism", "Color": WHITE})
        # Each facet's matrix carries its image's offset in its third row, in beam radii, as manufacturers' files do.
        for k in range(spec["prism_facets"]):
            angle = math.pi / 2 + 2 * math.pi * k / spec["prism_facets"]
            reach = 1.25
            ET.SubElement(slot, "Facet", {"Color": WHITE, "Rotation": "{1.000000,0.000000,0.000000}{0.000000,1.000000,0.000000}{%.6f,%.6f,1.000000}"
                                          % (reach * math.cos(angle), reach * math.sin(angle))})
    physical = ET.SubElement(fixture, "PhysicalDescriptions")
    properties = ET.SubElement(physical, "Properties")
    ET.SubElement(properties, "Weight", {"Value": f"{spec['weight']:.6f}"})
    ET.SubElement(properties, "PowerConsumption", {"Value": f"{spec['power']:.6f}"})
    models = ET.SubElement(fixture, "Models")
    geometries = ET.SubElement(fixture, "Geometries")
    beam_attributes = {
        "Name": "Beam", "Model": "Lens", "LampType": spec["lamp"], "PowerConsumption": f"{spec['power']:.6f}", "LuminousFlux": f"{spec['lumens']:.6f}",
        "ColorTemperature": f"{spec['color_temperature']:.6f}", "BeamAngle": f"{spec['beam_angle']:.6f}", "FieldAngle": f"{spec['field_angle']:.6f}",
        "BeamRadius": f"{layout['lens']:.6f}", "BeamType": "Wash" if spec["beam_type"] == "Wash" else "Spot", "ThrowRatio": "1.000000",
        "RectangleRatio": "1.777700", "ColorRenderingIndex": "100"}
    ET.SubElement(models, "Model", {"Name": "Lens", "Length": f"{2 * layout['lens']:.6f}", "Width": f"{2 * layout['lens']:.6f}", "Height": "0.010000",
                                     "PrimitiveType": "Cylinder"})
    if moving:
        for name, size in (("Base", layout["base"]), ("Yoke", layout["yoke"])):
            ET.SubElement(models, "Model", {"Name": name, "File": name, "Length": f"{size[0]:.6f}", "Width": f"{size[1]:.6f}", "Height": f"{size[2]:.6f}",
                                             "PrimitiveType": "Undefined"})
        ET.SubElement(models, "Model", {"Name": "Head", "File": "Head", "Length": f"{2 * layout['head_radius']:.6f}", "Width": f"{2 * layout['head_radius']:.6f}",
                                         "Height": f"{layout['head_length'] + 0.004:.6f}", "PrimitiveType": "Undefined"})
        base = ET.SubElement(geometries, "Geometry", {"Name": "Base", "Model": "Base", "Position": _matrix()})
        yoke = ET.SubElement(base, "Axis", {"Name": "Yoke", "Model": "Yoke", "Position": _matrix(-layout["base"][2])})
        head = ET.SubElement(yoke, "Axis", {"Name": "Head", "Model": "Head", "Position": _matrix(-layout["pivot"])})
        ET.SubElement(head, "Beam", dict(beam_attributes, Position=_matrix(-layout["head_length"] / 2 - 0.006)))
        root, carriers = "Base", {"pan": "Yoke", "tilt": "Head"}
    else:
        size = layout["body"]
        ET.SubElement(models, "Model", {"Name": "Body", "File": "Body", "Length": f"{2 * layout['radius']:.6f}", "Width": f"{2 * layout['radius']:.6f}",
                                         "Height": f"{size[2] + 0.004:.6f}", "PrimitiveType": "Undefined"})
        body = ET.SubElement(geometries, "Geometry", {"Name": "Body", "Model": "Body", "Position": _matrix()})
        ET.SubElement(body, "Beam", dict(beam_attributes, Position=_matrix(-size[2] - 0.006)))
        root, carriers = "Body", {}
    modes = ET.SubElement(fixture, "DMXModes")
    for mode in spec["modes"]:
        element = ET.SubElement(modes, "DMXMode", {"Name": mode["name"], "Geometry": root, "Description": ""})
        channels = ET.SubElement(element, "DMXChannels")
        slots = {key: index + 1 for index, key in enumerate(mode["channels"]) if key not in ("unused", "control")}
        controls = 0
        last = len(mode["channels"]) - 1
        for index, key in enumerate(mode["channels"]):
            if key in FINE:
                continue    # written with its coarse half
            # A DMX channel is known by its part and attribute, so no two may share both. A slot that
            # does nothing is left out (the mode still reaches its last slot), and control channels
            # are counted: Control1, Control2 and so on, as GDTF numbers them.
            if key == "unused" and index != last:
                continue
            offsets = [index + 1]
            fine = next((f for f, c in FINE.items() if c == key), None)
            if fine and fine in slots:
                offsets.append(slots[fine])
            if key == "control":
                controls += 1
            _channel(channels, spec, key, offsets, carriers.get(key, "Beam"), names, controls)
        ET.SubElement(element, "Relations")
        ET.SubElement(element, "FTMacros")
    ET.SubElement(fixture, "Revisions").append(ET.Element("Revision", {"Text": "Openviz Fixture Builder", "UserID": "0"}))
    ET.indent(gdtf, space="  ")
    return '<?xml version="1.0" encoding="UTF-8" standalone="no" ?>\n' + ET.tostring(gdtf, encoding="unicode") + "\n"


def build_gdtf(spec):
    """The GDTF file (bytes) of a checked description."""
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("description.xml", build_description(spec))
        archive.writestr("thumbnail.png", thumbnail_png(spec))
        for name, data in _models(spec).items():
            archive.writestr(f"models/gltf/{name}.glb", data)
        for pattern in GOBO_PATTERNS[:spec["gobos"]]:
            archive.writestr(f"wheels/Openviz Gobo {pattern}.png", gobo_png(pattern))
        archive.writestr(DESCRIPTION_FILE, json.dumps(spec, indent=1))
    return out.getvalue()


def read_spec(data):
    """The description a built fixture carries, or None for a fixture made elsewhere."""
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            return json.loads(archive.read(DESCRIPTION_FILE))
    except (zipfile.BadZipFile, KeyError, ValueError):
        return None


def placeholder_spec(gdtf_bytes, type_name, in_use=None):
    """A description to start from, for a type a console sent as a placeholder: its modes with their
    channel counts, and the dimmer where the placeholder says it is. The rest is for the manual to say.
    Each mode also says which slots the placeholder joins as 16-bit pairs (`pairs`), as a hint: the
    description itself does not keep it.
    `in_use` (each {"name", "slots"}) narrows it to the modes the show uses, and stands in for the
    file's modes where it has none that can be read."""
    manufacturer, _, name = type_name.partition("@")
    name = name.split("@")[0] or manufacturer
    spec = default_spec("moving")
    spec.update(manufacturer=manufacturer.strip() or "Unknown", name=re.sub(r"^Dummy - ", "", name).strip() or "Fixture")
    modes = []
    try:
        with zipfile.ZipFile(io.BytesIO(gdtf_bytes)) as archive:
            root = ET.fromstring(archive.read("description.xml"))
    except (zipfile.BadZipFile, KeyError, ET.ParseError):
        root = None
    for mode in root.iter("DMXMode") if root is not None else []:
        slots, dimmer, pairs = {}, False, []
        for channel in mode.iter("DMXChannel"):
            offsets = [int(o) for o in (channel.get("Offset") or "").split(",") if o.strip().isdigit()]
            logical = channel.find("LogicalChannel")
            attribute = logical.get("Attribute", "") if logical is not None else ""
            first_dimmer = attribute == "Dimmer" and not dimmer and len(offsets) <= 2
            dimmer = dimmer or first_dimmer
            if len(offsets) == 2 and not first_dimmer:
                pairs.append(offsets)    # a channel of two slots: 16 bits, its fine half beside it
            for k, offset in enumerate(offsets):
                if first_dimmer:
                    slots[offset] = "dimmer" if k == 0 else "dimmer_fine"
                else:
                    slots.setdefault(offset, "unused")
        if slots and max(slots) <= 512:
            count = max(slots)
            modes.append({"name": (mode.get("Name") or "Mode")[:40], "channels": [slots.get(n, "unused") for n in range(1, count + 1)], "pairs": pairs})
    wanted = [entry for entry in in_use or [] if isinstance(entry.get("slots"), int) and 0 < entry["slots"] <= 512]
    if wanted:
        names = {entry["name"] for entry in wanted}
        kept = [mode for mode in modes if mode["name"] in names]
        modes = kept or [{"name": str(entry["name"])[:40] or "Mode", "channels": ["unused"] * entry["slots"]} for entry in wanted]
    if modes and max(len(mode["channels"]) for mode in modes) <= 4:
        # A handful of channels is a dimmer, a par or a strobe, not a moving head.
        static = default_spec("static")
        spec.update({key: static[key] for key in static if key not in ("manufacturer", "name", "modes")})
    if modes:
        seen = set()
        for mode in modes:    # mode names must differ
            base, n = mode["name"], 2
            while mode["name"].lower() in seen:
                mode["name"] = f"{base} {n}"
                n += 1
            seen.add(mode["name"].lower())
        spec["modes"] = modes
    return spec


def from_template(key, spec=None, keep_modes=False):
    """A description with a template's figures: over `spec` where given (its maker, name and id kept,
    and with `keep_modes` its modes too: those of a console's placeholder, which knows the channel
    count), else over the defaults."""
    found = next((figures for name, _label, figures in TEMPLATES if name == key), None)
    if found is None:
        raise BuildError("There is no such template.")
    out = dict(default_spec(found["kind"]) if spec is None else spec)
    out.update({key_: value for key_, value in found.items() if key_ not in ("colors", "layout")})
    out["colors"] = [{"name": name, "hex": hex_} for name, hex_ in DEFAULT_COLORS[:found["colors"]]]
    if not (keep_modes and spec and spec.get("modes")):
        out["modes"] = [{"name": "Standard" if found["layout"] != "dimmer" else "1 Channel", "channels": list(LAYOUTS[found["layout"]])}]
    return out


def catalogue():
    """What the page offers: every kind of channel by group, the usual layouts, the gobo patterns and a default description."""
    groups = {}
    for key, (label, group, _attribute, coarse) in FUNCTIONS.items():
        groups.setdefault(group, []).append({"key": key, "label": label, "fine_of": coarse})
    return {"groups": [{"name": name, "functions": entries} for name, entries in groups.items()], "layouts": LAYOUTS,
            "patterns": GOBO_PATTERNS, "moving": default_spec("moving"), "static": default_spec("static"),
            "templates": [{"key": key, "label": label} for key, label, _figures in TEMPLATES]}
