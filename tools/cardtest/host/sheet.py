"""sheet.py RUN TOOL CASE... OUT.png -- card | reference | diff rows, for a quick look."""
import os, sys
from PIL import Image, ImageDraw
run, tool, *names, out = sys.argv[1:]
rows = []
for n in names:
    ims = []
    for s in 'SRD':
        p = os.path.join(run, tool, '%s_%s.BMP' % (n, s))
        ims.append(Image.open(p).convert('RGB') if os.path.exists(p) else Image.new('RGB', (64, 64), 'gray'))
    rows.append((n, ims))
w = max(sum(i.width + 8 for i in ims) for n, ims in rows)
h = sum(max(i.height for i in ims) + 18 for n, ims in rows)
S = Image.new('RGB', (w, h), 'white')
d = ImageDraw.Draw(S)
y = 0
for n, ims in rows:
    d.text((2, y + 2), n, fill='black')
    x = 0
    for im in ims:
        S.paste(im, (x, y + 14))
        x += im.width + 8
    y += max(i.height for i in ims) + 18
S.save(out)
