"""
86Box-Next icon glyphs, drawn in a 32x32 unit space.  See iconkit.py.

Each glyph is a function of a Canvas.  Composite icons (an image file, a
disabled drive, a settings page with a gear) are built from these in
make_icons.py.
"""
import math
from iconkit import P, OUTLINE, hexc, shade, mix, BOLD, SEMI, JP

W = P["white"]


def grad(c, top=0.25, bottom=-0.18):
    return (shade(c, top), shade(c, bottom))


# ============================================================ media =========

def floppy_35(c, body=P["blue"]):
    c.shadow(lambda s: s.rect(4, 3.5, 24, 25, 3, fill=body, outline=None))
    c.rect(4, 3.5, 24, 25, 3, fill=grad(body, 0.2, -0.2))
    # metal shutter
    c.rect(9.5, 3.5, 13, 9, 1, fill=(P["silver"], shade(P["silver"], -0.25)), width=0.7)
    c.rect(17, 5, 3, 5.5, 0.6, fill=P["slate_d"], outline=None)
    # label
    c.rect(7.5, 15.5, 17, 11, 1.4, fill=(W, P["slate_xl"]), width=0.7)
    for i in range(3):
        c.line([(10, 19 + i * 2.6), (22, 19 + i * 2.6)], hexc("#94A3B8"), 0.7)
    # write-protect notch
    c.rect(5.6, 24.6, 2, 2, 0.3, fill=P["ink"], outline=None)


def floppy_525(c):
    body = hexc("#2B2F3A")
    c.shadow(lambda s: s.rect(2.5, 2.5, 27, 27, 1.5, fill=body, outline=None))
    c.rect(2.5, 2.5, 27, 27, 1.5, fill=(hexc("#454B5A"), hexc("#1F232C")))
    # label strip
    c.rect(5, 4.5, 22, 5, 0.8, fill=(W, P["slate_xl"]), width=0.6)
    c.line([(7, 7), (19, 7)], P["red"], 0.9)
    # hub
    c.circle(16, 17.5, 4.6, fill=(hexc("#9AA3B2"), hexc("#5C6575")), width=0.6)
    c.circle(16, 17.5, 2.2, fill=hexc("#11141A"), outline=None)
    # read slot
    c.rect(14.3, 23, 3.4, 5.6, 1.7, fill=hexc("#11141A"), outline=None)
    # index hole
    c.circle(10.5, 17.5, 0.9, fill=hexc("#11141A"), outline=None)


def cd_disc(c, x=2, y=2, d=28, tint=None):
    r = d / 2
    cx, cy = x + r, y + r
    c.shadow(lambda s: s.circle(cx, cy, r, fill=P["slate"], outline=None))
    c.circle(cx, cy, r, fill=("r", W, tint or hexc("#C9D3E0")))
    # iridescent sheen
    c.pie(x + 1, y + 1, d - 2, d - 2, 200, 245, hexc("#7DD3FC", 150))
    c.pie(x + 1, y + 1, d - 2, d - 2, 245, 280, hexc("#C4B5FD", 140))
    c.pie(x + 1, y + 1, d - 2, d - 2, 20, 60, hexc("#FDE68A", 150))
    c.pie(x + 1, y + 1, d - 2, d - 2, 60, 95, hexc("#A7F3D0", 140))
    c.circle(cx, cy, r * 0.36, fill=(W, P["slate_xl"]), width=0.6)
    c.circle(cx, cy, r * 0.16, fill=P["slate_l"], width=0.6)
    c.circle(cx, cy, r, fill=None, width=0.9)


def cdrom(c):
    cd_disc(c)


def dvdrom(c):
    cd_disc(c, tint=hexc("#93A8F5"))
    c.text(16, 25.2, "DVD", 5.2, P["blue_d"], BOLD)


def hard_disk(c):
    c.shadow(lambda s: s.rect(3, 4, 26, 24, 2.5, fill=P["slate"], outline=None))
    c.rect(3, 4, 26, 24, 2.5, fill=(hexc("#E5EAF1"), hexc("#AEB8C6")))
    c.circle(14, 15.5, 9, fill=("r", W, hexc("#B9C3D1")), width=0.7)
    c.circle(14, 15.5, 2.2, fill=P["slate"], width=0.5)
    c.line([(25, 7.5), (16, 16)], P["slate_d"], 1.6)
    c.circle(25, 7.5, 1.8, fill=P["slate_d"], outline=None)
    c.circle(6, 25, 0.9, fill=P["green"], outline=None)
    for sx, sy in ((5.5, 6.5), (26.5, 25.5)):
        c.circle(sx, sy, 0.7, fill=P["slate"], outline=None)


def cartridge_body(c, body, label=W, notch=True):
    c.shadow(lambda s: s.rect(4, 3, 24, 26, 3, fill=body, outline=None))
    c.rect(4, 3, 24, 26, 3, fill=grad(body, 0.25, -0.2))
    if notch:
        c.rect(12, 3, 8, 3, 0.8, fill=shade(body, -0.35), outline=None)
    c.rect(7.5, 9, 17, 11, 1.5, fill=(label, shade(label, -0.08)), width=0.6)


def zip_disk(c):
    cartridge_body(c, P["violet"])
    c.text(16, 14.7, "Z", 9, P["violet"], BOLD)
    c.rect(10, 23, 12, 3.5, 1, fill=shade(P["violet"], -0.3), outline=None)


def jaz(c):
    cartridge_body(c, P["teal"])
    c.text(16, 14.7, "J", 9, P["teal"], BOLD)
    c.rect(10, 23, 12, 3.5, 1, fill=shade(P["teal"], -0.3), outline=None)


def syquest(c):
    body = hexc("#9F1239")
    cartridge_body(c, body, notch=False)
    c.circle(16, 14.5, 4, fill=("r", W, P["slate_l"]), width=0.5)
    c.circle(16, 14.5, 1.2, fill=P["slate"], outline=None)
    c.rect(9, 23, 14, 3.5, 1, fill=shade(body, -0.35), outline=None)


def rdisk(c):
    cartridge_body(c, P["cyan"])
    c.rect(12.5, 11, 7, 7, 1, fill=P["cyan"], outline=None)
    c.rect(14, 12.5, 4, 4, 0.6, fill=W, outline=None)
    c.rect(10, 23, 12, 3.5, 1, fill=shade(P["cyan"], -0.3), outline=None)


def mo(c):
    body = P["indigo"]
    c.shadow(lambda s: s.rect(4, 3, 24, 26, 2.5, fill=body, outline=None))
    c.rect(4, 3, 24, 26, 2.5, fill=grad(body))
    c.rect(9, 3, 14, 14, 1.2, fill=(P["silver"], shade(P["silver"], -0.25)), width=0.6)
    c.circle(16, 10, 4.5, fill=("r", W, hexc("#C7D2FE")), width=0.5)
    c.circle(16, 10, 1.3, fill=P["slate"], outline=None)
    c.rect(8, 20, 16, 6.5, 1.2, fill=(W, P["slate_xl"]), width=0.5)
    c.text(16, 23.4, "MO", 4.6, body, BOLD)


def tape(c):
    body = hexc("#475569")
    c.shadow(lambda s: s.rect(2.5, 6, 27, 20, 2.5, fill=body, outline=None))
    c.rect(2.5, 6, 27, 20, 2.5, fill=grad(body, 0.2, -0.25))
    c.rect(6, 9, 20, 11, 1.8, fill=(hexc("#1E293B"), hexc("#0F172A")), width=0.6)
    for x in (11, 21):
        c.circle(x, 14.5, 3.4, fill=("r", hexc("#A16207"), hexc("#713F12")), outline=None)
        c.circle(x, 14.5, 1.3, fill=W, outline=None)
    c.rect(10, 21.5, 12, 2.6, 0.8, fill=P["amber"], outline=None)


def cassette(c):
    body = hexc("#F1E9D6")
    c.shadow(lambda s: s.rect(2, 6, 28, 20, 2.2, fill=body, outline=None))
    c.rect(2, 6, 28, 20, 2.2, fill=(body, hexc("#D8CBAE")))
    c.rect(4.5, 8, 23, 4, 0.8, fill=P["orange"], outline=None)
    c.rect(7, 13, 18, 7.5, 3.6, fill=hexc("#334155"), width=0.6)
    for x in (11, 21):
        c.circle(x, 16.75, 2.5, fill=W, width=0.5)
        c.circle(x, 16.75, 0.9, fill=P["slate_d"], outline=None)
    c.poly([(8, 26), (10, 22), (22, 22), (24, 26)], fill=hexc("#C9BB9A"), width=0.6)


def cartridge(c):
    body = hexc("#94A3B8")
    c.shadow(lambda s: s.rect(6, 2, 20, 28, 2, fill=body, outline=None))
    c.poly([(6, 4), (8, 2), (24, 2), (26, 4), (26, 30), (6, 30)], fill=grad(body, 0.25, -0.15))
    c.rect(9, 6, 14, 11, 1.2, fill=(P["red"], P["red_d"]), width=0.6)
    c.text(16, 11.4, "PCjr", 4.4, W, BOLD)
    for i in range(5):
        c.line([(9, 20 + i * 2), (23, 20 + i * 2)], shade(body, -0.25), 0.7)


# ============================================================ hardware ======

def chip(c, body=hexc("#1F2937"), die=P["gold"], pins=True):
    if pins:
        for i in range(5):
            o = 8.5 + i * 3.75
            for (x, y, w, h) in ((o - 0.8, 2.5, 1.6, 3), (o - 0.8, 26.5, 1.6, 3), (2.5, o - 0.8, 3, 1.6), (26.5, o - 0.8, 3, 1.6)):
                c.rect(x, y, w, h, 0.4, fill=(hexc("#E2E8F0"), hexc("#94A3B8")), outline=None)
    c.shadow(lambda s: s.rect(5.5, 5.5, 21, 21, 2.5, fill=body, outline=None))
    c.rect(5.5, 5.5, 21, 21, 2.5, fill=grad(body, 0.25, -0.2))
    c.rect(10.5, 10.5, 11, 11, 1.5, fill=(shade(die, 0.25), shade(die, -0.2)), width=0.6)


def processor(c):
    chip(c)
    c.text(16, 16.3, "x86", 4.6, P["ink"], BOLD)


def interpreter(c):
    chip(c, die=P["sky"])
    c.text(16, 16.5, "i", 8, P["ink"], BOLD)


def recompiler(c):
    chip(c, die=P["amber"])
    c.poly([(17.5, 11), (12.5, 17), (15.8, 17), (14.5, 21), (19.5, 15), (16.2, 15)], fill=P["ink"], outline=None)


def monitor(c, screen=None):
    c.shadow(lambda s: s.rect(2, 4, 28, 19, 2.5, fill=P["slate"], outline=None))
    c.rect(2, 4, 28, 19, 2.5, fill=(hexc("#475569"), hexc("#1E293B")))
    c.rect(4, 6, 24, 15, 1.2, fill=screen or (hexc("#60A5FA"), hexc("#1D4ED8")), outline=None)
    c.poly([(5, 6.5), (16, 6.5), (6, 16)], fill=hexc("#FFFFFF", 55), outline=None)
    c.rect(13, 23, 6, 3.5, 0, fill=(hexc("#64748B"), hexc("#475569")), outline=None)
    c.rect(8, 26, 16, 3, 1.5, fill=(hexc("#94A3B8"), hexc("#475569")))


def display(c):
    monitor(c)


def machine(c):
    # tower + monitor
    c.shadow(lambda s: s.rect(18, 4, 12, 25, 2, fill=P["slate"], outline=None))
    c.rect(18, 4, 12, 25, 2, fill=(hexc("#E2E8F0"), hexc("#A3AEBE")))
    c.rect(20, 7, 8, 2, 0.6, fill=P["slate_d"], outline=None)
    c.rect(20, 10.5, 8, 2, 0.6, fill=P["slate_d"], outline=None)
    c.circle(24, 23, 1.6, fill=P["green"], width=0.5)
    m = c.sub(0, 4, 0.66)
    monitor(m)


def network(c):
    c.shadow(lambda s: s.circle(16, 16, 13, fill=P["blue"], outline=None))
    c.circle(16, 16, 13, fill=("r", hexc("#7DD3FC"), hexc("#1D4ED8")))
    col = hexc("#FFFFFF", 200)
    c.ellipse(10.2, 3, 11.6, 26, fill=None, outline=col, width=1.1)
    c.line([(16, 3.2), (16, 28.8)], col, 1.1)
    c.line([(3.4, 16), (28.6, 16)], col, 1.1)
    c.arc(5, -2, 22, 13, 30, 150, col, 1.0)
    c.arc(5, 21, 22, 13, 210, 330, col, 1.0)
    c.circle(16, 16, 13, fill=None, width=0.9)


def connector(c, body, pins, x0=3, w=26):
    c.shadow(lambda s: s.poly([(x0, 9), (x0 + w, 9), (x0 + w - 3, 23), (x0 + 3, 23)], fill=body, outline=None))
    c.poly([(x0, 9), (x0 + w, 9), (x0 + w - 3, 23), (x0 + 3, 23)], fill=(hexc("#E2E8F0"), hexc("#94A3B8")))
    c.poly([(x0 + 2, 11), (x0 + w - 2, 11), (x0 + w - 4.3, 21), (x0 + 4.3, 21)], fill=grad(body), width=0.6)
    top, bot = pins
    for i in range(top):
        x = x0 + 4.5 + i * (w - 9) / max(1, top - 1)
        c.circle(x, 14, 0.95, fill=hexc("#FDE68A"), outline=None)
    for i in range(bot):
        x = x0 + 6 + i * (w - 12) / max(1, bot - 1)
        c.circle(x, 18, 0.95, fill=hexc("#FDE68A"), outline=None)


def serial_ports(c):
    connector(c, P["teal"], (5, 4), x0=6, w=20)


def parallel_ports(c):
    connector(c, P["pink"], (7, 6), x0=2, w=28)


def ports(c):
    # a plug and its cable
    c.line([(4, 28), (9, 23), (12, 23)], P["slate_d"], 2.4)
    c.shadow(lambda s: s.rect(11, 15, 12, 12, 2, fill=P["slate"], outline=None))
    c.poly([(11, 18), (19, 10), (26, 17), (18, 25)], fill=(hexc("#E2E8F0"), hexc("#94A3B8")))
    c.line([(20.5, 9.5), (24, 6)], P["gold_d"], 1.6)
    c.line([(24.5, 13.5), (28, 10)], P["gold_d"], 1.6)


def storage_controllers(c):
    for i, y in enumerate((19, 12.5, 6)):
        c.shadow(lambda s, y=y: s.rect(5, y, 22, 8, 4, fill=P["slate"], outline=None), dy=0.4)
        c.ellipse(5, y + 3, 22, 7, fill=(hexc("#93C5FD"), hexc("#1D4ED8")))
        c.rect(5, y + 2, 22, 4.5, 0, fill=("h", hexc("#3B82F6"), hexc("#1E40AF")), outline=None)
        c.line([(5, y + 2), (5, y + 6.5)], OUTLINE, 0.9, caps=False)
        c.line([(27, y + 2), (27, y + 6.5)], OUTLINE, 0.9, caps=False)
        c.ellipse(5, y - 1, 22, 6.5, fill=(hexc("#DBEAFE"), hexc("#93C5FD")))
        c.circle(23, y + 5.2, 0.8, fill=P["green"], outline=None)


def expansion_card(c, pcb=P["pcb"]):
    c.shadow(lambda s: s.rect(2.5, 6, 27, 18, 1.5, fill=pcb, outline=None))
    c.rect(2.5, 6, 27, 18, 1.5, fill=grad(pcb, 0.15, -0.25))
    for i in range(10):
        c.rect(6 + i * 2.2, 24, 1.4, 3.5, 0.3, fill=(P["gold"], P["gold_d"]), outline=None)
    c.rect(5, 9, 9, 8, 1, fill=(hexc("#334155"), hexc("#0F172A")), width=0.5)
    c.rect(16, 9, 5, 5, 0.6, fill=(hexc("#334155"), hexc("#0F172A")), width=0.5)
    c.rect(23, 9, 4, 11, 0.6, fill=(P["silver"], hexc("#94A3B8")), width=0.5)
    c.line([(6, 20.5), (20, 20.5)], hexc("#BBF7D0", 160), 0.6)


def other_peripherals(c):
    expansion_card(c)


def usb(c):
    """The USB trident: a stem from a round foot to an arrowhead, a branch
    ending in a circle on the left and one ending in a square on the right."""
    col = P["blue"]
    w = 2.2
    c.line([(16, 26), (16, 7)], col, w)
    c.poly([(16, 2.5), (20.5, 9), (11.5, 9)], fill=col, outline=None)
    c.line([(16, 20), (8.5, 15.5), (8.5, 12)], col, w)
    c.circle(8.5, 10.5, 2.6, fill=col, outline=None)
    c.line([(16, 17.5), (23.5, 13), (23.5, 10)], col, w)
    c.rect(21.2, 6.2, 4.6, 4.6, 0.3, fill=col, outline=None)
    c.circle(16, 27, 3.2, fill=col, outline=None)


def pcmcia(c):
    """A PC Card, connector end down: the metal shell, its label, and the
    68-pin socket edge."""
    shell = P["silver"]
    c.shadow(lambda s: s.rect(6, 2.5, 20, 27, 1.6, fill=shell, outline=None))
    c.rect(6, 2.5, 20, 27, 1.6, fill=(shell, shade(shell, -0.3)), width=0.7)
    c.rect(8.5, 5, 15, 15, 1, fill=(P["blue"], shade(P["blue"], -0.3)), width=0.5)
    c.text(16, 12.6, "PC", 5.2, W, BOLD)
    c.rect(7.5, 24.5, 17, 3.5, 0.5, fill=(hexc("#334155"), hexc("#0F172A")), outline=None)
    for i in range(7):
        c.rect(8.6 + i * 2.25, 25.5, 1.2, 1.5, 0.2, fill=(P["gold"], P["gold_d"]), outline=None)


def isa_memory(c):
    pcb = P["pcb"]
    c.shadow(lambda s: s.rect(2, 9, 28, 13, 1.2, fill=pcb, outline=None))
    c.rect(2, 9, 28, 13, 1.2, fill=grad(pcb, 0.15, -0.25))
    for i in range(4):
        c.rect(4 + i * 6.5, 11, 5, 7.5, 0.6, fill=(hexc("#334155"), hexc("#0F172A")), width=0.5)
    for i in range(12):
        c.rect(3.5 + i * 2.15, 22, 1.3, 3, 0.3, fill=(P["gold"], P["gold_d"]), outline=None)


def isa_rom(c):
    body = hexc("#1F2937")
    for i in range(6):
        for x in (6, 24.5):
            c.rect(x, 6 + i * 3.6, 1.6 if x > 10 else 1.6, 2, 0.3, fill=(hexc("#E2E8F0"), hexc("#94A3B8")), outline=None)
    c.shadow(lambda s: s.rect(7.5, 4, 17, 25, 1.8, fill=body, outline=None))
    c.rect(7.5, 4, 17, 25, 1.8, fill=grad(body, 0.25, -0.2))
    c.circle(16, 4, 2, fill=hexc("#0B0F17"), outline=None)
    c.rect(9.5, 10, 13, 12, 1, fill=(P["paper"], P["slate_xl"]), width=0.5)
    c.text(16, 16.2, "ROM", 4.2, P["ink"], BOLD)


def sound(c, waves=True):
    c.shadow(lambda s: s.poly([(3, 12), (9, 12), (16, 5), (16, 27), (9, 20), (3, 20)], fill=P["slate"], outline=None))
    c.rect(3, 12, 7, 8, 1, fill=(hexc("#94A3B8"), hexc("#475569")))
    c.poly([(9, 12), (16, 5), (16, 27), (9, 20)], fill=(hexc("#CBD5E1"), hexc("#64748B")))
    if waves:
        c.arc(12, 9, 10, 14, -50, 50, P["blue"], 1.8)
        c.arc(10, 4.5, 18, 23, -50, 50, P["blue"], 1.8)


def midi(c):
    c.shadow(lambda s: s.rect(2, 7, 28, 19, 2, fill=P["slate"], outline=None))
    c.rect(2, 7, 28, 19, 2, fill=(hexc("#475569"), hexc("#1E293B")))
    c.rect(3.5, 11, 25, 13.5, 0.8, fill=W, width=0.5)
    for i in range(1, 7):
        x = 3.5 + i * 25 / 7
        c.line([(x, 11), (x, 24.5)], hexc("#94A3B8"), 0.5, caps=False)
    for i in (1, 2, 4, 5, 6):
        x = 3.5 + i * 25 / 7
        c.rect(x - 1.2, 11, 2.4, 7.5, 0.4, fill=P["ink"], outline=None)
    c.circle(6, 9, 0.8, fill=P["red"], outline=None)
    c.rect(19, 8.3, 8, 1.4, 0.6, fill=P["sky"], outline=None)


def keyboard(c, x=1.5, y=10, w=29, h=14):
    c.shadow(lambda s: s.rect(x, y, w, h, 2, fill=P["slate"], outline=None))
    c.rect(x, y, w, h, 2, fill=(hexc("#F1F5F9"), hexc("#CBD5E1")))
    kw = (w - 3) / 9
    for row in range(3):
        n = 9 - (1 if row == 2 else 0)
        for i in range(n):
            kx = x + 1.5 + i * kw + (kw / 2 if row == 1 else 0)
            if kx + kw - 0.6 > x + w - 1:
                continue
            c.rect(kx, y + 1.5 + row * 3.4, kw - 0.6, 2.7, 0.5, fill=(W, hexc("#E2E8F0")), width=0.35)
    c.rect(x + 1.5 + 2 * kw, y + h - 3.2, 5 * kw - 0.6, 2.2, 0.5, fill=(W, hexc("#E2E8F0")), width=0.35)


def input_devices(c):
    keyboard(c, 1, 14, 21, 13)
    c.shadow(lambda s: s.rect(21, 6, 9, 14, 4.5, fill=P["slate"], outline=None))
    c.rect(21, 6, 9, 14, 4.5, fill=(W, hexc("#CBD5E1")))
    c.line([(25.5, 6.5), (25.5, 11)], hexc("#94A3B8"), 0.6, caps=False)
    c.rect(24.8, 8, 1.4, 2.6, 0.7, fill=P["blue"], outline=None)
    c.line([(25.5, 6), (25.5, 3.5), (21, 2.5)], P["slate_d"], 0.8)


def key_bindings(c):
    keyboard(c, 1.5, 12, 29, 15)
    c.shadow(lambda s: s.rect(9, 2, 14, 10, 2, fill=P["slate"], outline=None))
    c.rect(9, 2, 14, 10, 2, fill=(P["blue"], P["blue_d"]))
    c.text(16, 7, "Ctrl", 4.6, W, BOLD)


def gear(c, cx=16, cy=16, r=13, col=None, teeth=8):
    col = col or (hexc("#CBD5E1"), hexc("#64748B"))
    pts = []
    for i in range(teeth * 4):
        a = 2 * math.pi * i / (teeth * 4)
        rr = r if (i % 4) in (0, 1) else r * 0.78
        pts.append((cx + rr * math.cos(a + math.pi / (teeth * 4)), cy + rr * math.sin(a + math.pi / (teeth * 4))))
    c.shadow(lambda s: s.poly(pts, fill=P["slate"], outline=None))
    c.poly(pts, fill=col)
    c.circle(cx, cy, r * 0.42, fill=(hexc("#F8FAFC"), hexc("#CBD5E1")), width=0.8)


def settings(c):
    gear(c)


def gear_badge(c):
    gear(c.sub(15, 15, 0.52), col=(hexc("#FCD34D"), hexc("#D97706")))


def performance(c):
    c.shadow(lambda s: s.pie(2, 4, 28, 28, 180, 360, P["slate"]))
    c.pie(2, 4, 28, 28, 180, 360, (hexc("#475569"), hexc("#1E293B")))
    c.arc(4.5, 6.5, 23, 23, 185, 245, P["green"], 2.4)
    c.arc(4.5, 6.5, 23, 23, 250, 300, P["amber"], 2.4)
    c.arc(4.5, 6.5, 23, 23, 305, 355, P["red"], 2.4)
    c.line([(16, 18), (24, 10)], W, 1.6)
    c.circle(16, 18, 2.2, fill=W, width=0.6)
    c.line([(2, 18), (30, 18)], OUTLINE, 0.9)


def emulator(c):
    """The Preferences > Emulator page: the app mark."""
    app_mark(c, P["green"])


# ============================================================ actions =======

def run(c):
    c.shadow(lambda s: s.poly([(8, 4), (28, 16), (8, 28)], fill=P["green"], outline=None))
    c.poly([(8, 4.5), (27.5, 16), (8, 27.5)], fill=(hexc("#4ADE80"), P["green_d"]))


def pause(c):
    for x in (6.5, 18.5):
        c.shadow(lambda s, x=x: s.rect(x, 4, 7, 24, 2, fill=P["amber"], outline=None))
        c.rect(x, 4, 7, 24, 2, fill=(hexc("#FCD34D"), hexc("#D97706")))


def fast_forward(c):
    for x in (2.5, 15.5):
        c.poly([(x, 6), (x + 14, 16), (x, 26)], fill=(hexc("#7DD3FC"), P["blue_d"]))


def rewind(c):
    for x in (29.5, 16.5):
        c.poly([(x, 6), (x - 14, 16), (x, 26)], fill=(hexc("#7DD3FC"), P["blue_d"]))


def record(c):
    c.shadow(lambda s: s.circle(16, 16, 12, fill=P["red"], outline=None))
    c.circle(16, 16, 12, fill=("r", hexc("#FCA5A5"), P["red_d"]))


def hard_reset(c):
    col = (hexc("#5EEAD4"), hexc("#0F766E"))
    c.shadow(lambda s: s.circle(16, 16, 13.5, fill=P["teal"], outline=None))
    c.circle(16, 16, 13.5, fill=col)
    c.arc(8.5, 8.5, 15, 15, -200, 60, W, 2.4)
    c.poly([(9.0, 6.6), (9.6, 13.8), (15.5, 10.2)], fill=W, outline=None)


def acpi_shutdown(c):
    c.shadow(lambda s: s.circle(16, 16, 13.5, fill=P["red"], outline=None))
    c.circle(16, 16, 13.5, fill=(hexc("#F87171"), P["red_d"]))
    c.arc(8.5, 9, 15, 15, -60, 240, W, 2.4)
    c.line([(16, 6.5), (16, 15)], W, 2.4)


def keycap(c, x, y, w, h, label, size=5, col=None):
    col = col or (W, hexc("#CBD5E1"))
    c.shadow(lambda s: s.rect(x, y, w, h, 1.6, fill=P["slate"], outline=None), dy=0.5)
    c.rect(x, y, w, h, 1.6, fill=col)
    c.rect(x + 1, y + 0.8, w - 2, h - 2.6, 1.2, fill=(W, hexc("#F1F5F9")), outline=None)
    c.text(x + w / 2, y + (h - 1.8) / 2 + 0.6, label, size, P["ink"], BOLD)


def send_cad(c):
    keycap(c, 1, 3, 14, 11, "Ctrl", 4.4)
    keycap(c, 17, 3, 14, 11, "Alt", 4.4)
    keycap(c, 8, 17, 16, 12, "Del", 5, col=(hexc("#FCA5A5"), hexc("#DC2626")))


def send_cae(c):
    keycap(c, 1, 3, 14, 11, "Ctrl", 4.4)
    keycap(c, 17, 3, 14, 11, "Alt", 4.4)
    keycap(c, 8, 17, 16, 12, "Esc", 5, col=(hexc("#93C5FD"), hexc("#2563EB")))


def camera(c):
    body = hexc("#334155")
    c.shadow(lambda s: s.rect(2, 8, 28, 19, 3, fill=body, outline=None))
    c.rect(10, 5, 10, 5, 1.2, fill=(hexc("#64748B"), body))
    c.rect(2, 8, 28, 19, 3, fill=(hexc("#64748B"), hexc("#1E293B")))
    c.circle(16, 17.5, 7, fill=(hexc("#E2E8F0"), hexc("#94A3B8")), width=0.7)
    c.circle(16, 17.5, 4.8, fill=("r", hexc("#93C5FD"), hexc("#1E3A8A")), width=0.6)
    c.circle(14.5, 16, 1.2, fill=hexc("#FFFFFF", 200), outline=None)
    c.rect(24, 10.5, 3.5, 2, 0.6, fill=P["amber"], outline=None)


def take_screenshot(c):
    camera(c)


def frame_corners(c, col=P["red"]):
    for (x, y, dx, dy) in ((1.5, 1.5, 1, 1), (30.5, 1.5, -1, 1), (1.5, 30.5, 1, -1), (30.5, 30.5, -1, -1)):
        c.line([(x, y + 6 * dy), (x, y), (x + 6 * dx, y)], col, 1.8)


def take_raw_screenshot(c):
    camera(c.sub(3, 3, 0.82))
    frame_corners(c)


def clipboard(c):
    c.shadow(lambda s: s.rect(3, 4, 20, 26, 2, fill=P["brown"], outline=None))
    c.rect(3, 4, 20, 26, 2, fill=(hexc("#C08A5B"), hexc("#7C4A26")))
    c.rect(5.5, 7.5, 15, 20, 1, fill=(W, P["slate_xl"]), width=0.5)
    c.rect(8, 2, 10, 5, 1.5, fill=(hexc("#E2E8F0"), hexc("#94A3B8")))


def copy_screenshot(c):
    clipboard(c)
    camera(c.sub(11, 13, 0.62))


def copy_raw_screenshot(c):
    clipboard(c)
    s = c.sub(11, 13, 0.62)
    camera(s)
    frame_corners(c.sub(10, 12, 0.68))


def app_mark(c, col, glyph=True):
    """The 86Box-Next mark: a rounded tile with a monitor showing '86' and the
    'Next' chevron."""
    c.shadow(lambda s: s.rect(1.5, 1.5, 29, 29, 7, fill=col, outline=None), blur=1, dy=0.8)
    c.rect(1.5, 1.5, 29, 29, 7, fill=(shade(col, 0.25), shade(col, -0.3)))
    c.gloss(1.5, 1.5, 29, 29, 7, 0.28)
    # monitor
    c.rect(5.5, 6.5, 21, 15, 2.2, fill=(hexc("#1E293B"), hexc("#0F172A")), outline=hexc("#FFFFFF", 230), width=1.1)
    c.rect(12.5, 21.5, 7, 2.2, 0, fill=hexc("#FFFFFF", 230), outline=None)
    c.rect(9, 23.5, 14, 2.2, 1.1, fill=hexc("#FFFFFF", 230), outline=None)
    if glyph:
        c.text(13.6, 14.2, "86", 8.2, W, BOLD)
        c.line([(21.6, 11.4), (23.8, 14), (21.6, 16.6)], shade(col, 0.45), 1.4)


def new_vm(c):
    monitor(c)
    plus_badge(c)


# ============================================================ status ========

def lock_key(c, label, on, face=BOLD, size=12):
    if on:
        col, top, txt = (hexc("#4ADE80"), P["green_d"]), hexc("#BBF7D0"), P["ink"]
    else:
        col, top, txt = (hexc("#64748B"), hexc("#1E293B")), hexc("#475569"), hexc("#E2E8F0")
    c.shadow(lambda s: s.rect(2, 2, 28, 28, 5, fill=P["slate"], outline=None))
    c.rect(2, 2, 28, 28, 5, fill=col)
    c.rect(4.5, 3.5, 23, 21.5, 4, fill=(shade(top, 0.15), top), outline=None)
    c.text(16, 14.5, label, size, txt, face)


def lock_arrow(c, on):
    lock_key(c, "", on)
    col = P["ink"] if on else hexc("#E2E8F0")
    c.line([(16, 7.5), (16, 21.5)], col, 2)
    c.poly([(11.5, 11), (16, 6), (20.5, 11)], fill=col, outline=None)
    c.poly([(11.5, 18), (16, 23), (20.5, 18)], fill=col, outline=None)


# ============================================================ overlays ======

def active(c):
    c.circle(25, 25, 6, fill=("r", hexc("#86EFAC"), P["green_d"]), outline=W, width=1.1)


def write_active(c):
    c.circle(7, 25, 6, fill=("r", hexc("#FDBA74"), hexc("#C2410C")), outline=W, width=1.1)


def disabled_badge(c):
    c.circle(25, 25, 6.2, fill=W, outline=None)
    c.circle(25, 25, 5.4, fill=None, outline=P["red"], width=1.6)
    c.line([(21.4, 28.6), (28.6, 21.4)], P["red"], 1.6, caps=False)


def write_protected(c):
    c.arc(3.2, 17, 7.6, 9, 180, 360, hexc("#64748B"), 1.6)
    c.rect(1.5, 21.5, 11, 9, 1.8, fill=(hexc("#FCD34D"), hexc("#D97706")), outline=W, width=0.9)
    c.circle(7, 25.5, 1.1, fill=P["ink"], outline=None)


def plus_badge(c):
    c.circle(25, 25, 6.5, fill=(hexc("#4ADE80"), P["green_d"]), outline=W, width=1.1)
    c.line([(25, 21.6), (25, 28.4)], W, 1.7)
    c.line([(21.6, 25), (28.4, 25)], W, 1.7)


def new(c):
    plus_badge(c)


def browse(c):
    c.line([(15, 15), (24, 24)], P["slate_d"], 3.4)
    c.circle(10, 10, 8, fill=("r", hexc("#E0F2FE"), hexc("#7DD3FC")), outline=P["slate_d"], width=2)
    c.ellipse(5, 5, 5, 3.5, fill=hexc("#FFFFFF", 200), outline=None)


def eject(c):
    col = (W, hexc("#CBD5E1"))
    c.poly([(1.5, 25), (7.5, 18), (13.5, 25)], fill=col, outline=P["slate_d"], width=0.9)
    c.rect(1.5, 27, 12, 3, 0.8, fill=col, outline=P["slate_d"], width=0.9)


def export(c):
    c.poly([(19, 22), (25, 22), (25, 18.5), (31, 25), (25, 31.5), (25, 28), (19, 28)],
           fill=(hexc("#4ADE80"), P["green_d"]), outline=W, width=0.9)


def warning(c):
    c.shadow(lambda s: s.poly([(16, 2.5), (30.5, 28.5), (1.5, 28.5)], fill=P["amber"], outline=None))
    c.poly([(16, 2.5), (30.5, 28.5), (1.5, 28.5)], fill=(hexc("#FDE047"), hexc("#F59E0B")))
    c.line([(16, 10.5), (16, 19.5)], P["ink"], 3)
    c.circle(16, 24, 1.8, fill=P["ink"], outline=None)


# ============================================================ I/O board =====

def coin(c, label, metal=P["gold"]):
    c.shadow(lambda s: s.circle(16, 16, 13.5, fill=metal, outline=None))
    c.circle(16, 16, 13.5, fill=(shade(metal, 0.3), shade(metal, -0.35)))
    c.circle(16, 16, 10.3, fill=(shade(metal, -0.12), shade(metal, 0.25)), outline=hexc("#FFFFFF", 120), width=0.7)
    c.text(16, 16.4, label, 13 if len(label) == 1 else 9.5, shade(metal, -0.6), BOLD)


def note(c, label):
    body = hexc("#4ADE80")
    c.shadow(lambda s: s.rect(1.5, 7, 29, 18, 2, fill=body, outline=None))
    c.rect(1.5, 7, 29, 18, 2, fill=(hexc("#BBF7D0"), hexc("#16A34A")))
    c.rect(3.5, 9, 25, 14, 1.2, fill=None, outline=hexc("#14532D", 160), width=0.7)
    c.circle(16, 16, 5.2, fill=hexc("#F0FDF4"), outline=hexc("#14532D", 160), width=0.6)
    c.text(16, 16.3, label, 6.5 if len(label) < 2 else 5.2, hexc("#14532D"), BOLD)
    c.text(6.5, 12, label, 3.5, hexc("#14532D"), BOLD)
    c.text(25.5, 20, label, 3.5, hexc("#14532D"), BOLD)


def operator_setup(c):
    # wrench
    c.line([(7, 25), (19, 13)], hexc("#64748B"), 4)
    c.circle(21, 11, 6.5, fill=(hexc("#CBD5E1"), hexc("#64748B")))
    c.circle(23.4, 8.6, 2.6, fill=W, outline=None)
    c.circle(7, 25, 2.6, fill=(hexc("#CBD5E1"), hexc("#64748B")))
    # screwdriver
    c.line([(25, 25), (13, 13)], hexc("#94A3B8"), 1.5)
    c.line([(29, 29), (23.5, 23.5)], P["red"], 4)


def calibrate(c):
    col = P["red"]
    c.circle(16, 16, 11, fill=None, outline=hexc("#64748B"), width=1.6)
    c.circle(16, 16, 5.5, fill=None, outline=hexc("#64748B"), width=1.3)
    for (a, b) in (((16, 1.5), (16, 9)), ((16, 23), (16, 30.5)), ((1.5, 16), (9, 16)), ((23, 16), (30.5, 16))):
        c.line([a, b], hexc("#64748B"), 1.6)
    c.circle(16, 16, 2.4, fill=("r", hexc("#FCA5A5"), col), outline=None)


# ============================================================ helpers =======

def page(c, x=12, y=2, w=18, h=24):
    fold = 5
    c.shadow(lambda s: s.poly([(x, y), (x + w - fold, y), (x + w, y + fold), (x + w, y + h), (x, y + h)], fill=P["slate"], outline=None))
    c.poly([(x, y), (x + w - fold, y), (x + w, y + fold), (x + w, y + h), (x, y + h)], fill=(W, P["slate_xl"]))
    c.poly([(x + w - fold, y), (x + w - fold, y + fold), (x + w, y + fold)], fill=hexc("#CBD5E1"))
    for i in range(4):
        c.line([(x + 3, y + 9 + i * 3), (x + w - 3, y + 9 + i * 3)], hexc("#CBD5E1"), 0.8)


def folder(c, x=1.5, y=8, w=29, h=21):
    c.shadow(lambda s: s.rect(x, y, w, h, 2, fill=P["amber"], outline=None))
    c.poly([(x, y + 2), (x + 2, y), (x + 10, y), (x + 12, y + 2.5), (x + w, y + 2.5), (x + w, y + h), (x, y + h)],
           fill=(hexc("#FCD34D"), hexc("#D97706")))
    c.rect(x, y + 5, w, h - 5, 1.5, fill=(hexc("#FDE68A"), hexc("#F59E0B")))


def speaker_badge(c, muted):
    s = c.sub(15, 15, 0.52)
    s.circle(16, 16, 15, fill=W, outline=None)
    sound(s, waves=not muted)
    if muted:
        s.line([(19, 11), (29, 21)], P["red"], 2.6)
        s.line([(29, 11), (19, 21)], P["red"], 2.6)


# ====================================================== small variants =====
# Drawn for 16-24 px, where the full glyphs' lettering and fine lines blur.

def send_cad_small(c):
    keycap(c, 1, 3, 30, 26, "Del", 13, col=(hexc("#FCA5A5"), hexc("#DC2626")))


def send_cae_small(c):
    keycap(c, 1, 3, 30, 26, "Esc", 13, col=(hexc("#93C5FD"), hexc("#2563EB")))


def note_small(c, label):
    body = hexc("#4ADE80")
    c.rect(1, 5, 30, 22, 3, fill=(hexc("#BBF7D0"), hexc("#16A34A")), width=1.4)
    c.text(16, 16.3, label, 14 if len(label) < 2 else 12, hexc("#14532D"), BOLD)


def calibrate_small(c):
    c.circle(16, 16, 11.5, fill=None, outline=hexc("#64748B"), width=3)
    for (a, b) in (((16, 1), (16, 10)), ((16, 22), (16, 31)), ((1, 16), (10, 16)), ((22, 16), (31, 16))):
        c.line([a, b], hexc("#64748B"), 3)
    c.circle(16, 16, 4, fill=P["red"], outline=None)


def coin_small(c, label, metal=P["gold"]):
    c.circle(16, 16, 15, fill=(shade(metal, 0.3), shade(metal, -0.35)), width=1.4)
    c.text(16, 16.6, label, 20, shade(metal, -0.65), BOLD)


def take_raw_screenshot_small(c):
    camera(c)
    c.rect(19, 19, 12, 12, 2, fill=P["red"], outline=W, width=1.2)
    c.text(25, 25, "R", 9, W, BOLD)


def copy_screenshot_small(c):
    clipboard(c)
    camera(c.sub(9, 11, 0.7))
