#!/usr/bin/env python3
"""Regenerate src/machine/machine_bios_vendor.c: which BIOS each machine runs.

The vendor is read from the ROM images themselves (copyright and sign-on
strings), for every BIOS a machine can select in Configure; OVERRIDES covers
the ROMs whose strings say nothing a pattern can catch (in-house BIOSes,
compressed images) or mislead (Cirrus Logic VGA BIOSes carry a Quadtel
copyright, clones say "not copr. IBM").

    tools/machine-bios-vendors.py <directory holding roms/>

Run it after an upstream sync adds machines; it lists any machine it could
not place, which then needs an OVERRIDES entry.
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")
OUT = os.path.join(SRC, "machine", "machine_bios_vendor.c")

# internal name -> vendor, for a machine with a single BIOS; None = not known.
OVERRIDES = {
    "americxt": "American C&P", "amixt": "AMI", "bw230": "Bondwell",
    "mpc1600": "Columbia Data Products", "pcspirit": "Eagle",
    "genxt": "Generic Turbo XT", "glabios": "GLaBIOS",
    "super16t": "Falcon Technology", "super16te": "Patterson Labs",
    "pc4i": "NCR", "pc8": "NCR", "pc916sx": "NCR", "3302": "NCR",
    "m19": "Olivetti", "m24": "Olivetti", "m240": "Olivetti", "m290": "Olivetti",
    "openxt": "Turbo XT BIOS", "pcxt": "Turbo XT BIOS",
    "p3105": "Philips", "p3120": "Philips", "p3345": "Philips",
    "pravetz16": "Pravetz", "mxl7t": "Micoms", "pravetz16s": None,
    "sansx16": "Sanyo", "mbc17": "Sanyo", "europc": "Schneider",
    "to16": "Thomson", "xi8088": "Xi8088 BIOS", "znic": None,
    "zdsz151": "Zenith", "zdsz159": "Zenith", "zdsupers": "Zenith",
    "v20xt": "c't V20-BIOS",
    "pc1512": "Amstrad", "pc1640": "Amstrad", "pc2086": "Amstrad",
    "pc3086": "Amstrad", "pc200": "Amstrad", "ppc512": "Amstrad",
    "pc5086": "Chips and Technologies", "pc5286": "Chips and Technologies",
    "gw286ct": "Chips and Technologies",
    "elt": "Epson", "maz1016": "IMM", "iskra3104": None,
    "lxt3": "Central Point Software",
    "ibmps1es": "IBM", "ibmps1_2121": "IBM", "cobalt": "IBM",
    "cmdpc30": "Commodore",
    "ft286": "AMI", "ftbaby286": "AMI", "at122": "AMI",
    "pb286": "ERSO",
    "siemens": "Tandon", "micronics386": "Tandon", "c747": "Tandon",
    "tuliptc7": "Tulip", "gdc212m": "GoldStar",
    "ibmatquadtel": "Quadtel", "quadt286": "Quadtel", "quadt386sx": "Quadtel",
    "drsm35286": "Quadtel", "megapc": "Quadtel",
    "acer100t": "Acer", "acera1g": "Acer", "acerv10": "Acer", "acerp3": "Acer",
    "acerv30": "Acer", "acerm1": "Acer", "acerm3a": "Acer", "acerv35n": "Acer",
    "acerv60n": "Acer",
    "dtk386": "DTK", "dtk461": "DTK",
    "svc386sxp1": "Silicon Valley Computer", "svc486wb": "Silicon Valley Computer",
    "mvi486": "Mylex", "xenon": "Normerel",
    # Intel boards: compressed .BIO images, AMI cores (P-prefixed: Phoenix).
    "ninja": "AMI", "revenge": "AMI", "pb520r": "AMI", "morrison32": "AMI",
    "pc330_65x6": "AMI", "pb570": "AMI", "holly": "AMI", "atlantis": "AMI",
    "endeavor": "AMI", "pb640": "AMI", "bl440zx": "Phoenix",
    "optiplexgn": "Phoenix", "optiplexgxa": "Phoenix",
    "cobalt3k_carmel": "Cobalt", "cobalt3k_pacifica": "Cobalt",
}

# Selectable BIOS internal name -> vendor, where neither name nor ROM tells.
VARIANT_OVERRIDES = {
    "jukost": "Juko", "laserxt_108": "Central Point Software",
    "laserxt": "Central Point Software",
}

# Checked in order: an Award ROM may mention Phoenix, a Phoenix one Quadtel.
SIGS = [
    ("Award", [b"award software", b"awardbios", b"award modular", b"award bootblock",
               b"award decompression", b"awardsoft"]),
    ("AMI", [b"american megatrends", b"amibios"]),
    ("MR BIOS", [b"microid research", b"mr bios"]),
    ("Phoenix", [b"phoenix technologies", b"phoenixbios", b"phoenix rom bios",
                 b"phoenix software"]),
    ("Compaq", [b"compaq computer"]),
    ("DTK", [b"datatech enterprises"]),
    ("Chips and Technologies", [b"chips and technologies", b"chips & technologies"]),
    ("IBM", [b"copr. ibm", b"ibm corp"]),
]

# Selectable BIOS names that already say whose they are.
NAME_SIGS = [
    (r"Award", "Award"), (r"\bAMI", "AMI"), (r"MR BIOS", "MR BIOS"),
    (r"Phoenix", "Phoenix"), (r"GLaBIOS", "GLaBIOS"), (r"DTK ERSO", "DTK/ERSO"),
    (r"Multitech", "Multitech"), (r"Acer BIOS", "Acer"), (r"IBM BIOS", "IBM"),
]


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def braced(text, pattern):
    m = re.search(pattern, text, re.M)
    if not m:
        return ""
    i, depth = m.end(), 1
    while depth and i < len(text):
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        i += 1
    return text[m.start():i]


MACHINE_SRC = "\n".join(read(f) for f in glob.glob(os.path.join(SRC, "machine", "*.c")))
ALL_SRC = MACHINE_SRC + "\n".join(read(f) for f in glob.glob(os.path.join(SRC, "**", "*.c"), recursive=True)
                                  if os.sep + "machine" + os.sep not in f)
ROMRE = re.compile(r'"(roms/machines/[^"]+)"')


def func_roms(name, depth=0):
    body = braced(MACHINE_SRC, r"^(?:[\w \*]*?\b)?" + re.escape(name)
                  + r"\s*\([^;{]*\)\s*(?:/\*.*?\*/|//[^\n]*)?\s*\{")
    files = ROMRE.findall(body)
    if depth < 2:
        for call in set(re.findall(r"\b(machine_\w+)\s*\(", body)):
            if call != name and not call.startswith("machine_get") and (call.endswith("_init") or "common" in call):
                files += func_roms(call, depth + 1)
    return files


def bios_variants(device):
    dev = braced(ALL_SRC, r"device_t\s+" + re.escape(device) + r"\s*=\s*\{")
    cm = re.search(r"\.config\s*=\s*(\w+)", dev)
    if not cm:
        return None, []
    cfg = braced(ALL_SRC, r"device_config_t\s+" + re.escape(cm.group(1)) + r"\s*\[\s*\]\s*=\s*\{")
    bm = re.search(r'\.name\s*=\s*"bios"', cfg)
    if not bm:
        return None, []
    sub = cfg[bm.start():]
    default = re.search(r'\.default_string\s*=\s*"([^"]*)"', sub)
    arr = braced(sub, r"\.bios\s*=\s*\{")
    variants = [(m.group(1), m.group(2), ROMRE.findall(m.group(3)))
                for m in re.finditer(r'\{\s*\.name\s*=\s*"([^"]*)"\s*,\s*\.internal_name\s*=\s*"([^"]*)"'
                                     r'.*?\.files\s*=\s*\{([^}]*)\}', arr, re.S)]
    return (default.group(1) if default else None), variants


def rom_vendor(roms, files, machine_name):
    datas = []
    for f in files:
        p = os.path.join(roms, f)
        if os.path.isfile(p):
            with open(p, "rb") as fh:
                datas.append(fh.read())
    if len(datas) >= 2:  # even/odd halves split every string
        datas += [bytes(b for p in zip(datas[0], datas[1]) for b in p),
                  bytes(b for p in zip(datas[1], datas[0]) for b in p)]
    low = b"".join(d.lower() for d in datas)
    for vendor, pats in SIGS:
        if any(p in low for p in pats):
            if vendor == "IBM" and "IBM" not in machine_name:
                return None  # clones disclaim IBM's copyright
            return vendor
    return None


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    roms = sys.argv[1]
    table = read(os.path.join(SRC, "machine", "machine_table.c"))
    rows, unplaced = [], []
    for m in re.finditer(r'\.name\s*=\s*"([^"]*)",\s*\n\s*\.internal_name\s*=\s*"([^"]+)"', table):
        name, iname = m.group(1), m.group(2)
        nxt = table.find(".internal_name", m.end())
        chunk = table[m.end(): nxt if nxt > 0 else len(table)]
        init = re.search(r"\.init\s*=\s*(\w+)", chunk)
        dev = re.search(r"\n\s*\.device\s*=\s*&(\w+)", chunk)
        default, variants = bios_variants(dev.group(1)) if dev else (None, [])
        variants = [v for v in variants if not v[1].startswith("diag_")]
        vendors = []
        if variants:
            variants.sort(key=lambda v: v[1] != default)
            for vname, viname, files in variants:
                vendor = VARIANT_OVERRIDES.get(viname)
                if vendor is None:
                    vendor = next((vd for pat, vd in NAME_SIGS if re.search(pat, vname)), None)
                if vendor is None:
                    vendor = OVERRIDES.get(iname) or rom_vendor(roms, files, name)
                if vendor and vendor not in vendors:
                    vendors.append(vendor)
        elif iname in OVERRIDES:
            vendors = [OVERRIDES[iname]] if OVERRIDES[iname] else []
        else:
            vendor = rom_vendor(roms, func_roms(init.group(1)) if init else [], name)
            vendors = [vendor] if vendor else []
        if vendors:
            rows.append((iname, vendors))
        elif iname not in OVERRIDES:
            unplaced.append(f"{iname} ({name})")

    out = [
        "/*",
        " * 86Box    A hypervisor and IBM PC system emulator that specializes in",
        " *          running old operating systems and software designed for IBM",
        " *          PC systems and compatibles from 1981 through fairly recent",
        " *          system designs based on the PCI bus.",
        " *",
        " *          This file is part of the 86Box distribution.",
        " *",
        " *          Which BIOS each machine runs (Settings > Machine).",
        " *",
        " *          Generated by tools/machine-bios-vendors.py from the ROM",
        " *          images; regenerate it rather than editing it by hand.",
        " */",
        "#include <stdint.h>",
        "#include <string.h>",
        "#include <86box/86box.h>",
        "#include <86box/machine.h>",
        "",
        "typedef struct machine_bios_vendor_t {",
        "    const char *internal_name;",
        "    /* The default BIOS's vendor first, then the other selectable ones. */",
        "    const char *vendors[4];",
        "} machine_bios_vendor_t;",
        "",
        "static const machine_bios_vendor_t machine_bios_vendors[] = {",
    ]
    for iname, vendors in rows:
        out.append("    { %-22s { %s } }," % ('"%s",' % iname, ", ".join('"%s"' % v for v in vendors[:4])))
    out += [
        "};",
        "",
        "/* The idx-th vendor of the BIOSes machine m can run, NULL past the last or",
        "   when it is not known. */",
        "const char *",
        "machine_get_bios_vendor(int m, int idx)",
        "{",
        "    const char *iname = machine_get_internal_name_ex(m);",
        "",
        "    if ((iname == NULL) || (idx < 0) || (idx >= 4))",
        "        return NULL;",
        "",
        "    for (size_t i = 0; i < (sizeof(machine_bios_vendors) / sizeof(machine_bios_vendors[0])); i++) {",
        "        if (!strcmp(machine_bios_vendors[i].internal_name, iname))",
        "            return machine_bios_vendors[i].vendors[idx];",
        "    }",
        "",
        "    return NULL;",
        "}",
        "",
    ]
    with open(OUT, "w", newline="\n") as f:
        f.write("\n".join(out))
    print(f"{len(rows)} machines written to {os.path.relpath(OUT, ROOT)}")
    for u in unplaced:
        print("not placed:", u)


if __name__ == "__main__":
    main()
