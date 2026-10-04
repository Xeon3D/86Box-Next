"""
Regenerate the 86Box-Next icon set.

    python tools/icons/make_icons.py [--preview out.png] [--only name,name]

Writes src/qt/icons/*.ico, the application icon in its Windows, macOS and
Linux forms, and the VM Manager wizard artwork.  Everything is drawn by
glyphs.py; nothing is hand-edited, so change a glyph and rerun.
"""
import argparse
import os
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(__file__))
import glyphs as g                                   # noqa: E402
from iconkit import Canvas, P, hexc, render, save_ico, save_png, disabled, downsample  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ICONS = os.path.join(ROOT, "src", "qt", "icons")

UI_SIZES = [16, 20, 24, 32, 40, 48, 64]
APP_SIZES = [16, 20, 24, 32, 40, 48, 64, 72, 128, 256]


# ------------------------------------------------------------ composites ---
def image_of(dev):
    """A disk image file: the medium in front of a document."""
    def draw(c):
        g.page(c, 12, 2, 18, 24)
        dev(c.sub(0, 9, 0.72))
    return draw


def disabled_of(dev):
    def draw(c):
        c.img.alpha_composite(disabled(render(dev)))
        g.disabled_badge(c)
    return draw


def with_gear(dev):
    def draw(c):
        dev(c.sub(0, 0, 0.86))
        g.gear_badge(c)
    return draw


def folder_of(disc):
    def draw(c):
        disc(c.sub(0, 0, 0.78))
        g.folder(c.sub(13, 14, 0.56))
    return draw


def host_of(disc):
    def draw(c):
        disc(c.sub(0, 0, 0.78))
        g.monitor(c.sub(13, 14, 0.58))
    return draw


def speaker_of(disc, muted):
    def draw(c):
        disc(c.sub(0, 0, 0.86))
        g.speaker_badge(c, muted)
    return draw


def pair(a, b):
    """Two devices, one behind the other (the settings tabs)."""
    def draw(c):
        a(c.sub(0, 0, 0.7))
        b(c.sub(9.5, 9.5, 0.7))
    return draw


def lock(label, on, **kw):
    return lambda c: g.lock_key(c, label, on, **kw)


def scsi(c):
    g.expansion_card(c, pcb=hexc("#2563EB"))
    c.rect(5, 9, 9, 8, 1, fill=(hexc("#F8FAFC"), hexc("#CBD5E1")), width=0.5)
    c.text(9.5, 13.2, "SCSI", 2.9, P["blue_d"], g.BOLD)


def app(col):
    return lambda c: g.app_mark(c, col)


APP_COLORS = {
    "green":  P["green"],
    "yellow": P["amber"],
    "red":    P["red"],
    "gray":   hexc("#64748B"),
}

ICON_SET = {
    # app / VM state
    **{f"86Box-{k}": app(v) for k, v in APP_COLORS.items()},
    "emulator": g.emulator,
    "new_vm": g.new_vm,

    # actions
    "run": g.run, "pause": g.pause, "fast_forward": g.fast_forward, "rewind": g.rewind,
    "record": g.record, "hard_reset": g.hard_reset, "acpi_shutdown": g.acpi_shutdown,
    "send_cad": g.send_cad, "send_cae": g.send_cae, "settings": g.settings,
    "take_screenshot": g.take_screenshot, "take_raw_screenshot": g.take_raw_screenshot,
    "copy_screenshot": g.copy_screenshot, "copy_raw_screenshot": g.copy_raw_screenshot,
    "interpreter": g.interpreter, "recompiler": g.recompiler,
    "warning": g.warning,

    # overlays (drawn over another icon at 0,0)
    "active": g.active, "write_active": g.write_active, "disabled": g.disabled_badge,
    "write_protected": g.write_protected, "new": g.new, "browse": g.browse,
    "eject": g.eject, "export": g.export,

    # keyboard lock indicators
    "caps_lock_off": lock("A", False), "caps_lock_on": lock("A", True),
    "num_lock_off": lock("1", False), "num_lock_on": lock("1", True),
    "kana_lock_off": lock("カ", False, face=g.JP, size=11), "kana_lock_on": lock("カ", True, face=g.JP, size=11),
    "scroll_lock_off": lambda c: g.lock_arrow(c, False), "scroll_lock_on": lambda c: g.lock_arrow(c, True),

    # media
    "floppy_35": g.floppy_35, "floppy_525": g.floppy_525,
    "floppy_35_image": image_of(g.floppy_35), "floppy_525_image": image_of(g.floppy_525),
    "floppy_disabled": disabled_of(g.floppy_35),
    "cdrom": g.cdrom, "cdrom_image": image_of(g.cdrom), "cdrom_disabled": disabled_of(g.cdrom),
    "cdrom_folder": folder_of(g.cdrom), "cdrom_host": host_of(g.cdrom),
    "cdrom_mute": speaker_of(g.cdrom, True), "cdrom_unmute": speaker_of(g.cdrom, False),
    "dvdrom": g.dvdrom, "dvdrom_image": image_of(g.dvdrom), "dvdrom_folder": folder_of(g.dvdrom),
    "dvdrom_host": host_of(g.dvdrom), "dvdrom_unmute": speaker_of(g.dvdrom, False),
    "hard_disk": g.hard_disk,
    "zip": g.zip_disk, "zip_image": image_of(g.zip_disk),
    "jaz": g.jaz, "jaz_image": image_of(g.jaz),
    "syquest": g.syquest, "syquest_image": image_of(g.syquest),
    "rdisk": g.rdisk, "rdisk_image": image_of(g.rdisk), "rdisk_disabled": disabled_of(g.rdisk),
    "mo": g.mo, "mo_image": image_of(g.mo), "mo_disabled": disabled_of(g.mo),
    "tape": g.tape, "tape_image": image_of(g.tape), "tape_disabled": disabled_of(g.tape),
    "cassette": g.cassette, "cassette_image": image_of(g.cassette),
    "cartridge": g.cartridge, "cartridge_image": image_of(g.cartridge),

    # settings pages
    "machine": g.machine, "machine_tab": with_gear(g.machine),
    "processor": g.processor, "display": g.display, "general_display": with_gear(g.display),
    "monitor": with_gear(g.monitor),
    "input_devices": g.input_devices, "key_bindings": g.key_bindings,
    "sound": g.sound, "general_sound": with_gear(g.sound), "midi": g.midi,
    "network": g.network, "ports": g.ports, "serial_ports": g.serial_ports, "parallel_ports": g.parallel_ports,
    "storage_controllers": g.storage_controllers, "general_storage_controllers": with_gear(g.storage_controllers),
    "scsi_controllers": scsi,
    "floppy_and_cdrom_drives": pair(g.floppy_35, g.cdrom), "floppy_tab": pair(g.floppy_525, g.floppy_35),
    "other_removable_devices": pair(g.zip_disk, g.mo), "rdisk_tab": pair(g.rdisk, g.zip_disk),
    "other_peripherals": g.other_peripherals, "general_other_peripherals": with_gear(g.other_peripherals),
    "pcmcia": g.pcmcia,
    "isa_memory": g.isa_memory, "isa_rom": g.isa_rom, "performance": g.performance,

    # arcade I/O boards
    **{f"coin_{i}": (lambda c, i=i: g.coin(c, str(i))) for i in range(1, 6)},
    "coin_6": lambda c: g.coin(c, "T", P["silver"]),
    "note_1": lambda c: g.note(c, "5"), "note_2": lambda c: g.note(c, "10"),
    "note_3": lambda c: g.note(c, "20"), "note_4": lambda c: g.note(c, "50"),
    "operator_setup": g.operator_setup, "calibrate": g.calibrate,
}


# Simplified renderings for 16-24 px.
SMALL = {
    "send_cad": g.send_cad_small, "send_cae": g.send_cae_small,
    "calibrate": g.calibrate_small,
    "take_raw_screenshot": g.take_raw_screenshot_small,
    "copy_screenshot": g.copy_screenshot_small, "copy_raw_screenshot": g.copy_screenshot_small,
    **{f"coin_{i}": (lambda c, i=i: g.coin_small(c, str(i))) for i in range(1, 6)},
    "coin_6": lambda c: g.coin_small(c, "T", P["silver"]),
    "note_1": lambda c: g.note_small(c, "5"), "note_2": lambda c: g.note_small(c, "10"),
    "note_3": lambda c: g.note_small(c, "20"), "note_4": lambda c: g.note_small(c, "50"),
}


# ------------------------------------------------------------- app icons ---
def write_app_icons(art):
    """The application icon: Windows .ico (already in ICON_SET), macOS .icns
    and the Linux hicolor PNGs, all from the green (release) mark."""
    mark = art["86Box-green"]
    for kind, col in (("release", "green"), ("beta", "yellow"), ("dev", "red"), ("branch", "gray")):
        d = os.path.join(ROOT, "src", "mac", "icons", kind)
        if os.path.isdir(d):
            big = downsample(art[f"86Box-{col}"], 1024)
            big.save(os.path.join(d, "86Box.icns"), sizes=[(16, 16), (32, 32), (64, 64), (128, 128), (256, 256), (512, 512), (1024, 1024)])
    for px in (16, 20, 24, 32, 40, 48, 64, 72, 128, 256):
        d = os.path.join(ROOT, "src", "unix", "assets", f"{px}x{px}")
        if os.path.isdir(d):
            save_png(mark, os.path.join(d, "net.86box.86Box.png"), px)


def write_wizard_art(art):
    """VM Manager artwork: the Add Machine wizard logo and watermark, and the
    first-run wizard picture, at their existing sizes."""
    assets = os.path.join(ROOT, "src", "qt", "assets")
    mark = art["86Box-green"]
    for name in ("addvm-logo.png",):
        p = os.path.join(assets, name)
        if not os.path.exists(p):
            continue
        w, h = Image.open(p).size
        out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        s = min(w, h)
        icon = downsample(mark, s)
        out.alpha_composite(icon, ((w - s) // 2, (h - s) // 2))
        out.save(p)
    for name in ("addvm-watermark.png", "86box-wizard.png"):
        p = os.path.join(assets, name)
        if not os.path.exists(p):
            continue
        w, h = Image.open(p).size
        out = Image.new("RGBA", (w, h))
        top, bot = hexc("#15803D"), hexc("#0F172A")
        d = ImageDraw.Draw(out)
        for y in range(h):
            t = y / max(1, h - 1)
            d.line([(0, y), (w, y)], fill=tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(4)))
        s = int(w * 0.8)
        icon = downsample(mark, s)
        out.alpha_composite(icon, ((w - s) // 2, int(h * 0.08)))
        out.save(p)


# --------------------------------------------------------------- preview ---
def preview(art, path):
    names = sorted(art)
    cols = 10
    cell = 120
    rows = (len(names) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * cell, rows * cell * 2 // 1), "white")
    d = ImageDraw.Draw(sheet)
    half = rows * cell
    d.rectangle([0, half, cols * cell, 2 * half], fill=(32, 33, 36))
    for i, n in enumerate(names):
        x, y = (i % cols) * cell, (i // cols) * cell
        for off, txt in ((0, "black"), (half, "white")):
            im32 = downsample(art[n], 32)
            im16 = downsample(render(SMALL[n]) if n in SMALL else art[n], 16)
            im64 = downsample(art[n], 64)
            sheet.paste(im64, (x + 6, y + off + 8), im64)
            sheet.paste(im32, (x + 76, y + off + 8), im32)
            sheet.paste(im16, (x + 84, y + off + 50), im16)
            d.text((x + 4, y + off + 100), n[:20], fill=txt)
    sheet.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview")
    ap.add_argument("--only")
    ap.add_argument("--no-write", action="store_true")
    a = ap.parse_args()

    names = a.only.split(",") if a.only else list(ICON_SET)
    art = {n: render(ICON_SET[n]) for n in names}

    if not a.no_write:
        for n, img in art.items():
            sizes = APP_SIZES if n.startswith("86Box-") else UI_SIZES
            small = render(SMALL[n]) if n in SMALL else None
            save_ico(img, os.path.join(ICONS, n + ".ico"), sizes, small)
        if not a.only:
            write_app_icons(art)
            write_wizard_art(art)
    if a.preview:
        preview(art, a.preview)
    print(f"{len(art)} icons")


if __name__ == "__main__":
    main()
