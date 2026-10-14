"""vhd2raw.py IN.vhd OUT.raw: dynamic (or fixed) VHD to a sparse raw image."""
import struct, sys

f = open(sys.argv[1], 'rb')
f.seek(-512, 2)
foot = f.read(512)
assert foot[:8] == b'conectix'
size = struct.unpack('>Q', foot[48:56])[0]
dtype = struct.unpack('>I', foot[60:64])[0]
o = open(sys.argv[2], 'wb')
o.truncate(size)
if dtype == 2:
    f.seek(0)
    while True:
        b = f.read(1 << 20)
        if not b:
            break
        o.write(b)
    o.truncate(size)
    sys.exit(0)
data_off = struct.unpack('>Q', foot[16:24])[0]
f.seek(data_off)
hdr = f.read(1024)
assert hdr[:8] == b'cxsparse'
bat_off = struct.unpack('>Q', hdr[16:24])[0]
nbat = struct.unpack('>I', hdr[28:32])[0]
bsize = struct.unpack('>I', hdr[32:36])[0]
f.seek(bat_off)
bat = struct.unpack('>%dI' % nbat, f.read(nbat * 4))
bm = ((bsize // 512 + 7) // 8 + 511) // 512 * 512
for i, e in enumerate(bat):
    if e == 0xffffffff:
        continue
    f.seek(e * 512 + bm)
    o.seek(i * bsize)
    o.write(f.read(bsize))
o.close()
