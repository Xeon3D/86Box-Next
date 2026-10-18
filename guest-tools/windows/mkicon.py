#!/usr/bin/env python3
"""Make gtagent.ico: the 86Box icon as classic BMP icon images (4, 8 and
32 bits per pixel), which Windows 95 and NT 4.0 can read -- the emulator's
own .ico files hold PNG images only.

    mkicon.py <86Box .ico with PNG images> <out .ico> [--win16]

--win16: one 32x32 16-colour image, all Windows 3.x reads.
"""
import io
import struct
import sys

from PIL import Image

VGA16 = [
    (0, 0, 0), (128, 0, 0), (0, 128, 0), (128, 128, 0),
    (0, 0, 128), (128, 0, 128), (0, 128, 128), (192, 192, 192),
    (128, 128, 128), (255, 0, 0), (0, 255, 0), (255, 255, 0),
    (0, 0, 255), (255, 0, 255), (0, 255, 255), (255, 255, 255),
]


def source_images(path):
    data = open(path, 'rb').read()
    count = struct.unpack_from('<HHH', data)[2]
    images = {}
    for i in range(count):
        w, h, _, _, _, _, size, off = struct.unpack_from('<BBBBHHII', data, 6 + 16 * i)
        img = Image.open(io.BytesIO(data[off:off + size])).convert('RGBA')
        images[img.width] = img
    return images


def row_bytes(bits):
    return ((bits + 31) // 32) * 4


def and_mask(img):
    w, h = img.size
    stride = row_bytes(w)
    out = bytearray()
    for y in range(h - 1, -1, -1):
        row = bytearray(stride)
        for x in range(w):
            if img.getpixel((x, y))[3] < 128:
                row[x // 8] |= 0x80 >> (x % 8)
        out += row
    return bytes(out)


def bmp_image(img, bpp):
    w, h = img.size
    header = struct.pack('<IiiHHIIiiII', 40, w, h * 2, 1, bpp, 0, 0, 0, 0, 0, 0)
    if bpp == 32:
        pixels = bytearray()
        for y in range(h - 1, -1, -1):
            for x in range(w):
                r, g, b, a = img.getpixel((x, y))
                pixels += bytes((b, g, r, a) if a else (0, 0, 0, 0))
        return header + bytes(pixels) + and_mask(img)

    if bpp == 4:
        palette = VGA16
    else:
        palette = None
    rgb = Image.new('RGB', img.size, (0, 0, 0))
    rgb.paste(img, mask=img.split()[3])
    if palette is not None:
        pal_img = Image.new('P', (1, 1))
        flat = [c for rgb_ in palette for c in rgb_]
        pal_img.putpalette(flat + [0] * (768 - len(flat)))
        q = rgb.quantize(palette=pal_img, dither=Image.Dither.NONE)
    else:
        q = rgb.quantize(colors=256, dither=Image.Dither.NONE)
        flat = q.getpalette()[:768]
        flat += [0] * (768 - len(flat))
        palette = [tuple(flat[i:i + 3]) for i in range(0, 768, 3)]
    ncolors = 1 << bpp
    pal = b''.join(struct.pack('BBBB', b, g, r, 0) for (r, g, b) in palette[:ncolors])
    pal += b'\0' * (4 * ncolors - len(pal))
    stride = row_bytes(w * bpp)
    pixels = bytearray()
    for y in range(h - 1, -1, -1):
        row = bytearray(stride)
        for x in range(w):
            idx = q.getpixel((x, y)) if img.getpixel((x, y))[3] >= 128 else 0
            if bpp == 8:
                row[x] = idx
            else:
                row[x // 2] |= (idx & 15) << (4 if x % 2 == 0 else 0)
        pixels += row
    return header + pal + bytes(pixels) + and_mask(img)


def main():
    src = source_images(sys.argv[1])
    entries = []
    formats = ((32, 4), (16, 4), (32, 8), (16, 8), (48, 32), (32, 32), (16, 32))
    if '--win16' in sys.argv[3:]:
        formats = ((32, 4),)
    for size, bpp in formats:
        img = src[size] if size in src else src[max(src)].resize((size, size), Image.LANCZOS)
        entries.append((size, bpp, bmp_image(img, bpp)))
    out = bytearray(struct.pack('<HHH', 0, 1, len(entries)))
    offset = 6 + 16 * len(entries)
    for size, bpp, data in entries:
        colors = (1 << bpp) if bpp < 8 else 0
        out += struct.pack('<BBBBHHII', size, size, colors, 0, 1, bpp, len(data), offset)
        offset += len(data)
    for _, _, data in entries:
        out += data
    open(sys.argv[2], 'wb').write(out)


if __name__ == '__main__':
    main()
