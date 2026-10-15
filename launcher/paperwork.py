"""A show on paper, as PDF: a light plot drawn to scale, and the lists a crew works from.

`plot_pdf` draws three sheets: the rig from above, from the house and from stage left. Every
fixture is drawn at its true size where it hangs, with its ID and its patch beside it; the
scenery is drawn as the boxes its pieces fill. `paperwork_pdf` writes a summary (what there is
of each type, what each layer weighs and draws, how full each universe is), the fixture schedule
with every fixture's place, and the patch in address order.

Both work from what the core says about a show: the patch rows of `vizcore_inspect --patch` as
the launcher reads them, and the output of `vizcore_inspect --plot` (`read_plot`). Lengths arrive
in millimetres in MVR's coordinates: X toward stage left (the audience's right), Y upstage, Z up.

Standard library only.
"""

import math
import re
import time

import pdf
from pdf import MM

# Sheets a plot can be drawn on: label, width and height in millimetres lying on its side, and
# the size its lists are printed on.
PAPERS = {
    "a4": ("A4", 297.0, 210.0, "a4"),
    "a3": ("A3", 420.0, 297.0, "a4"),
    "a2": ("A2", 594.0, 420.0, "a4"),
    "a1": ("A1", 841.0, 594.0, "a4"),
    "a0": ("A0", 1189.0, 841.0, "a4"),
    "letter": ("Letter", 279.4, 215.9, "letter"),
    "tabloid": ("Tabloid, 11 x 17 in", 431.8, 279.4, "letter"),
    "arch_c": ("Arch C, 18 x 24 in", 609.6, 457.2, "letter"),
    "arch_d": ("Arch D, 24 x 36 in", 914.4, 609.6, "letter"),
    "arch_e": ("Arch E, 36 x 48 in", 1219.2, 914.4, "letter"),
}
UNITS = ("metric", "imperial")

METRIC_SCALES = [10, 20, 25, 50, 75, 100, 125, 150, 200, 250, 300, 400, 500, 750, 1000, 1500, 2000, 5000]
# How many times smaller than life, and how a drawing in feet and inches says so.
IMPERIAL_SCALES = [(12, '1" = 1\'-0"'), (16, '3/4" = 1\'-0"'), (24, '1/2" = 1\'-0"'), (32, '3/8" = 1\'-0"'), (48, '1/4" = 1\'-0"'),
                   (64, '3/16" = 1\'-0"'), (96, '1/8" = 1\'-0"'), (128, '3/32" = 1\'-0"'), (192, '1/16" = 1\'-0"'),
                   (384, '1/32" = 1\'-0"'), (768, '1/64" = 1\'-0"'), (1536, '1/128" = 1\'-0"')]
FOOT = 304.8

# The sheets: which way is across the sheet, which way is up it, and which way points at
# whoever is looking; then what to write at the four edges of the drawing (left, right, bottom, top).
# The last is the plan again, with the light on the floor instead of the labels.
VIEWS = [
    ("plan", "Plan view", "from above", (1, 0, 0), (0, 1, 0), (0, 0, 1), ("STAGE RIGHT", "STAGE LEFT", "AUDIENCE", "UPSTAGE")),
    ("front", "Front view", "from the house", (1, 0, 0), (0, 0, 1), (0, -1, 0), ("STAGE RIGHT", "STAGE LEFT", "", "")),
    ("side", "Side view", "from stage left", (0, 1, 0), (0, 0, 1), (1, 0, 0), ("AUDIENCE", "UPSTAGE", "", "")),
    ("lux", "Light levels", "on the floor, everything at full", (1, 0, 0), (0, 1, 0), (0, 0, 1), ("STAGE RIGHT", "STAGE LEFT", "AUDIENCE", "UPSTAGE")),
]
LUX_CELL = 250.0    # millimetres of floor per reading

# What tells one fixture type from another inside its outline. After these eight, the same
# again on a grey outline.
MARKS = ["plain", "circle", "cross", "dot", "plus", "ring", "bar", "double"]

REMARKS = {"unpatched": "no address", "past_end": "runs past the end of its universe", "overlap": "shares addresses",
           "nomode": "mode not found: first mode used", "moved": "moved", "added": "added in Openviz",
           "repatched": "re-patched", "remoded": "mode changed"}


# ------------------------------------------------------------------------------------- reading

# What can be changed about a show's plot and paperwork, kept beside the show by the launcher
# ("<show>.paper.json"), and what each is when nothing was said. Text fields go into the title
# block and the lists' heads; empty ones are left out.
PAPER_TEXT = ("title", "event", "venue", "designer", "drawn_by", "company", "contact", "revision", "revision_note", "date", "notes")
PAPER_SHEETS = ("plan", "front", "side", "lux")
PAPER_LISTS = ("summary", "schedule", "patch")
PAPER_LABELS = ("id_patch", "id", "patch", "purpose", "none")    # what is written beside each fixture on the plan
FIXTURE_FIELDS = ("purpose", "colour", "note")


def paper_edits(edits=None):
    """`edits` as the plot and paperwork use them: every field there, as given or as it is by default."""
    edits = edits if isinstance(edits, dict) else {}
    out = {key: str(edits.get(key) or "") for key in PAPER_TEXT}
    sheets, lists = edits.get("sheets") or {}, edits.get("lists") or {}
    out["sheets"] = {key: sheets.get(key, True) is not False for key in PAPER_SHEETS}
    out["lists"] = {key: lists.get(key, True) is not False for key in PAPER_LISTS}
    out["labels"] = edits.get("labels") if edits.get("labels") in PAPER_LABELS else "id_patch"
    types = edits.get("types") if isinstance(edits.get("types"), dict) else {}
    out["types"] = {name: {"name": str((entry or {}).get("name") or ""), "hidden": bool((entry or {}).get("hidden"))}
                    for name, entry in types.items() if isinstance(entry, dict)}
    fixtures = edits.get("fixtures") if isinstance(edits.get("fixtures"), dict) else {}
    out["fixtures"] = {uuid: {key: str((entry or {}).get(key) or "") for key in FIXTURE_FIELDS} for uuid, entry in fixtures.items() if isinstance(entry, dict)}
    return out


def dress_rows(rows, edits):
    """The patch rows as the paper shows them: types left out gone, types renamed renamed."""
    out = []
    for row in rows:
        entry = edits["types"].get(type_name(row), {})
        if entry.get("hidden"):
            continue
        out.append(dict(row, type=entry["name"]) if entry.get("name") else row)
    return out


def title_of(show, edits):
    return edits["title"] or (show[:-4] if show.lower().endswith(".mvr") else show)


def read_plot(text):
    """What `vizcore_inspect --plot` printed, as
    {"types": {number: (low, high)}, "places": {uuid: (type number, placing)}, "objects": [...],
    "beams": {uuid: [{"origin", "direction", "lumens", "beam", "field", "intensity", "kind"}, ...]}}.
    A beam is as it stands at rest with the dimmer full: where the light leaves (millimetres)
    and which way (a unit vector), its flux and its angles.
    A placing is twelve numbers: the three axes and the origin of an MVR matrix. An object is
    {"kind", "uuid", "name", "layer", "class", "boxes": [(placing, low, high)]}. "unplaced" counts the
    fixtures the rig file gives no place, which have no place and no beams here. Lines that are not
    as expected are passed over."""
    types, places, objects, by_uuid, beams = {}, {}, [], {}, {}
    unplaced = 0
    for line in text.splitlines():
        parts = line.split("\t")
        try:
            if parts[0] == "unplaced" and len(parts) == 2:
                unplaced = max(0, int(parts[1]))
                continue
            if parts[0] == "beam" and len(parts) == 14:
                numbers = [float(part) for part in parts[3:13]]
                if all(math.isfinite(number) for number in numbers) and parts[13] != "None":    # a beam that only glows lights nothing
                    beams.setdefault(parts[1], []).append({"origin": tuple(numbers[0:3]), "direction": tuple(numbers[3:6]), "lumens": numbers[6],
                                                           "beam": numbers[7], "field": numbers[8], "intensity": numbers[9], "kind": parts[13]})
                continue
            if parts[0] == "object" and len(parts) == 6:
                made = {"kind": parts[1], "uuid": parts[2], "name": parts[3], "layer": parts[4], "class": parts[5], "boxes": []}
                objects.append(made)
                by_uuid[parts[2]] = made
                continue
            first = {"type": 2, "place": 3, "box": 2}.get(parts[0])
            if first is None or len(parts) != {"type": 8, "place": 15, "box": 20}[parts[0]]:
                continue
            numbers = tuple(float(part) for part in parts[first:])
            if not all(math.isfinite(number) for number in numbers):
                continue
            if parts[0] == "type":
                types[int(parts[1])] = (numbers[:3], numbers[3:])
            elif parts[0] == "place":
                places[parts[1]] = (int(parts[2]), numbers)
            elif parts[1] in by_uuid:
                by_uuid[parts[1]]["boxes"].append((numbers[:12], numbers[12:15], numbers[15:]))
        except ValueError:
            continue
    return {"types": types, "places": places, "objects": objects, "beams": beams, "unplaced": unplaced}


def standing_screen(name, width, height, x, y, z, turn):
    """A screen added in Openviz, as an object of a plot: upright, `width` by `height`, the middle
    of its lower edge at x, y, z, turned by `turn` degrees anticlockwise seen from above (at 0 it
    faces the house). Drawn 80 mm thick, behind its face."""
    c, s = math.cos(math.radians(turn)), math.sin(math.radians(turn))
    placing = (c, s, 0.0, -s, c, 0.0, 0.0, 0.0, 1.0, float(x), float(y), float(z))    # its own Y runs back from the face
    return {"kind": "VideoScreen", "uuid": "", "name": name, "layer": "", "class": "",
            "boxes": [(placing, (-width / 2.0, 0.0, 0.0), (width / 2.0, 80.0, float(height)))]}


def on_stage(row):
    return "notype" not in row["notes"] and "removed" not in row["notes"]


def natural(text):
    """Sorts "2" before "10" and "A2" before "A10"."""
    return [(0, int(part), "") if part.isdigit() else (1, 0, part.casefold()) for part in re.split(r"(\d+)", str(text)) if part]


def type_name(row):
    return row["type"].replace("@", " ").strip() or "(no type)"


def patch_of(row):
    return f"{row['universe']}.{row['address']:03d}" if row["universe"] > 0 else ""


# ------------------------------------------------------------------------------------ geometry

def place(placing, point):
    """Where a point of something stands on stage, given how that thing is placed."""
    ux, uy, uz, vx, vy, vz, wx, wy, wz, ox, oy, oz = placing
    x, y, z = point
    return (x * ux + y * vx + z * wx + ox, x * uy + y * vy + z * wy + oy, x * uz + y * vz + z * wz + oz)


def corners(placing, low, high):
    return [place(placing, (x, y, z)) for x in (low[0], high[0]) for y in (low[1], high[1]) for z in (low[2], high[2])]


def hull(points):
    """The outline that a rubber band round `points` would take, anticlockwise."""
    points = sorted(set((round(x, 4), round(y, 4)) for x, y in points))
    if len(points) < 3:
        return points

    def turn(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])

    lower, upper = [], []
    for point in points:
        while len(lower) >= 2 and turn(lower[-2], lower[-1], point) <= 0:
            lower.pop()
        lower.append(point)
    for point in reversed(points):
        while len(upper) >= 2 and turn(upper[-2], upper[-1], point) <= 0:
            upper.pop()
        upper.append(point)
    return lower[:-1] + upper[:-1]


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def bounds(points):
    xs = [x for x, _ in points]
    ys = [y for _, y in points]
    return min(xs), min(ys), max(xs), max(ys)


# ------------------------------------------------------------------------------------ measures

def length(mm, units):
    """A length as a drawing in those units writes it: 12.34 (metres), or 40'-6"."""
    if units == "imperial":
        inches = int(round(abs(mm) / 25.4))
        sign = "-" if mm < 0 and inches else ""
        return f"{sign}{inches // 12}'-{inches % 12}\""
    value = f"{mm / 1000.0:.2f}"
    return "0.00" if value == "-0.00" else value


def weight(kg, units):
    if not kg:
        return ""
    return f"{kg * 2.2046226:,.1f} lb" if units == "imperial" else f"{kg:,.1f} kg"


def power(watts):
    if not watts:
        return ""
    return f"{watts / 1000.0:,.2f} kW" if watts >= 10000 else f"{watts:,.0f} W"


def choose_scale(needed, units):
    """The standard scale at which something `needed` times smaller than life still fits: (how many times smaller, how it is written)."""
    if units == "imperial":
        return next((entry for entry in IMPERIAL_SCALES if entry[0] >= needed), IMPERIAL_SCALES[-1])
    scale = next((entry for entry in METRIC_SCALES if entry >= needed), METRIC_SCALES[-1])
    return scale, f"1:{scale}"


def grid_step(scale, units):
    """How far apart the grid lines are, in millimetres on stage, so that they are at least 7 mm apart on paper."""
    steps = [FOOT * feet for feet in (1, 2, 5, 10, 20, 50, 100, 200, 500)] if units == "imperial" else \
        [500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0, 50000.0, 100000.0]
    return next((step for step in steps if step / scale >= 7.0), steps[-1])


def grid_label(mm, units):
    if units == "imperial":
        return f"{int(round(mm / FOOT))}'"
    metres = mm / 1000.0
    return f"{metres:g}"


# ------------------------------------------------------------------------------- light levels

def candela(beam):
    """A beam's intensity in candela: its flux spread evenly over its field cone."""
    half = math.radians(min(max(beam["field"], 0.5), 179.0) / 2.0)
    return beam["lumens"] * beam["intensity"] / (2.0 * math.pi * (1.0 - math.cos(half)))


def lux_at(point, beams):
    """Illuminance at a point on the floor (millimetres), in lux, from these beams: full inside the
    beam angle, fading to nothing at the field angle, by the inverse square of the distance and the
    cosine of the angle the light meets the floor at. Everything at rest and at full."""
    total = 0.0
    for beam in beams:
        ox, oy, oz = beam["origin"]
        vx, vy, vz = point[0] - ox, point[1] - oy, point[2] - oz
        distance = math.sqrt(vx * vx + vy * vy + vz * vz)
        if distance < 100.0:
            continue
        dx, dy, dz = beam["direction"]
        along = (vx * dx + vy * dy + vz * dz) / distance    # cosine of the angle off the beam's axis
        if along <= 0.0:
            continue
        off = math.degrees(math.acos(min(along, 1.0)))
        half_field = max(beam["field"], 0.5) / 2.0
        if off >= half_field:
            continue
        half_beam = min(max(beam["beam"], 0.0), beam["field"]) / 2.0
        share = 1.0 if off <= half_beam else 1.0 - (off - half_beam) / max(half_field - half_beam, 1e-6)
        share = share * share * (3.0 - 2.0 * share)
        incidence = -vz / distance    # the floor faces up; light from above meets it squarely
        if incidence <= 0.0:
            continue
        metres = distance / 1000.0
        total += beam["candela"] * share * incidence / (metres * metres)
    return total


def lux_grid(beams, box, cell, floor=0.0):
    """Lux across the floor over `box` (left, bottom, right, top; millimetres), one value per `cell`
    square: {"columns", "rows", "cell", "left", "bottom", "values" (rows of columns), "peak"}."""
    left, bottom, right, top = box
    columns = max(int(math.ceil((right - left) / cell)), 1)
    rows = max(int(math.ceil((top - bottom) / cell)), 1)
    # Each beam reaches a known circle of floor: cells outside it are not asked.
    reaching = []
    for beam in beams:
        ox, oy, oz = beam["origin"]
        height = oz - floor
        if height <= 0.0 or beam["lumens"] <= 0.0 or beam["intensity"] <= 0.0:
            continue
        dx, dy, dz = beam["direction"]
        half = math.radians(min(max(beam["field"], 0.5), 170.0) / 2.0)
        tilt = math.acos(max(min(-dz, 1.0), -1.0))    # how far the beam leans from straight down
        if tilt + half >= math.radians(89.0):
            reach = height * 60.0    # nearly level: it goes a long way
        else:
            reach = height * math.tan(tilt + half) + cell
        centre = (ox + dx * height, oy + dy * height) if dz < 0.0 else (ox, oy)
        reaching.append((dict(beam, candela=candela(beam)), centre, reach + cell))
    values = []
    peak = 0.0
    for row in range(rows):
        y = bottom + (row + 0.5) * cell
        line = []
        for column in range(columns):
            x = left + (column + 0.5) * cell
            near = [beam for beam, centre, reach in reaching if abs(centre[0] - x) <= reach and abs(centre[1] - y) <= reach]
            value = lux_at((x, y, floor), near) if near else 0.0
            peak = max(peak, value)
            line.append(value)
        values.append(line)
    return {"columns": columns, "rows": rows, "cell": cell, "left": left, "bottom": bottom, "values": values, "peak": peak}


# How light levels are coloured on the sheet: nothing to dark red, through yellow, to white.
LUX_RAMP = [(0.0, (1.0, 1.0, 1.0)), (0.02, (0.35, 0.15, 0.45)), (0.15, (0.55, 0.1, 0.1)), (0.4, (0.95, 0.5, 0.05)), (0.75, (1.0, 0.92, 0.3)), (1.0, (1.0, 1.0, 0.85))]


def lux_colour(share):
    """The colour for a share (0 to 1) of the sheet's top value."""
    share = min(max(share, 0.0), 1.0)
    for (a, colour_a), (b, colour_b) in zip(LUX_RAMP, LUX_RAMP[1:]):
        if share <= b:
            f = (share - a) / (b - a) if b > a else 1.0
            return tuple(colour_a[k] + (colour_b[k] - colour_a[k]) * f for k in range(3))
    return LUX_RAMP[-1][1]


def round_lux(value):
    """A tidy top value for the scale: 1, 2, 5 times a power of ten, at or above `value`."""
    if value <= 0.0:
        return 100.0
    power = 10.0 ** math.floor(math.log10(value))
    for step in (1.0, 2.0, 5.0, 10.0):
        if step * power >= value * 0.999:
            return step * power
    return 10.0 * power


# ------------------------------------------------------------------------------------ the plot

class Taken:
    """The places on a sheet that already hold something, to keep labels off each other."""

    CELL = 24.0

    def __init__(self):
        self.cells = {}

    def _cells(self, box):
        x0, y0, x1, y1 = box
        for column in range(int(x0 // self.CELL), int(x1 // self.CELL) + 1):
            for row in range(int(y0 // self.CELL), int(y1 // self.CELL) + 1):
                yield column, row

    def add(self, box, owner=None):
        for cell in self._cells(box):
            self.cells.setdefault(cell, []).append((box, owner))

    def hits(self, box, owner=None):
        """How many things `box` would lie on, not counting `owner`'s own."""
        x0, y0, x1, y1 = box
        found = set()
        for cell in self._cells(box):
            for other, whose in self.cells.get(cell, ()):
                if (whose is None or whose != owner) and other[0] < x1 and other[2] > x0 and other[1] < y1 and other[3] > y0:
                    found.add((other, whose))
        return len(found)


def rig_for_plot(fixtures, plot):
    """The fixtures that will be on stage and have a place, each with the corners of the box it fills."""
    drawn = []
    for row in fixtures:
        placed = plot["places"].get(row["uuid"])
        if not on_stage(row) or not placed:
            continue
        low, high = plot["types"].get(placed[0], ((0.0, 0.0, 0.0), (0.0, 0.0, 0.0)))
        if any(high[axis] - low[axis] < 1.0 for axis in range(3)):
            low, high = (-150.0, -150.0, -300.0), (150.0, 150.0, 0.0)    # its file gives no sizes: a box as big as a small fixture
        drawn.append({"row": row, "placing": placed[1], "low": low, "high": high, "corners": corners(placed[1], low, high),
                      "origin": placed[1][9:12]})
    return drawn


def kind_of(item):
    """How a set piece is drawn: "truss", "screen" or "scenery". Consoles write trusses as plain
    scene objects, so the names are asked too."""
    words = " ".join((item["name"], item["class"], item["layer"])).casefold()
    if item["kind"] in ("VideoScreen", "Projector"):
        return "screen"
    return "truss" if item["kind"] in ("Truss", "Support") or "truss" in words or "traverse" in words else "scenery"


def scenery_for_plot(plot):
    """Every box of every set piece, as {"corners", "kind"}."""
    return [{"corners": corners(placing, low, high), "kind": kind_of(item)}
            for item in plot["objects"] for placing, low, high in item["boxes"]]


def extents(view, rig, pieces):
    """What the sheet has to show, in millimetres on stage: (left, bottom, right, top).

    Everything, unless the scenery reaches far beyond the rig (a whole venue round a small stage):
    then the rig with some room round it, and the scenery runs off the edge."""
    across, up = view[3], view[4]

    def reach(things):
        flat = [(dot(point, across), dot(point, up)) for thing in things for point in thing["corners"]]
        return bounds(flat) if flat else None

    of_rig, of_scenery = reach(rig), reach(pieces)
    if of_rig:
        span = max(of_rig[2] - of_rig[0], of_rig[3] - of_rig[1])
        room = max(5000.0, span * 0.5)
        box = [of_rig[0] - 1000.0, of_rig[1] - 1000.0, of_rig[2] + 1000.0, of_rig[3] + 1000.0]
        if of_scenery:
            box = [max(min(box[0], of_scenery[0] - 500.0), of_rig[0] - room), max(min(box[1], of_scenery[1] - 500.0), of_rig[1] - room),
                   min(max(box[2], of_scenery[2] + 500.0), of_rig[2] + room), min(max(box[3], of_scenery[3] + 500.0), of_rig[3] + room)]
    elif of_scenery:
        box = [of_scenery[0] - 500.0, of_scenery[1] - 500.0, of_scenery[2] + 500.0, of_scenery[3] + 500.0]
    else:
        box = [-5000.0, -4000.0, 5000.0, 4000.0]
    # The zero lines belong on the sheet where they are near: the centre line, and the floor of an elevation.
    if box[0] > 0.0 > box[0] - 3000.0 or box[2] < 0.0 < box[2] + 3000.0:
        box[0], box[2] = min(box[0], -500.0), max(box[2], 500.0)
    if view[0] != "plan":
        box[1] = min(box[1], -400.0) if box[1] < 6000.0 else box[1]
    return tuple(box)


def draw_mark(page, mark, x, y, radius, angle, line):
    """The sign inside a fixture's outline that says which type it is."""
    def spoke(turned, reach):
        c, s = math.cos(math.radians(angle + turned)) * reach, math.sin(math.radians(angle + turned)) * reach
        page.line(x - c, y - s, x + c, y + s, width=line)

    if mark == "circle":
        page.circle(x, y, radius * 0.72, width=line)
    elif mark == "dot":
        page.circle(x, y, radius * 0.42, stroke=None, fill=0.0)
    elif mark == "cross":
        spoke(45, radius * 0.95)
        spoke(135, radius * 0.95)
    elif mark == "plus":
        spoke(0, radius * 0.8)
        spoke(90, radius * 0.8)
    elif mark == "ring":
        page.circle(x, y, radius * 0.72, width=line)
        page.circle(x, y, radius * 0.24, stroke=None, fill=0.0)
    elif mark == "bar":
        spoke(90, radius * 0.9)
    elif mark == "double":
        page.circle(x, y, radius * 0.8, width=line)
        page.circle(x, y, radius * 0.4, width=line)


def style_of(index):
    """The mark and the shade of the outline for the nth fixture type."""
    return MARKS[index % len(MARKS)], (1.0 if (index // len(MARKS)) % 2 == 0 else 0.8)


def plot_pdf(show, fixtures, plot, paper="a3", units="metric", when=None, edits=None):
    """The light plot of a show: bytes of a PDF with a plan view, a front view and a side view,
    and the light on the floor. `fixtures` are the show's patch rows, `plot` what `read_plot` made
    of the core's answer, `edits` what was changed about the paper (paper_edits)."""
    label, sheet_width, sheet_height, _ = PAPERS[paper]
    when = time.localtime() if when is None else when
    edits = paper_edits(edits)
    views = [view for view in VIEWS if edits["sheets"][view[0]]]
    if not views:
        raise ValueError("no sheet is chosen")
    rig = rig_for_plot(dress_rows(fixtures, edits), plot)
    pieces = scenery_for_plot(plot)

    # One mark per fixture type, in the order of their names.
    names = sorted({type_name(item["row"]) for item in rig}, key=str.casefold)
    styles = {name: style_of(index) for index, name in enumerate(names)}

    width, height = sheet_width * MM, sheet_height * MM
    edge = 8.0 * MM
    column = min(max(sheet_width * 0.19, 56.0), 92.0) * MM          # the key and the title block, down the right side
    inset = 7.0 * MM                                                # room for the grid's numbers round the drawing
    area = (edge + inset, edge + inset, width - edge - column - inset, height - edge - inset)    # left, bottom, right, top
    area_width, area_height = (area[2] - area[0]) / MM, (area[3] - area[1]) / MM
    text_size = min(max(sheet_width / 420.0 * 5.6, 4.6), 9.0)

    reach = {view[0]: extents(view, rig, pieces) for view in VIEWS if view[0] != "lux"}
    reach["lux"] = reach["plan"]
    lit = [beam for item in rig for beam in plot.get("beams", {}).get(item["row"]["uuid"], [])]
    levels = lux_grid(lit, reach["plan"], LUX_CELL) if lit else None
    top_lux = round_lux(levels["peak"]) if levels else 0.0
    needed = max(max((box[2] - box[0]) / area_width, (box[3] - box[1]) / area_height) for box in reach.values())
    scale, scale_text = choose_scale(needed, units)
    step = grid_step(scale, units)

    document = pdf.Document(f"{title_of(show, edits)} - light plot", created=when)
    for sheet, view in enumerate(views, 1):
        key, title, seen, across, up, toward, edges = view
        page = document.page(width, height)
        box = reach[key]
        middle = ((box[0] + box[2]) / 2.0, (box[1] + box[3]) / 2.0)
        centre = ((area[0] + area[2]) / 2.0, (area[1] + area[3]) / 2.0)
        if key != "plan":
            # An elevation stands on the foot of the sheet, with a little ground under it, rather than floating in the middle.
            middle = (middle[0], box[1] - 600.0 + area_height * scale / 2.0)

        def paper_point(point, _across=across, _up=up, _middle=middle, _centre=centre):
            return (_centre[0] + (dot(point, _across) - _middle[0]) / scale * MM, _centre[1] + (dot(point, _up) - _middle[1]) / scale * MM)

        def paper_flat(a, b, _middle=middle, _centre=centre):
            return (_centre[0] + (a - _middle[0]) / scale * MM, _centre[1] + (b - _middle[1]) / scale * MM)

        # What the drawing area shows of the stage, edge to edge.
        shown = (middle[0] - (area[2] - area[0]) / MM * scale / 2.0, middle[1] - (area[3] - area[1]) / MM * scale / 2.0,
                 middle[0] + (area[2] - area[0]) / MM * scale / 2.0, middle[1] + (area[3] - area[1]) / MM * scale / 2.0)

        page.rect(edge, edge, width - 2 * edge, height - 2 * edge, line=0.9)
        page.line(width - edge - column, edge, width - edge - column, height - edge, width=0.9)

        # ---- the grid, with its numbers outside the drawing ----
        page.clip(area[0], area[1], area[2] - area[0], area[3] - area[1])
        first = math.ceil(shown[0] / step)
        for count in range(first, int(math.floor(shown[2] / step)) + 1):
            x = paper_flat(count * step, 0.0)[0]
            page.line(x, area[1], x, area[3], stroke=0.86, width=0.25)
        first = math.ceil(shown[1] / step)
        for count in range(first, int(math.floor(shown[3] / step)) + 1):
            y = paper_flat(0.0, count * step)[1]
            page.line(area[0], y, area[2], y, stroke=0.86, width=0.25)
        page.unclip()
        small = text_size * 0.82
        for count in range(math.ceil(shown[0] / step), int(math.floor(shown[2] / step)) + 1):
            x = paper_flat(count * step, 0.0)[0]
            page.text(x, area[1] - small - 1.5, grid_label(count * step, units), size=small, align="center", shade=0.4)
        for count in range(math.ceil(shown[1] / step), int(math.floor(shown[3] / step)) + 1):
            y = paper_flat(0.0, count * step)[1]
            page.text(area[0] - 2.0, y - small * 0.35, grid_label(count * step, units), size=small, align="right", shade=0.4)
        page.rect(area[0], area[1], area[2] - area[0], area[3] - area[1], stroke=0.55, line=0.35)

        page.clip(area[0], area[1], area[2] - area[0], area[3] - area[1])

        # ---- the light on the floor, under everything else ----
        if key == "lux" and levels and top_lux > 0.0:
            for ri in range(levels["rows"]):
                for ci in range(levels["columns"]):
                    value = levels["values"][ri][ci]
                    if value < top_lux * 0.004:
                        continue
                    x0, y0 = paper_flat(levels["left"] + ci * LUX_CELL, levels["bottom"] + ri * LUX_CELL)
                    x1, y1 = paper_flat(levels["left"] + (ci + 1) * LUX_CELL, levels["bottom"] + (ri + 1) * LUX_CELL)
                    page.rect(x0, y0, x1 - x0, y1 - y0, stroke=None, fill=lux_colour(value / top_lux))

        # ---- scenery: what is nearer to whoever looks is drawn later, and so on top ----
        whole = (shown[2] - shown[0]) * (shown[3] - shown[1])
        outlines = []
        for piece in pieces:
            flat = hull([paper_point(point) for point in piece["corners"]])
            if len(flat) < 3:
                continue
            x0, y0, x1, y1 = bounds(flat)
            if x1 < area[0] or x0 > area[2] or y1 < area[1] or y0 > area[3]:
                continue
            covers = (x1 - x0) * (y1 - y0) / (MM * MM) * scale * scale / whole
            outlines.append((covers > 0.5, max(dot(point, toward) for point in piece["corners"]), piece["kind"], flat))
        for too_big, _, kind, flat in sorted(outlines, key=lambda entry: (not entry[0], entry[1])):
            if too_big or key == "lux":    # something round the whole stage, or the light sheet: only edges, so that nothing is hidden
                page.polygon(flat, stroke=0.6, width=0.35, dash=(3, 2) if too_big else None)
            elif kind == "truss":
                page.polygon(flat, stroke=0.3, fill=0.88, width=0.5)
            elif kind == "screen":
                page.polygon(flat, stroke=0.15, fill=0.75, width=0.6)
            else:
                page.polygon(flat, stroke=0.55, fill=0.955, width=0.35)

        # ---- the zero lines ----
        zero = paper_flat(0.0, 0.0)
        if key in ("plan", "lux"):
            page.line(area[0], zero[1], area[2], zero[1], stroke=0.35, width=0.4, dash=(4, 2))
        else:
            page.line(area[0], zero[1], area[2], zero[1], stroke=0.0, width=0.9)    # the floor
        if key != "side":
            page.line(zero[0], area[1], zero[0], area[3], stroke=0.35, width=0.4, dash=(9, 2, 1.5, 2))
            page.text(zero[0] + 2.0, area[3] - text_size - 2.0, "CL", size=text_size, font="bold", shade=0.35)

        # ---- fixtures ----
        taken = Taken()
        symbols = []
        smallest = 2.7 * MM
        for item in sorted(rig, key=lambda entry: max(dot(point, toward) for point in entry["corners"])):
            flat = hull([paper_point(point) for point in item["corners"]])
            if len(flat) < 3:
                continue
            x0, y0, x1, y1 = bounds(flat)
            mid = ((x0 + x1) / 2.0, (y0 + y1) / 2.0)
            larger = max(x1 - x0, y1 - y0)
            if larger < smallest:    # too small to read at this scale: drawn bigger than life, about its middle
                grow = smallest / max(larger, 0.01)
                flat = [(mid[0] + (x - mid[0]) * grow, mid[1] + (y - mid[1]) * grow) for x, y in flat]
                x0, y0, x1, y1 = bounds(flat)
            mark, shade = styles[type_name(item["row"])]
            page.polygon(flat, stroke=0.0, fill=shade, width=0.6)
            # The mark turns with the fixture: along whichever of its own axes lies flattest on the sheet.
            placing = item["placing"]
            along = max((placing[0:3], placing[3:6]), key=lambda axis: math.hypot(dot(axis, across), dot(axis, up)))
            angle = math.degrees(math.atan2(dot(along, up), dot(along, across))) if key == "plan" else 0.0
            draw_mark(page, mark, (x0 + x1) / 2.0, (y0 + y1) / 2.0, min(x1 - x0, y1 - y0) * 0.4, angle, 0.45)
            symbols.append((item, (x0, y0, x1, y1)))
            taken.add((x0, y0, x1, y1), owner=item["row"]["uuid"])
        page.unclip()

        # ---- an elevation says how high things hang ----
        if key in ("front", "side"):
            heights = {}
            for item in rig:
                level = round(item["origin"][2] / 10.0) * 10.0
                heights[level] = heights.get(level, 0) + 1
            last = None
            for level in sorted(heights):
                y = paper_flat(0.0, level)[1]
                if y < area[1] or y > area[3] or (last is not None and y - last < text_size * 1.15):
                    continue
                last = y
                unit = "" if units == "imperial" else " m"
                words = f"{length(level, units)}{unit}  ({heights[level]})"
                page.line(area[2] - 5.0 * MM, y, area[2], y, stroke=0.0, width=0.5)
                page.text(area[2] - 5.6 * MM, y + 1.2, words, size=text_size, align="right")
                taken.add((area[2] - 6.0 * MM - pdf.text_width(words, text_size), y - 1.0, area[2], y + text_size + 1.5))

        if key != "lux" and edits["labels"] != "none":
            _labels(page, key, symbols, taken, area, text_size, edits)

        # ---- which way round the drawing is ----
        faint = text_size * 0.95
        page.text(area[0] + 3.0, area[1] + 3.0, edges[0], size=faint, shade=0.5)
        page.text(area[2] - 3.0, area[1] + 3.0, edges[1], size=faint, align="right", shade=0.5)
        # Beside the centre line where that runs through the middle of the sheet, not on it.
        aside = (area[0] + area[2]) / 2.0
        if key != "side" and abs(zero[0] - aside) < 40.0:
            aside = zero[0] - 45.0
        page.text(aside, area[1] + 3.0, edges[2], size=faint, align="center", shade=0.5)
        page.text(aside, area[3] - faint - 2.0, edges[3], size=faint, align="center", shade=0.5)

        _key_and_title(page, show, rig, names, styles, plot, (width - edge - column, edge, width - edge, height - edge),
                       f"{title}, {seen}", scale_text, label, step, units, when, sheet, len(views), smallest / MM * scale,
                       lux=(top_lux, levels["peak"] if levels else 0.0, len(lit)) if key == "lux" else None, edits=edits)
    return document.data()


def id_ranges(ids):
    """IDs written short: runs of three or more numbers in a row as "401-413", the rest one by one."""
    ids = sorted({str(value).strip() for value in ids if str(value).strip()}, key=natural)
    out, run = [], []

    def close():
        if len(run) >= 3:
            out.append(f"{run[0]}-{run[-1]}")
        else:
            out.extend(run)
        run.clear()

    for value in ids:
        if value.isdigit() and run and int(value) == int(run[-1]) + 1:
            run.append(value)
            continue
        close()
        if value.isdigit():
            run.append(value)
        else:
            out.append(value)
    close()
    return out


def _labels(page, key, symbols, taken, area, text_size, edits=None):
    """Writes beside each fixture what it is: its ID, and on the plan its patch under it (or what
    `edits["labels"]` asks for: the ID alone, the patch alone, or its purpose).

    Fixtures drawn on the same spot (a column of them seen from above, a row seen from its end)
    share one label. A label goes where nothing else is, as near as that can be had; one that had
    to go further off is tied to its fixture by a thin line."""
    patch_size = text_size * 0.8
    gap = 1.2
    edits = paper_edits(edits)
    mode = edits["labels"]

    def ident(row):
        """The bold part: the ID, or the purpose where that is asked for and given."""
        if mode == "patch":
            return ""
        if mode == "purpose":
            return edits["fixtures"].get(row["uuid"], {}).get("purpose", "") or str(row["id"]).strip()
        return str(row["id"]).strip()

    def patch_text(row):
        return patch_of(row) if mode in ("id_patch", "patch") else ""

    # Which symbols lie on each other: middles nearer than a millimetre, or a quarter of the symbol.
    groups, cells = [], {}
    for item, box in symbols:
        mid = ((box[0] + box[2]) / 2.0, (box[1] + box[3]) / 2.0)
        near = max(1.0 * MM, 0.25 * min(box[2] - box[0], box[3] - box[1]))
        home = None
        for column in (-1, 0, 1):
            for row in (-1, 0, 1):
                for group in cells.get((int(mid[0] // 12.0) + column, int(mid[1] // 12.0) + row), ()):
                    if math.hypot(group["mid"][0] - mid[0], group["mid"][1] - mid[1]) <= near:
                        home = group
        if home is None:
            home = {"mid": mid, "box": list(box), "members": []}
            groups.append(home)
            cells.setdefault((int(mid[0] // 12.0), int(mid[1] // 12.0)), []).append(home)
        home["members"].append(item)
        home["box"] = [min(home["box"][0], box[0]), min(home["box"][1], box[1]), max(home["box"][2], box[2]), max(home["box"][3], box[3])]

    # Each label as lines of (text, font, size) runs. Fixtures alone first: they have the room beside them.
    for group in sorted(groups, key=lambda entry: len(entry["members"])):
        members = sorted(group["members"], key=lambda item: (-item["origin"][2], natural(item["row"]["id"])))    # the highest first
        lines = []
        if len(members) == 1:
            row = members[0]["row"]
            if ident(row):
                lines.append([(ident(row), "bold", text_size)])
            if key == "plan" and patch_text(row):
                lines.append([(patch_text(row), "regular", patch_size)])
        elif key == "plan" and len(members) <= 8:
            for item in members:
                row = item["row"]
                runs = [(ident(row), "bold", text_size)] if ident(row) else []
                if patch_text(row):
                    runs.append((("  " if runs else "") + patch_text(row), "regular", patch_size))
                if runs:
                    lines.append(runs)
        elif mode in ("id_patch", "id"):
            line = ""
            for part in id_ranges(item["row"]["id"] for item in members):
                longer = f"{line}, {part}" if line else part
                if line and len(longer) > 16:
                    lines.append([(line + ",", "bold", text_size)])
                    line = part
                else:
                    line = longer
            if line:
                lines.append([(line, "bold", text_size)])
        if not lines:
            continue

        widths = [sum(pdf.text_width(text, size, font) for text, font, size in runs) for runs in lines]
        wide = max(widths) + 1.0
        tall = sum(max(size for _, _, size in runs) for runs in lines) * 1.04
        x0, y0, x1, y1 = group["box"]
        mid = ((x0 + x1) / 2.0, (y0 + y1) / 2.0)
        owner = members[0]["row"]["uuid"]    # a label never lies on its own fixtures: it is sought outside their box

        best, fewest, far = None, None, 0
        for ring in range(9 if key == "plan" else 4):
            dy = gap + ring * (tall * 0.9 + gap)
            dx = gap + ring * (wide * 0.6 + gap)
            above, below = (mid[0] - wide / 2.0, y1 + dy), (mid[0] - wide / 2.0, y0 - dy - tall)
            right, left = (x1 + dx, mid[1] - tall / 2.0), (x0 - dx - wide, mid[1] - tall / 2.0)
            spots = [above, below, right, left] if key == "plan" else [below, above, right, left]
            spots += [(x1 + dx, y1 + dy), (x0 - dx - wide, y1 + dy), (x1 + dx, y0 - dy - tall), (x0 - dx - wide, y0 - dy - tall)]
            if ring:    # and slid along the fixture's edges
                spots += [(mid[0] - wide / 2.0 + dx, y1 + gap), (mid[0] - wide / 2.0 - dx, y1 + gap),
                          (mid[0] - wide / 2.0 + dx, y0 - gap - tall), (mid[0] - wide / 2.0 - dx, y0 - gap - tall)]
            for x, y in spots:
                if x < area[0] + 1.0 or y < area[1] + 1.0 or x + wide > area[2] - 1.0 or y + tall > area[3] - 1.0:
                    continue
                hits = taken.hits((x, y, x + wide, y + tall), owner=owner)
                if fewest is None or hits < fewest:
                    best, fewest, far = (x, y), hits, ring
                if hits == 0:
                    break
            if fewest == 0:
                break
        if best is None or (fewest and key != "plan"):
            continue    # an elevation with fixtures behind each other: rather no label than one on top of another
        taken.add((best[0], best[1], best[0] + wide, best[1] + tall))
        if far:
            # From the label's nearest point to the fixture's nearest point.
            start = (min(max(mid[0], best[0]), best[0] + wide), min(max(mid[1], best[1]), best[1] + tall))
            end = (min(max(start[0], x0), x1), min(max(start[1], y0), y1))
            page.line(start[0], start[1], end[0], end[1], stroke=0.35, width=0.25)
        y = best[1] + tall
        for runs, width in zip(lines, widths):
            size = max(size for _, _, size in runs)
            y -= size * 1.04
            x = best[0] + (wide - width) / 2.0
            for text, font, run_size in runs:
                page.text(x, y + size * 0.2, text, size=run_size, font=font)
                x += pdf.text_width(text, run_size, font)


def title_facts(edits):
    """The title block's lines that were filled in, in the order a title block has them."""
    facts = [("Event", edits["event"]), ("Venue", edits["venue"]), ("Designer", edits["designer"]), ("Drawn by", edits["drawn_by"])]
    if edits["revision"] or edits["revision_note"]:
        facts.append(("Revision", " - ".join(part for part in (edits["revision"], edits["revision_note"]) if part)))
    return [(what, value) for what, value in facts if value]


def _key_and_title(page, show, rig, names, styles, plot, box, view, scale_text, paper_label, step, units, when, sheet, sheets, floor_mm, lux=None, edits=None):
    """The right-hand column of a plot sheet: what each mark stands for, and what the sheet is.
    On the light sheet, `lux` is (the top of the scale, the highest reading, the number of beams) and the key is the scale."""
    edits = paper_edits(edits)
    left, bottom, right, top = box
    pad = 3.0 * MM
    inner = right - left - 2 * pad
    if lux is not None:
        _lux_key(page, box, lux, units)

    # ---- title block, at the foot: as tall as what it holds ----
    facts = title_facts(edits) + [
        ("Scale", f"{scale_text} on {paper_label}"),
        ("Grid", f"{int(round(step / FOOT))} ft" if units == "imperial" else f"{step / 1000.0:g} m"),
        ("Fixtures", f"{len(rig)}, and {plot['unplaced']} without a place, not drawn" if plot.get("unplaced") else str(len(rig))),
        ("Date", edits["date"] or time.strftime("%Y-%m-%d %H:%M", when)),
        ("Sheet", f"{sheet} of {sheets}")]
    block = (20.0 + 4.1 * len(facts)) * MM
    page.line(left, bottom + block, right, bottom + block, width=0.9)
    y = bottom + block - 6.0 * MM
    page.text(left + pad, y, pdf.fit(title_of(show, edits), inner, 12, "bold"), size=12, font="bold")
    y -= 5.2 * MM
    page.text(left + pad, y, pdf.fit(f"Light plot: {view}", inner, 8, "bold"), size=8, font="bold")
    y -= 1.5 * MM
    for what, value in facts:
        y -= 4.1 * MM
        page.text(left + pad, y, what, size=6.5, shade=0.4)
        page.text(left + pad + 15.0 * MM, y, pdf.fit(value, inner - 15.0 * MM, 7.5), size=7.5)
    page.text(right - pad, bottom + 2.2 * MM, "Openviz", size=6, align="right", shade=0.45)
    foot = " - ".join(part for part in (edits["company"], edits["contact"]) if part) or show
    page.text(left + pad, bottom + 2.2 * MM, pdf.fit(foot, inner - 12.0 * MM, 6), size=6, shade=0.45)

    if lux is not None:
        return
    # ---- the key, from the top ----
    y = top - 6.0 * MM
    page.text(left + pad, y, "Key", size=9, font="bold")
    y -= 2.5 * MM
    counts = {}
    shape = {}
    for item in rig:
        name = type_name(item["row"])
        counts[name] = counts.get(name, 0) + 1
        shape.setdefault(name, (item["high"][0] - item["low"][0], item["high"][1] - item["low"][1]))
    row_height = 8.2 * MM
    sample = 6.4 * MM
    notes_room = 30.0 * MM
    for index, name in enumerate(names):
        if y - row_height < bottom + block + notes_room:
            page.text(left + pad, y - 4.0 * MM, f"and {len(names) - index} more types", size=7, font="italic", shade=0.3)
            y -= 6.0 * MM
            break
        y -= row_height
        mark, shade = styles[name]
        long, wide = shape[name]
        grow = sample / max(long, wide, 1.0)
        half = (max(long * grow, sample * 0.3) / 2.0, max(wide * grow, sample * 0.3) / 2.0)
        mid = (left + pad + sample / 2.0, y + sample / 2.0 - 0.6 * MM)
        page.rect(mid[0] - half[0], mid[1] - half[1], 2 * half[0], 2 * half[1], fill=shade, line=0.6)
        draw_mark(page, mark, mid[0], mid[1], min(half) * 0.8, 0.0, 0.45)
        words = left + pad + sample + 2.5 * MM
        page.text(words, y + 3.4 * MM, pdf.fit(name, right - pad - words, 7.2, "bold"), size=7.2, font="bold")
        page.text(words, y + 0.4 * MM, f"{counts[name]} on stage", size=6.4, shade=0.3)

    # ---- what the lines mean ----
    y -= 4.5 * MM
    kinds = {"truss": False, "screen": False, "scenery": False}
    for item in plot["objects"]:
        if item["boxes"]:
            kinds[kind_of(item)] = True
    swatches = [("Truss", 0.3, 0.88, kinds["truss"]), ("Screen", 0.15, 0.75, kinds["screen"]), ("Scenery (the box each piece fills)", 0.55, 0.955, kinds["scenery"])]
    for text, stroke, fill, there in swatches:
        if there and y - 4.6 * MM > bottom + block + 12.0 * MM:
            y -= 4.6 * MM
            page.rect(left + pad, y, sample, 2.6 * MM, stroke=stroke, fill=fill, line=0.4)
            page.text(left + pad + sample + 2.5 * MM, y + 0.5 * MM, pdf.fit(text, inner - sample - 2.5 * MM, 6.6), size=6.6)
    beside = {"id_patch": "Beside each fixture: its ID and, on the plan, universe.address.", "id": "Beside each fixture: its ID.",
              "patch": "Beside each fixture, on the plan: universe.address.", "purpose": "Beside each fixture: its purpose, or its ID where it has none.",
              "none": ""}[edits["labels"]]
    notes = [line for line in edits["notes"].splitlines() if line.strip()]
    notes += [f"{beside} Fixtures on one spot share a label, the highest first." if beside else "",
              f"Fixtures are drawn at their true size, or as {length(floor_mm, units)}{'' if units == 'imperial' else ' m'} across where smaller.",
              "Heights at the right of an elevation: where fixtures hang, and how many."]
    y -= 2.0 * MM
    for index, note in enumerate(note for note in notes if note):
        mine = index < len([line for line in edits["notes"].splitlines() if line.strip()])    # what was written for this show: darker
        for line in wrap(note, inner, 6.4 if mine else 6.0):
            if y - 2.8 * MM < bottom + block + 1.5 * MM:
                break
            y -= 3.0 * MM if mine else 2.8 * MM
            page.text(left + pad, y, line, size=6.4 if mine else 6.0, shade=0.0 if mine else 0.35)
        y -= 0.8 * MM


def _lux_key(page, box, lux, units):
    """The scale of the light sheet: a bar of the colours with lux values beside it, and what the readings assume."""
    left, bottom, right, top = box
    pad = 3.0 * MM
    inner = right - left - 2 * pad
    top_lux, peak, beams = lux
    y = top - 6.0 * MM
    page.text(left + pad, y, "Light on the floor", size=9, font="bold")
    y -= 5.0 * MM
    page.text(left + pad, y, "lux, everything at full", size=6.8, shade=0.35)
    bar_height = 60.0 * MM
    bar_width = 7.0 * MM
    y -= bar_height + 3.0 * MM
    steps = 40
    for index in range(steps):
        share = index / steps
        page.rect(left + pad, y + bar_height * share, bar_width, bar_height / steps + 0.3, stroke=None, fill=lux_colour(share + 0.5 / steps))
    page.rect(left + pad, y, bar_width, bar_height, stroke=0.3, line=0.4)
    for share in (0.0, 0.25, 0.5, 0.75, 1.0):
        value = top_lux * share
        page.line(left + pad + bar_width, y + bar_height * share, left + pad + bar_width + 1.5 * MM, y + bar_height * share, width=0.4)
        page.text(left + pad + bar_width + 2.2 * MM, y + bar_height * share - 2.2, f"{value:,.0f}" if value >= 10 or value == 0 else f"{value:.1f}", size=7)
    foot = "fc" if units == "imperial" else ""
    if units == "imperial":
        page.text(left + pad + bar_width + 2.2 * MM, y + bar_height + 4.0 * MM, f"({top_lux / 10.764:,.0f} {foot} at the top)", size=6.4, shade=0.35)
    y -= 6.0 * MM
    notes = [f"Highest reading {peak:,.0f} lux, from {beams} beam{'s' if beams != 1 else ''}.",
             "Every fixture at full, pan and tilt as they rest, zoom and colour at their defaults, open white, no gobos, nothing in the way.",
             "Each beam's flux is spread over its field angle, full to the beam angle and fading to nothing at the field angle.",
             f"Readings every {LUX_CELL / 1000.0:g} m on the floor at height 0; readings above a quarter of a percent of the top are drawn.",
             "A beam that only glows (an LED wall) is left out."]
    for note in notes:
        for line in wrap(note, inner, 6.0):
            y -= 2.8 * MM
            page.text(left + pad, y, line, size=6.0, shade=0.35)
        y -= 0.8 * MM


def wrap(text, width, size, font="regular"):
    """`text` broken into lines no wider than `width`."""
    lines, line = [], ""
    for word in str(text).split():
        longer = f"{line} {word}".strip()
        if line and pdf.text_width(longer, size, font) > width:
            lines.append(line)
            line = word
        else:
            line = longer
    return lines + [line] if line else lines


# ----------------------------------------------------------------------------------- the lists

class Lists:
    """Pages of lists, each with the show's name at its head and its number at its foot."""

    def __init__(self, document, paper, show, when, edits=None):
        _, long, short, _ = PAPERS[paper]
        self.edits = paper_edits(edits)
        self.document = document
        self.width, self.height = short * MM, long * MM    # standing upright
        self.left, self.right = 14.0 * MM, self.width - 14.0 * MM
        self.top, self.bottom = self.height - 13.0 * MM, 15.0 * MM
        self.show = title_of(show, self.edits)
        self.when = when
        self.pages = []
        self.page = None
        self.title = ""
        self.y = 0.0
        self.columns = None

    def start(self, title):
        """A new page under a new title."""
        self.title = title
        self.columns = None
        self._page()

    def _page(self):
        self.page = self.document.page(self.width, self.height)
        self.pages.append(self.page)
        span = self.right - self.left
        self.page.text(self.left, self.top - 10.0, pdf.fit(self.show, span * 0.6, 12, "bold"), size=12, font="bold")
        self.page.text(self.right, self.top - 10.0, pdf.fit(self.title, span * 0.38, 10), size=10, align="right")
        self.page.line(self.left, self.top - 15.0, self.right, self.top - 15.0, width=0.8)
        self.y = self.top - 24.0
        if self.columns:
            self._head()

    def room(self, needed):
        if self.y - needed < self.bottom:
            self._page()

    def heading(self, text):
        self.columns = None
        self.room(60.0)
        self.y -= 8.0
        self.page.text(self.left, self.y - 9.0, text, size=9.5, font="bold")
        self.y -= 14.0

    def lines(self, texts, size=8.0):
        self.columns = None
        for text in texts:
            for line in wrap(text, self.right - self.left, size):
                self.room(size * 1.4)
                self.page.text(self.left, self.y - size, line, size=size)
                self.y -= size * 1.4

    def _head(self):
        x = self.left
        for heading, width, align in self.columns:
            self._cell(x, width, heading, align, 7.0, "bold", 0.25)
            x += width
        self.y -= 12.0
        self.page.line(self.left, self.y + 1.0, self.right, self.y + 1.0, width=0.5)
        self.striped = False

    def _cell(self, x, width, text, align, size, font, shade=0.0):
        text = pdf.fit(text, width - 5.0, size, font)
        at = x + 2.5 if align == "left" else x + width - 2.5 if align == "right" else x + width / 2.0
        self.page.text(at, self.y - 8.0, text, size=size, font=font, align=align, shade=shade)

    def table(self, columns, rows):
        """A table. `columns` are (heading, share of the width, "left" | "right" | "center");
        `rows` are (kind, cells): "row", "total" (ruled off and bold), or "group" (a heading
        across the table, its one cell the text)."""
        span = self.right - self.left
        shares = sum(share for _, share, _ in columns)
        self.columns = [(heading, span * share / shares, align) for heading, share, align in columns]
        self.room(40.0)
        self._head()
        group = None

        def band(text):
            self.y -= 3.0
            self.page.rect(self.left, self.y - 11.5, span, 12.5, stroke=None, fill=0.87)
            self.page.text(self.left + 2.5, self.y - 8.2, pdf.fit(text, span - 5.0, 8.0, "bold"), size=8.0, font="bold")
            self.y -= 12.5
            self.striped = False

        for index, (kind, cells) in enumerate(rows):
            if kind == "group":
                following = sum(1 for later, _ in rows[index + 1:index + 3] if later != "group")
                self.room(15.5 + 11.5 * following)    # never a heading alone at the foot of a page
                group = str(cells[0])
                band(group)
                continue
            before = self.page
            self.room(11.5)
            if self.page is not before and group:    # the group runs on over the page: say which it is
                band(group.split("   -   ")[0] + "   -   continued")
            if kind == "total":
                self.page.line(self.left, self.y - 0.2, self.right, self.y - 0.2, width=0.5)
            elif self.striped:
                self.page.rect(self.left, self.y - 10.6, span, 11.5, stroke=None, fill=0.95)
            self.striped = not self.striped
            x = self.left
            for (_, width, align), cell in zip(self.columns, cells):
                self._cell(x, width, cell, align, 7.5, "bold" if kind == "total" else "regular")
                x += width
            self.y -= 11.5
        self.columns = None
        self.y -= 4.0

    def finish(self):
        stamp = self.edits["date"] or time.strftime("%Y-%m-%d %H:%M", self.when)
        if self.edits["revision"]:
            stamp = f"{stamp}   Revision {self.edits['revision']}"
        for number, page in enumerate(self.pages, 1):
            page.line(self.left, self.bottom - 6.0, self.right, self.bottom - 6.0, width=0.4, stroke=0.5)
            page.text(self.left, self.bottom - 15.0, stamp, size=7, shade=0.4)
            page.text(self.width / 2.0, self.bottom - 15.0, pdf.fit(" - ".join(part for part in (self.edits["company"], self.edits["contact"]) if part) or "Openviz",
                                                                    (self.right - self.left) * 0.4, 7), size=7, align="center", shade=0.4)
            page.text(self.right, self.bottom - 15.0, f"Page {number} of {len(self.pages)}", size=7, align="right", shade=0.4)


def paperwork_pdf(show, detail, plot, paper="a3", units="metric", when=None, edits=None):
    """The lists that go with a plot: bytes of a PDF. `detail` is the launcher's account of the
    show's patch ({"fixtures", "universes", "conflicts", "totals"}), `plot` what `read_plot` made
    of the core's answer (for where each fixture is), `edits` what was changed about the paper."""
    when = time.localtime() if when is None else when
    edits = paper_edits(edits)
    if not any(edits["lists"].values()):
        raise ValueError("no list is chosen")
    paper = PAPERS[paper][3]
    document = pdf.Document(f"{title_of(show, edits)} - paperwork", created=when)
    lists = Lists(document, paper, show, when, edits)
    rows = dress_rows([row for row in detail["fixtures"] if on_stage(row)], edits)
    left_out = [row for row in detail["fixtures"] if not on_stage(row)]
    # What is on paper, so that a type left out of it is left out of the totals too.
    totals = {"fixtures": len(rows), "weight": round(sum(row["weight"] for row in rows), 3), "power": round(sum(row["power"] for row in rows), 1),
              "no_weight": sum(1 for row in rows if not row["weight"]), "sources_only": sum(1 for row in rows if row["power"] and not row.get("power_rated", True))}

    def remarks(row):
        return "; ".join(REMARKS[word] for word in [*row["problems"], *row["notes"]] if word in REMARKS)

    def watts(row):
        return f"{row['power']:,.0f}" if row["power"] else ""

    def mass(value):
        return "" if not value else f"{value * 2.2046226:,.1f}" if units == "imperial" else f"{value:,.1f}"

    mass_unit = "lb" if units == "imperial" else "kg"

    def extra(row, key):
        return edits["fixtures"].get(row["uuid"], {}).get(key, "")

    # ---- summary ----
    if edits["lists"]["summary"]:
        summary(lists, show, rows, left_out, totals, detail, units, edits, mass, mass_unit)
    if edits["lists"]["schedule"]:
        schedule(lists, rows, plot, units, mass, mass_unit, watts, remarks, extra)
    if edits["lists"]["patch"]:
        patch_list(lists, rows, detail, remarks)
    lists.finish()
    return document.data()


def summary(lists, show, rows, left_out, totals, detail, units, edits, mass, mass_unit):
    """The first page of the lists: what the rig is, by type, by layer, by universe."""
    lists.start("Summary")
    facts = [f"{what}: {value}" for what, value in title_facts(edits)]
    facts += [f"Rig file: {show}", f"Fixtures on stage: {len(rows)}"]
    if totals["weight"]:
        facts.append(f"Weight of the fixtures: {weight(totals['weight'], units)}"
                     + (f" ({weight(totals['weight'], 'metric')})" if units == "imperial" else "")
                     + (f". {totals['no_weight']} of them state no weight and are not counted." if totals["no_weight"] else "."))
    if totals["power"]:
        facts.append(f"Power, everything at full: {power(totals['power'])}"
                     + (f". For {totals['sources_only']} fixtures this is what the light sources take; they draw more than that."
                        if totals["sources_only"] else "."))
    changed = {word: sum(1 for row in rows if word in row["notes"]) for word in ("moved", "repatched", "remoded", "added")}
    removed = sum(1 for row in left_out if "removed" in row["notes"])
    words = [f"{count} {text}" for count, text in ((changed["moved"], "moved"), (changed["repatched"], "re-patched"),
                                                  (changed["remoded"], "given another mode"), (changed["added"], "added"), (removed, "removed")) if count]
    if words:
        facts.append("Changed in Openviz since the rig file was made: " + ", ".join(words) + ".")
    missing = sum(1 for row in left_out if "notype" in row["notes"] and "removed" not in row["notes"])
    if missing:
        facts.append(f"{missing} fixtures are left out because the rig file does not carry their fixture type.")
    if detail["conflicts"]:
        facts.append(f"{len(detail['conflicts'])} pairs of fixtures share addresses: see the patch.")
    lists.lines(facts)

    lists.heading("Fixtures by type")
    kinds = {}
    for row in rows:
        entry = kinds.setdefault((type_name(row), row["mode"]), {"count": 0, "slots": row["slots"], "each": row["weight"], "weight": 0.0,
                                                               "draw": row["power"], "power": 0.0})
        entry["count"] += 1
        entry["weight"] += row["weight"]
        entry["power"] += row["power"]
    table = [("row", [name, mode, entry["count"], entry["slots"] or "", mass(entry["each"]), mass(entry["weight"]),
                      f"{entry['draw']:,.0f}" if entry["draw"] else "", f"{entry['power']:,.0f}" if entry["power"] else ""])
             for (name, mode), entry in sorted(kinds.items(), key=lambda item: (item[0][0].casefold(), natural(item[0][1])))]
    table.append(("total", ["Total", "", len(rows), "", "", mass(totals["weight"]), "", f"{totals['power']:,.0f}" if totals["power"] else ""]))
    lists.table([("Type", 30, "left"), ("Mode", 17, "left"), ("Count", 7, "right"), ("Slots", 7, "right"), (f"Each ({mass_unit})", 10, "right"),
                 (f"Weight ({mass_unit})", 11, "right"), ("Each (W)", 9, "right"), ("Power (W)", 11, "right")], table)

    lists.heading("By layer")
    layers = {}
    for row in rows:
        entry = layers.setdefault(row["layer"] or "(no layer)", [0, 0.0, 0.0])
        entry[0] += 1
        entry[1] += row["weight"]
        entry[2] += row["power"]
    table = [("row", [name, count, mass(heavy), f"{draw:,.0f}" if draw else ""]) for name, (count, heavy, draw) in sorted(layers.items(), key=lambda item: natural(item[0]))]
    table.append(("total", ["Total", len(rows), mass(totals["weight"]), f"{totals['power']:,.0f}" if totals["power"] else ""]))
    lists.table([("Layer", 46, "left"), ("Fixtures", 12, "right"), (f"Weight ({mass_unit})", 16, "right"), ("Power (W)", 16, "right")], table)

    lists.heading("Universes")
    table = [("row", [entry["universe"], entry["fixtures"], entry["used"], 512 - entry["used"], "fixtures share addresses" if entry["conflict"] else ""])
             for entry in detail["universes"]]
    unpatched = sum(1 for row in rows if row["universe"] <= 0)
    if table:
        table.append(("total", [f"{len(detail['universes'])} universes", sum(entry["fixtures"] for entry in detail["universes"]),
                                f"{sum(entry['used'] for entry in detail['universes']):,}", "", f"{unpatched} fixtures have no address" if unpatched else ""]))
        lists.table([("Universe", 12, "right"), ("Fixtures", 12, "right"), ("Slots used", 13, "right"), ("Slots free", 13, "right"), ("", 40, "left")], table)
    else:
        lists.lines(["No fixture has an address."])
    notes = [line for line in edits["notes"].splitlines() if line.strip()]
    if notes:
        lists.heading("Notes")
        lists.lines(notes)


def schedule(lists, rows, plot, units, mass, mass_unit, watts, remarks, extra):
    """Every fixture on stage, layer by layer: what it is, where, its patch, and what was written about it."""
    lists.start("Fixture schedule")
    unit = "" if units == "imperial" else " (m)"
    # Purpose, colour and note only where something was written in them for this show.
    written = [key for key in FIXTURE_FIELDS if any(extra(row, key) for row in rows)]
    table = []
    for layer in sorted({row["layer"] for row in rows}, key=natural):
        members = sorted((row for row in rows if row["layer"] == layer), key=lambda row: (natural(row["id"]), row["name"].casefold()))
        heavy, draw = sum(row["weight"] for row in members), sum(row["power"] for row in members)
        about = [f"{len(members)} fixture{'s' if len(members) != 1 else ''}"] + [text for text in (weight(heavy, units), power(draw)) if text]
        table.append(("group", [f"{layer or '(no layer)'}   -   " + ", ".join(about)]))
        for row in members:
            placed = plot["places"].get(row["uuid"])
            at = [length(value, units) for value in placed[1][9:12]] if placed else ["", "", ""]
            table.append(("row", [row["id"], row["name"], type_name(row), row["mode"], patch_of(row), *at, mass(row["weight"]), watts(row),
                                  *[extra(row, key) for key in written], remarks(row)]))
    shares = {"purpose": 14, "colour": 9, "note": 14}
    squeeze = sum(shares[key] for key in written) / 3.0    # what the written columns take, taken from name, type and mode
    columns = [("ID", 6, "left"), ("Name", 21 - squeeze, "left"), ("Type", 32 - squeeze, "left"), ("Mode", 13 - squeeze * 0.5, "left"), ("Patch", 8, "right"),
               (f"X{unit}", 8, "right"), (f"Y{unit}", 8, "right"), (f"Z{unit}", 8, "right"), (mass_unit, 7, "right"), ("W", 7, "right")]
    columns += [({"purpose": "Purpose", "colour": "Colour", "note": "Note"}[key], shares[key], "left") for key in written]
    columns.append(("Remarks", 13 - squeeze * 0.5, "left"))
    lists.table(columns, table)
    lists.lines(["X is toward stage left, Y upstage, Z up, from the rig file's own zero point to where each fixture hangs."], size=6.5)


def patch_list(lists, rows, detail, remarks):
    """The patch, universe by universe, and what has no address."""
    lists.start("Patch")
    table = []
    for entry in detail["universes"]:
        members = sorted((row for row in rows if row["universe"] == entry["universe"]), key=lambda row: (row["address"], natural(row["id"])))
        table.append(("group", [f"Universe {entry['universe']}   -   {entry['fixtures']} fixture{'s' if entry['fixtures'] != 1 else ''}, "
                                f"{entry['used']} of 512 slots" + (", fixtures share addresses" if entry["conflict"] else "")]))
        for row in members:
            table.append(("row", [row["address"], row["address"] + max(row["slots"], 1) - 1, row["slots"] or "", row["id"], row["name"], type_name(row),
                                  row["mode"], row["layer"], remarks(row)]))
    loose = sorted((row for row in rows if row["universe"] <= 0), key=lambda row: (natural(row["id"]), row["name"].casefold()))
    if loose:
        table.append(("group", [f"No address   -   {len(loose)} fixture{'s' if len(loose) != 1 else ''}"]))
        table += [("row", ["", "", row["slots"] or "", row["id"], row["name"], type_name(row), row["mode"], row["layer"], remarks(row)]) for row in loose]
    if table:
        lists.table([("From", 6, "right"), ("To", 6, "right"), ("Slots", 6, "right"), ("ID", 7, "left"), ("Name", 23, "left"), ("Type", 33, "left"),
                     ("Mode", 14, "left"), ("Layer", 14, "left"), ("Remarks", 16, "left")], table)
    else:
        lists.lines(["There are no fixtures on stage."])
