"""
86Box-Next icon kit: a tiny vector-ish drawing layer on top of Pillow.

Glyphs are drawn in a 32x32 unit space onto a large supersampled canvas and
downsampled per output size, which gives smooth, anti-aliased icons without an
SVG renderer.  A Canvas carries a transform, so a glyph can be drawn shrunk and
offset (badges, composites) with the same code.
"""
import math
import os
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageChops

SS = 32            # supersampling: pixels per unit (32 units -> 1024 px)
GRID = 32

FONT_DIR = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts")


def font(name, px):
    for cand in name if isinstance(name, (list, tuple)) else [name]:
        p = os.path.join(FONT_DIR, cand)
        if os.path.exists(p):
            return ImageFont.truetype(p, int(px))
    return ImageFont.load_default()


BOLD = ["segoeuib.ttf", "bahnschrift.ttf"]
SEMI = ["seguisb.ttf", "segoeuib.ttf"]
JP = ["YuGothB.ttc", "msgothic.ttc"]


def hexc(h, a=255):
    h = h.lstrip("#")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), a)


def mix(c1, c2, t):
    return tuple(int(round(c1[i] + (c2[i] - c1[i]) * t)) for i in range(4))


def shade(c, f):
    """f > 0 lightens toward white, f < 0 darkens toward black."""
    if f >= 0:
        return mix(c, (255, 255, 255, c[3]), f)
    return mix(c, (0, 0, 0, c[3]), -f)


# --------------------------------------------------------------- palette ---
P = {
    "blue":    hexc("#3B82F6"), "blue_d":   hexc("#1D4ED8"),
    "sky":     hexc("#38BDF8"),
    "indigo":  hexc("#6366F1"),
    "violet":  hexc("#8B5CF6"),
    "purple":  hexc("#A855F7"),
    "pink":    hexc("#EC4899"),
    "red":     hexc("#EF4444"), "red_d":    hexc("#B91C1C"),
    "orange":  hexc("#F97316"),
    "amber":   hexc("#F59E0B"),
    "yellow":  hexc("#FACC15"),
    "lime":    hexc("#84CC16"),
    "green":   hexc("#22C55E"), "green_d":  hexc("#15803D"),
    "emerald": hexc("#10B981"),
    "teal":    hexc("#14B8A6"),
    "cyan":    hexc("#06B6D4"),
    "slate":   hexc("#64748B"),
    "slate_l": hexc("#CBD5E1"),
    "slate_xl": hexc("#E2E8F0"),
    "slate_d": hexc("#334155"),
    "ink":     hexc("#1E293B"),
    "white":   hexc("#FFFFFF"),
    "paper":   hexc("#F8FAFC"),
    "gold":    hexc("#F5B83D"), "gold_d":   hexc("#B7791F"),
    "silver":  hexc("#D7DEE8"),
    "brown":   hexc("#8B5E3C"),
    "pcb":     hexc("#16A34A"), "pcb_d":    hexc("#166534"),
}

OUTLINE = hexc("#0F172A", 150)   # soft dark outline: reads on light and dark


class Canvas:
    def __init__(self, size=GRID, ss=SS, img=None, ox=0.0, oy=0.0, k=1.0):
        self.ss = ss
        self.img = img if img is not None else Image.new("RGBA", (size * ss, size * ss), (0, 0, 0, 0))
        self.ox, self.oy, self.k = ox, oy, k

    # transform -----------------------------------------------------------
    def sub(self, ox, oy, k):
        """A canvas drawing into the same image, shrunk by k and moved."""
        return Canvas(img=self.img, ss=self.ss, ox=self.ox + ox * self.k, oy=self.oy + oy * self.k, k=self.k * k)

    def X(self, x):
        return (self.ox + x * self.k) * self.ss

    def Y(self, y):
        return (self.oy + y * self.k) * self.ss

    def L(self, v):
        return v * self.k * self.ss

    def box(self, x, y, w, h):
        return [self.X(x), self.Y(y), self.X(x + w), self.Y(y + h)]

    def pts(self, pts):
        return [(self.X(x), self.Y(y)) for x, y in pts]

    # painting ------------------------------------------------------------
    def _paint(self, draw_mask, fill, bbox=None, alpha=1.0):
        mask = Image.new("L", self.img.size, 0)
        draw_mask(ImageDraw.Draw(mask))
        if alpha < 1.0:
            mask = mask.point(lambda v: int(v * alpha))
        if isinstance(fill, tuple) and len(fill) == 4 and isinstance(fill[0], int):
            layer = Image.new("RGBA", self.img.size, fill)
        else:
            layer = self._gradient(fill, bbox or mask.getbbox() or (0, 0, 1, 1))
        a = ImageChops.multiply(layer.getchannel("A"), mask)
        layer.putalpha(a)
        self.img.alpha_composite(layer)

    def _gradient(self, spec, bbox):
        """spec: (top, bottom) vertical, or ('h', left, right), or ('r', inner, outer)."""
        w, h = self.img.size
        layer = Image.new("RGBA", (w, h))
        x0, y0, x1, y1 = [int(v) for v in bbox]
        if spec[0] == "r":
            inner, outer = spec[1], spec[2]
            cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
            rad = max(1, max(x1 - x0, y1 - y0) / 2)
            g = Image.radial_gradient("L").resize((int(rad * 2), int(rad * 2)))
            col = Image.new("RGBA", g.size)
            ci = Image.new("RGBA", g.size, inner)
            co = Image.new("RGBA", g.size, outer)
            col = Image.composite(co, ci, g)
            layer.paste(col, (int(cx - rad), int(cy - rad)))
            return layer
        horiz = spec[0] == "h"
        c1, c2 = (spec[1], spec[2]) if horiz else (spec[0], spec[1])
        n = max(1, (x1 - x0) if horiz else (y1 - y0))
        strip = Image.new("RGBA", (n, 1) if horiz else (1, n))
        for i in range(n):
            strip.putpixel((i, 0) if horiz else (0, i), mix(c1, c2, i / max(1, n - 1)))
        strip = strip.resize((max(1, x1 - x0), max(1, y1 - y0)))
        layer.paste(strip, (x0, y0))
        return layer

    def rect(self, x, y, w, h, r=0, fill=None, outline=OUTLINE, width=0.9, alpha=1.0):
        b = self.box(x, y, w, h)
        rr = self.L(r)
        if fill is not None:
            self._paint(lambda d: d.rounded_rectangle(b, rr, fill=255), fill, b, alpha)
        if outline is not None and width:
            self._stroke(lambda d, wpx: d.rounded_rectangle(b, rr, outline=255, width=wpx), outline, width)

    def ellipse(self, x, y, w, h, fill=None, outline=OUTLINE, width=0.9, alpha=1.0):
        b = self.box(x, y, w, h)
        if fill is not None:
            self._paint(lambda d: d.ellipse(b, fill=255), fill, b, alpha)
        if outline is not None and width:
            self._stroke(lambda d, wpx: d.ellipse(b, outline=255, width=wpx), outline, width)

    def circle(self, cx, cy, r, **kw):
        self.ellipse(cx - r, cy - r, 2 * r, 2 * r, **kw)

    def poly(self, pts, fill=None, outline=OUTLINE, width=0.9, alpha=1.0):
        p = self.pts(pts)
        xs = [q[0] for q in p]
        ys = [q[1] for q in p]
        b = [min(xs), min(ys), max(xs), max(ys)]
        if fill is not None:
            self._paint(lambda d: d.polygon(p, fill=255), fill, b, alpha)
        if outline is not None and width:
            self.line(list(pts) + [pts[0]], outline, width)

    def pie(self, x, y, w, h, start, end, fill, alpha=1.0):
        b = self.box(x, y, w, h)
        self._paint(lambda d: d.pieslice(b, start, end, fill=255), fill, b, alpha)

    def arc(self, x, y, w, h, start, end, color, width=1.5):
        b = self.box(x, y, w, h)
        wpx = max(1, int(self.L(width)))
        self._paint(lambda d: d.arc(b, start, end, fill=255, width=wpx), color)
        # round caps
        cx, cy = x + w / 2, y + h / 2
        for ang in (start, end):
            a = math.radians(ang)
            px = cx + (w / 2 - width / 2) * math.cos(a)
            py = cy + (h / 2 - width / 2) * math.sin(a)
            self.ellipse(px - width / 2, py - width / 2, width, width, fill=color, outline=None)

    def line(self, pts, color, width=1.5, caps=True):
        p = self.pts(pts)
        wpx = max(1, int(self.L(width)))
        self._paint(lambda d: d.line(p, fill=255, width=wpx, joint="curve"), color)
        if caps:
            for (x, y) in (pts[0], pts[-1]):
                self.ellipse(x - width / 2, y - width / 2, width, width, fill=color, outline=None)

    def _stroke(self, draw, color, width):
        wpx = max(1, int(self.L(width)))
        self._paint(lambda d: draw(d, wpx), color)

    def text(self, x, y, s, size, color, face=BOLD, anchor="mm"):
        f = font(face, self.L(size))
        self._paint(lambda d: d.text((self.X(x), self.Y(y)), s, font=f, fill=255, anchor=anchor), color)

    def gloss(self, x, y, w, h, r=0, strength=0.35):
        """A soft top highlight over a shape's upper half."""
        self.rect(x + 0.6, y + 0.6, w - 1.2, h * 0.45, max(0, r - 0.6),
                  fill=(hexc("#FFFFFF", int(255 * strength)), hexc("#FFFFFF", 0)), outline=None)

    def shadow(self, draw, blur=0.8, dy=0.6, opacity=0.35):
        """Drop shadow for whatever draw(canvas) paints."""
        tmp = Canvas(img=Image.new("RGBA", self.img.size), ss=self.ss, ox=self.ox, oy=self.oy + dy * self.k, k=self.k)
        draw(tmp)
        a = tmp.img.getchannel("A").filter(ImageFilter.GaussianBlur(self.L(blur)))
        a = a.point(lambda v: int(v * opacity))
        sh = Image.new("RGBA", self.img.size, (0, 0, 0, 255))
        sh.putalpha(a)
        self.img.alpha_composite(sh)


# ----------------------------------------------------------------- output ---
def render(draw, size=GRID):
    c = Canvas(size=size)
    draw(c)
    return c.img


def downsample(img, px):
    out = img.resize((px, px), Image.LANCZOS)
    if px <= 24:
        out = out.filter(ImageFilter.UnsharpMask(radius=0.6, percent=60, threshold=0))
    return out


def save_ico(img, path, sizes, small=None):
    """small: an optional simplified rendering used for sizes of 24 px and
    under, where the full glyph's detail would only blur."""
    imgs = [downsample(small if (small is not None and s <= 24) else img, s) for s in sizes]
    big = imgs[-1]
    big.save(path, format="ICO", sizes=[(s, s) for s in sizes], append_images=imgs[:-1])


def save_png(img, path, px):
    downsample(img, px).save(path)


def disabled(img):
    """Desaturate and fade: the device is there but not in use."""
    r, g, b, a = img.split()
    grey = Image.merge("RGB", (r, g, b)).convert("L")
    grey = grey.point(lambda v: int(150 + (v - 128) * 0.45))
    out = Image.merge("RGBA", (grey, grey, grey, a.point(lambda v: int(v * 0.6))))
    return out
