"""A small PDF writer for drawings and lists: lines, shapes and text. Standard library only.

Text is set in Helvetica, one of the fonts every PDF reader carries itself, so nothing is
embedded; the widths below are that font's own, which lets text be centred, set flush right and
cut to fit. Characters outside Windows-1252 (the encoding those fonts are used with) come out
as "?".

Lengths are in points (1/72 inch) from the page's bottom-left corner, as in PDF itself; `MM`
turns millimetres into points. Shades are 0 (black) to 1 (white), or a (red, green, blue) triple.

    document = Document("My plot")
    page = document.page(420 * MM, 297 * MM)
    page.line(10, 10, 200, 10, width=1.0)
    page.text(20, 20, "Hello", size=12, font="bold")
    document.save("plot.pdf")
"""

import math
import time
import zlib

MM = 72.0 / 25.4

# Advance widths in thousandths of the font size, for the characters 32 to 126 and 160 to 255.
_REGULAR = [278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556, 556, 556, 556, 556, 556,
            556, 278, 278, 584, 584, 584, 556, 1015, 667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778, 667, 778,
            722, 667, 611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469, 556, 333, 556, 556, 500, 556, 556, 278, 556, 556, 222, 222,
            500, 222, 833, 556, 556, 556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584]
_REGULAR_HIGH = [278, 333, 556, 556, 556, 556, 260, 556, 333, 737, 370, 556, 584, 333, 737, 333, 400, 549, 333, 333, 333, 576, 537, 278,
                 333, 333, 365, 556, 834, 834, 834, 611, 667, 667, 667, 667, 667, 667, 1000, 722, 667, 667, 667, 667, 278, 278, 278, 278,
                 722, 722, 778, 778, 778, 778, 778, 584, 778, 722, 722, 722, 722, 667, 667, 611, 556, 556, 556, 556, 556, 556, 889, 500,
                 556, 556, 556, 556, 278, 278, 278, 278, 556, 556, 556, 556, 556, 556, 556, 549, 611, 556, 556, 556, 556, 500, 556, 500]
_BOLD = [278, 333, 474, 556, 556, 889, 722, 238, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556, 556, 556, 556, 556, 556, 556,
         333, 333, 584, 584, 584, 611, 975, 722, 722, 722, 722, 667, 611, 778, 722, 278, 556, 722, 611, 833, 722, 778, 667, 778, 722, 667,
         611, 722, 667, 944, 667, 667, 611, 333, 278, 333, 584, 556, 333, 556, 611, 556, 611, 556, 333, 611, 611, 278, 278, 556, 278, 889,
         611, 611, 611, 611, 389, 556, 333, 611, 556, 778, 556, 556, 500, 389, 280, 389, 584]
_BOLD_HIGH = [278, 333, 556, 556, 556, 556, 280, 556, 333, 737, 370, 556, 584, 333, 737, 333, 400, 549, 333, 333, 333, 576, 556, 278, 333,
              333, 365, 556, 834, 834, 834, 611, 722, 722, 722, 722, 722, 722, 1000, 722, 667, 667, 667, 667, 278, 278, 278, 278, 722, 722,
              778, 778, 778, 778, 778, 584, 778, 722, 722, 722, 722, 667, 667, 611, 556, 556, 556, 556, 556, 556, 889, 556, 556, 556, 556,
              556, 278, 278, 278, 278, 611, 611, 611, 611, 611, 611, 611, 549, 611, 611, 611, 611, 611, 556, 611, 556]
# What Windows-1252 has between 128 and 159 that turns up in names: dashes, quotes, the ellipsis.
_SPECIAL = {0x80: 556, 0x85: 1000, 0x91: 222, 0x92: 222, 0x93: 333, 0x94: 333, 0x95: 350, 0x96: 556, 0x97: 1000, 0x99: 1000}

# Name in the page's resources, PDF base font, widths. Oblique is the regular face slanted: same widths.
FONTS = {"regular": ("F1", "Helvetica", _REGULAR, _REGULAR_HIGH),
         "bold": ("F2", "Helvetica-Bold", _BOLD, _BOLD_HIGH),
         "italic": ("F3", "Helvetica-Oblique", _REGULAR, _REGULAR_HIGH)}

ELLIPSIS = "…"


def encode(text):
    """Text as the bytes the fonts are used with (Windows-1252); what that cannot say becomes "?"."""
    return "".join(c if c >= " " else " " for c in str(text)).encode("cp1252", "replace")


def text_width(text, size, font="regular"):
    """How wide `text` is when set at `size` points."""
    _, _, low, high = FONTS[font]
    total = 0
    for code in encode(text):
        if 32 <= code <= 126:
            total += low[code - 32]
        elif code >= 160:
            total += high[code - 160]
        else:
            total += _SPECIAL.get(code, 556)
    return total * size / 1000.0


def fit(text, width, size, font="regular"):
    """`text`, cut short and ended with an ellipsis if it is wider than `width`."""
    text = str(text)
    if text_width(text, size, font) <= width:
        return text
    low, high = 0, len(text)
    while low < high:    # the longest beginning that fits with the ellipsis after it
        middle = (low + high + 1) // 2
        if text_width(text[:middle].rstrip() + ELLIPSIS, size, font) <= width:
            low = middle
        else:
            high = middle - 1
    return text[:low].rstrip() + ELLIPSIS if low else ""


def _number(value):
    """A number as PDF wants it: plain decimals, no exponent, no needless digits."""
    if not math.isfinite(value):
        value = 0.0
    written = f"{value:.3f}".rstrip("0").rstrip(".")
    return "0" if written in ("", "-0") else written


def _shade(shade, stroke):
    if isinstance(shade, (int, float)):
        return f"{_number(min(max(shade, 0.0), 1.0))} {'G' if stroke else 'g'}"
    red, green, blue = (min(max(part, 0.0), 1.0) for part in shade)
    return f"{_number(red)} {_number(green)} {_number(blue)} {'RG' if stroke else 'rg'}"


class Page:
    """One sheet. Every drawing call adds to it at once; nothing can be taken back."""

    def __init__(self, width, height):
        self.width = width
        self.height = height
        self._out = []
        self._clips = 0

    # ---- shapes ----

    def _paint(self, path, stroke, fill, width, dash):
        """Draws a path that is already described: filled with `fill`, outlined in `stroke`, either or both."""
        if stroke is None and fill is None:
            return
        state = []
        if fill is not None:
            state.append(_shade(fill, False))
        if stroke is not None:
            state.append(_shade(stroke, True))
            state.append(f"{_number(width)} w")
            state.append(f"[{' '.join(_number(part) for part in dash)}] 0 d" if dash else "[] 0 d")
        operator = "B" if stroke is not None and fill is not None else "S" if stroke is not None else "f"
        self._out.append(f"{' '.join(state)} {path} {operator}")

    def line(self, x1, y1, x2, y2, stroke=0.0, width=0.5, dash=None):
        self._paint(f"{_number(x1)} {_number(y1)} m {_number(x2)} {_number(y2)} l", stroke, None, width, dash)

    def polygon(self, points, stroke=0.0, fill=None, width=0.5, dash=None, closed=True):
        """Straight lines through `points`; closed back to the first unless `closed` is false."""
        points = list(points)
        if len(points) < 2:
            return
        path = [f"{_number(points[0][0])} {_number(points[0][1])} m"]
        path += [f"{_number(x)} {_number(y)} l" for x, y in points[1:]]
        if closed:
            path.append("h")
        self._paint(" ".join(path), stroke, fill if closed else None, width, dash)

    def rect(self, x, y, width, height, stroke=0.0, fill=None, line=0.5, dash=None):
        self._paint(f"{_number(x)} {_number(y)} {_number(width)} {_number(height)} re", stroke, fill, line, dash)

    def circle(self, x, y, radius, stroke=0.0, fill=None, width=0.5, dash=None):
        k = radius * 0.5522847498    # four curves that are a circle to within a fortieth of a percent
        path = [f"{_number(x + radius)} {_number(y)} m"]
        for (ax, ay), (bx, by), (cx, cy) in (((radius, k), (k, radius), (0, radius)), ((-k, radius), (-radius, k), (-radius, 0)),
                                             ((-radius, -k), (-k, -radius), (0, -radius)), ((k, -radius), (radius, -k), (radius, 0))):
            path.append(f"{_number(x + ax)} {_number(y + ay)} {_number(x + bx)} {_number(y + by)} {_number(x + cx)} {_number(y + cy)} c")
        path.append("h")
        self._paint(" ".join(path), stroke, fill, width, dash)

    # ---- text ----

    def text(self, x, y, text, size=8.0, font="regular", align="left", shade=0.0, angle=0.0):
        """Sets `text` with its baseline starting (or, by `align`, centred or ending) at x, y.
        `angle` turns it anticlockwise, in degrees, about that point."""
        data = encode(text)
        if not data.strip():
            return
        shift = 0.0 if align == "left" else text_width(text, size, font) * (0.5 if align == "center" else 1.0)
        cos, sin = math.cos(math.radians(angle)), math.sin(math.radians(angle))
        x -= shift * cos
        y -= shift * sin
        escaped = data.replace(b"\\", b"\\\\").replace(b"(", b"\\(").replace(b")", b"\\)").decode("latin-1")
        placed = f"{_number(x)} {_number(y)} Td" if not angle else \
            f"{_number(cos)} {_number(sin)} {_number(-sin)} {_number(cos)} {_number(x)} {_number(y)} Tm"
        self._out.append(f"BT {_shade(shade, False)} /{FONTS[font][0]} {_number(size)} Tf {placed} ({escaped}) Tj ET")

    # ---- clipping ----

    def clip(self, x, y, width, height):
        """Until `unclip`, nothing is drawn outside this rectangle."""
        self._out.append(f"q {_number(x)} {_number(y)} {_number(width)} {_number(height)} re W n")
        self._clips += 1

    def unclip(self):
        if self._clips:
            self._out.append("Q")
            self._clips -= 1

    def content(self):
        return ("\n".join(self._out + ["Q"] * self._clips) + "\n").encode("latin-1")


class Document:
    def __init__(self, title="", author="", created=None):
        self.title = title
        self.author = author
        self.created = time.localtime() if created is None else created
        self.pages = []

    def page(self, width, height):
        page = Page(width, height)
        self.pages.append(page)
        return page

    def data(self):
        """The whole file."""
        def words(text):    # any text at all, as PDF's two-bytes-a-character string
            return "<FEFF" + str(text).encode("utf-16-be").hex().upper() + ">"

        objects = []    # each one's body; object n is objects[n - 1]

        def add(body):
            objects.append(body if isinstance(body, bytes) else body.encode("latin-1"))
            return len(objects)

        catalog = add(b"")
        tree = add(b"")
        fonts = {}
        for name, base, _, _ in FONTS.values():
            fonts[name] = add(f"<< /Type /Font /Subtype /Type1 /BaseFont /{base} /Encoding /WinAnsiEncoding >>")
        resources = "<< /Font << " + " ".join(f"/{name} {number} 0 R" for name, number in fonts.items()) + " >> >>"
        kids = []
        for page in self.pages:
            packed = zlib.compress(page.content(), 9)
            stream = add(f"<< /Length {len(packed)} /Filter /FlateDecode >>\nstream\n".encode("latin-1") + packed + b"\nendstream")
            kids.append(add(f"<< /Type /Page /Parent {tree} 0 R /MediaBox [0 0 {_number(page.width)} {_number(page.height)}] "
                            f"/Resources {resources} /Contents {stream} 0 R >>"))
        objects[tree - 1] = f"<< /Type /Pages /Count {len(kids)} /Kids [{' '.join(f'{kid} 0 R' for kid in kids)}] >>".encode("latin-1")
        objects[catalog - 1] = f"<< /Type /Catalog /Pages {tree} 0 R >>".encode("latin-1")
        facts = f"<< /Title {words(self.title)} /Producer {words('Openviz')} /CreationDate (D:{time.strftime('%Y%m%d%H%M%S', self.created)})"
        if self.author:
            facts += f" /Author {words(self.author)}"
        info = add(facts + " >>")

        out = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
        offsets = []
        for number, body in enumerate(objects, 1):
            offsets.append(len(out))
            out += f"{number} 0 obj\n".encode("latin-1") + body + b"\nendobj\n"
        table = len(out)
        out += f"xref\n0 {len(objects) + 1}\n".encode("latin-1") + b"0000000000 65535 f \n"
        for offset in offsets:
            out += f"{offset:010d} 00000 n \n".encode("latin-1")
        out += f"trailer\n<< /Size {len(objects) + 1} /Root {catalog} 0 R /Info {info} 0 R >>\nstartxref\n{table}\n%%EOF\n".encode("latin-1")
        return bytes(out)

    def save(self, path):
        with open(path, "wb") as handle:
            handle.write(self.data())
