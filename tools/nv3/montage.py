"""montage.py OUT.png COLS SCALE shot1.bmp shot2.bmp ...

Tiles nvtest.sh screenshots (32-bit top-down BMPs) into one PNG, each scaled down
by SCALE (2 = half size), COLS per row. No dependencies beyond the standard library.
"""
import struct
import sys
import zlib


def load(path):
    d = open(path, 'rb').read()
    w, h = struct.unpack('<ii', d[18:26])
    return w, -h, d[54:]


def main():
    out, cols, scale = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    imgs = [load(p) for p in sys.argv[4:]]
    tw = max(w for w, _, _ in imgs) // scale + 4
    th = max(h for _, h, _ in imgs) // scale + 4
    rows = (len(imgs) + cols - 1) // cols
    W, H = tw * cols, th * rows
    buf = bytearray(W * H * 3)
    for k, (w, h, px) in enumerate(imgs):
        ox, oy = (k % cols) * tw, (k // cols) * th
        for y in range(0, h - scale + 1, scale):
            for x in range(0, w - scale + 1, scale):
                o = (y * w + x) * 4
                t = ((oy + y // scale) * W + ox + x // scale) * 3
                buf[t:t + 3] = bytes((px[o + 2], px[o + 1], px[o]))
    raw = b''.join(b'\0' + bytes(buf[y * W * 3:(y + 1) * W * 3]) for y in range(H))

    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))

    open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
                          + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


main()
