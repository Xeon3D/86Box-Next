"""review.py -- pull CARDTEST results out of a VM and build a review page.

  review.py VMDIR [--key NAME] [--bless]
  review.py --dir RESULTS [--key NAME] [--bless]

VMDIR is an 86Box VM folder: its first hard disk (raw .img or a VHD,
FAT12/16/32 first partition, long file names understood) is read
directly -- shut the guest down (or at least wait for the run to finish)
first. With --dir, RESULTS is a CTRES folder copied out by hand (NTFS
guests).

Results land in cardtest/runs/<key>/<timestamp>/ with review.html:
per tool, every case's status, the card/reference/diff pictures, and
whether the card's picture or the INFO lines changed against the golden
copy in cardtest/golden/<key>/. --bless makes this run the golden copy
(only after a human looked at the page and agreed).
"""
import argparse, datetime, difflib, hashlib, html, os, re, shutil, struct

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


# ---- disk access ----------------------------------------------------------

class Disk:
    """Raw image or VHD (fixed or dynamic), read by byte offset."""

    def __init__(self, path):
        self.f = open(path, 'rb')
        self.f.seek(0, 2)
        size = self.f.tell()
        self.bat = None
        self.f.seek(size - 512)
        foot = self.f.read(512)
        if foot[:8] == b'conectix' and struct.unpack('>I', foot[60:64])[0] != 2:  # dynamic
            off = struct.unpack('>Q', foot[16:24])[0]
            self.f.seek(off)
            hdr = self.f.read(1024)
            bat_off = struct.unpack('>Q', hdr[16:24])[0]
            n = struct.unpack('>I', hdr[28:32])[0]
            self.bsize = struct.unpack('>I', hdr[32:36])[0]
            self.f.seek(bat_off)
            self.bat = struct.unpack('>%dI' % n, self.f.read(n * 4))
            self.bm = ((self.bsize // 512 + 7) // 8 + 511) // 512 * 512

    def read(self, off, n):
        if self.bat is None:
            self.f.seek(off)
            return self.f.read(n)
        out = bytearray()
        while n:
            blk, within = divmod(off, self.bsize)
            take = min(n, self.bsize - within)
            e = self.bat[blk] if blk < len(self.bat) else 0xffffffff
            if e == 0xffffffff:
                out += bytes(take)
            else:
                self.f.seek(e * 512 + self.bm + within)
                out += self.f.read(take)
            off += take
            n -= take
        return bytes(out)


class Fat:
    def __init__(self, disk):
        self.d = disk
        mbr = disk.read(0, 512)
        self.base = 0
        # a partition table (not a boot sector of an unpartitioned disk)
        if mbr[510:512] == b'\x55\xaa' and mbr[0x1be + 4] and struct.unpack('<H', mbr[11:13])[0] != 512:
            self.base = struct.unpack('<I', mbr[0x1c6:0x1ca])[0] * 512
        bs = disk.read(self.base, 512)
        if bs[3:7] == b'NTFS':
            raise SystemExit('NTFS volume: copy C:\\CTRES out of the guest and use --dir')
        self.bps, self.spc = struct.unpack('<H', bs[11:13])[0], bs[13]
        res, nfats = struct.unpack('<H', bs[14:16])[0], bs[16]
        self.rootents = struct.unpack('<H', bs[17:19])[0]
        tot = struct.unpack('<H', bs[19:21])[0] or struct.unpack('<I', bs[32:36])[0]
        fatsz = struct.unpack('<H', bs[22:24])[0] or struct.unpack('<I', bs[36:40])[0]
        self.fat_off = self.base + res * self.bps
        self.root_off = self.fat_off + nfats * fatsz * self.bps
        rootsecs = (self.rootents * 32 + self.bps - 1) // self.bps
        self.data_off = self.root_off + rootsecs * self.bps
        clusters = (tot - (res + nfats * fatsz + rootsecs)) // self.spc
        self.kind = 12 if clusters < 4085 else 16 if clusters < 65525 else 32
        self.root_clus = struct.unpack('<I', bs[44:48])[0] if self.kind == 32 else None
        self.csize = self.bps * self.spc

    def next(self, c):
        if self.kind == 32:
            return struct.unpack('<I', self.d.read(self.fat_off + c * 4, 4))[0] & 0x0fffffff
        if self.kind == 16:
            return struct.unpack('<H', self.d.read(self.fat_off + c * 2, 2))[0]
        v = struct.unpack('<H', self.d.read(self.fat_off + c * 3 // 2, 2))[0]
        return v >> 4 if c & 1 else v & 0xfff

    def chain(self, c):
        end = {12: 0xff8, 16: 0xfff8, 32: 0x0ffffff8}[self.kind]
        while 2 <= c < end:
            yield c
            c = self.next(c)

    def read_chain(self, c, size=None):
        data = b''.join(self.d.read(self.data_off + (x - 2) * self.csize, self.csize) for x in self.chain(c))
        return data if size is None else data[:size]

    def entries(self, clus):
        raw = self.d.read(self.root_off, self.rootents * 32) if clus is None else self.read_chain(clus)
        lfn = {}
        for i in range(0, len(raw), 32):
            e = raw[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xe5:
                lfn = {}
                continue
            if e[11] == 0x0f:  # long-name piece: 13 UTF-16 characters
                lfn[e[0] & 0x1f] = e[1:11] + e[14:26] + e[28:32]
                continue
            if e[11] & 8:
                lfn = {}
                continue
            name = e[0:8].decode('latin-1').rstrip()
            ext = e[8:11].decode('latin-1').rstrip()
            name = name + ('.' + ext if ext else '')
            if lfn:
                units = b''.join(lfn[k] for k in sorted(lfn))
                chars = []
                for j in range(0, len(units), 2):
                    u = units[j] | units[j + 1] << 8
                    if u in (0, 0xffff):
                        break
                    chars.append(chr(u))
                if chars:
                    name = ''.join(chars)
                lfn = {}
            c = struct.unpack('<H', e[26:28])[0] | (struct.unpack('<H', e[20:22])[0] << 16 if self.kind == 32 else 0)
            yield name, bool(e[11] & 0x10), c, struct.unpack('<I', e[28:32])[0]

    def find(self, path):
        clus = self.root_clus
        for part in path.upper().split('\\'):
            for name, isdir, c, size in self.entries(clus):
                if name.upper() == part:
                    clus = c
                    break
            else:
                return None
        return clus

    def copy_tree(self, clus, dest):
        os.makedirs(dest, exist_ok=True)
        for name, isdir, c, size in self.entries(clus):
            if name in ('.', '..'):
                continue
            if isdir:
                self.copy_tree(c, os.path.join(dest, name))
            else:
                with open(os.path.join(dest, name), 'wb') as f:
                    f.write(self.read_chain(c, size))


def vm_disk(vmdir):
    cfg = open(os.path.join(vmdir, '86box.cfg'), encoding='latin-1').read()
    m = re.search(r'^hdd_01_fn\s*=\s*(.+)$', cfg, re.M)
    if not m:
        raise SystemExit('no hdd_01_fn in 86box.cfg')
    p = m.group(1).strip()
    return p if os.path.isabs(p) else os.path.join(vmdir, p)


# ---- logs and the page ----------------------------------------------------

def parse(log):
    cases, info = [], []
    for line in open(log, encoding='latin-1').read().splitlines():
        if line.startswith('CASE  '):
            parts = line[6:].split(None, 2)
            cases.append((parts[0], parts[1] if len(parts) > 1 else '?', parts[2] if len(parts) > 2 else ''))
        elif line.startswith('INFO  '):
            info.append(line[6:])
    return cases, info


def md5(p):
    return hashlib.md5(open(p, 'rb').read()).hexdigest() if os.path.exists(p) else None


COLOURS = {'PASS': '#2e7d32', 'WARN': '#b26a00', 'FAIL': '#c62828', 'SKIP': '#666', 'VISUAL': '#1565c0'}


def page(run, golden, key):
    out = ['<!doctype html><meta charset="utf-8"><title>CARDTEST %s</title>' % html.escape(key),
           '<style>body{font:14px system-ui,sans-serif;margin:16px;background:#fafafa;color:#222}'
           'table{border-collapse:collapse;margin:8px 0 24px}td,th{border:1px solid #ccc;padding:4px 8px;vertical-align:top}'
           'img{image-rendering:pixelated;width:192px;border:1px solid #999}.st{color:#fff;padding:1px 6px;border-radius:3px}'
           '.chg{background:#fff3cd}pre{background:#fff;border:1px solid #ccc;padding:8px;overflow:auto;max-height:300px}</style>',
           '<h1>CARDTEST &mdash; %s</h1><p>%s</p>' % (html.escape(key), html.escape(run))]
    summary = []
    # RUNALL.BAT puts each colour depth in its own folder (8BPP, 16BPP, ...);
    # plain runs leave the logs in the root. Both are shown.
    groups = [''] + sorted((d for d in os.listdir(run) if re.match(r'^\d+BPP$', d, re.I)
                            and os.path.isdir(os.path.join(run, d))), key=lambda d: int(d[:-3]))
    texts = [t for t in ('SUMMARY.TXT', 'EXTRAS.LOG', 'DEPTH.LOG', 'MODES.TXT') if os.path.exists(os.path.join(run, t))]
    for t in texts:
        body = open(os.path.join(run, t), encoding='latin-1').read()
        out.append('<details%s><summary>%s</summary><pre>%s</pre></details>' % (
            ' open' if t == 'SUMMARY.TXT' else '', t, html.escape(body)))
    for sub in groups:
        here = os.path.join(run, sub) if sub else run
        gl_here = (os.path.join(golden, sub) if sub else golden) if golden else None
        for log in sorted(f for f in os.listdir(here) if f.upper().endswith('.LOG')
                          and f.upper() not in ('EXTRAS.LOG', 'DEPTH.LOG')):
            draw_tool(out, summary, run, sub, here, gl_here, log)
    # GUI benchmarks run by AUTOGUI (C:\CTRES\GUI) -- autogui session, see guiresults.py
    import guiresults
    guiresults.draw(out, summary, run)
    head =['<table><tr><th>tool</th>' + ''.join('<th>%s</th>' % s for s in COLOURS) + '</tr>']
    for tool, anchor, counts in summary:
        head.append('<tr><td><a href="#%s">%s</a></td>%s</tr>' % (
            anchor, tool, ''.join('<td>%s</td>' % (counts.get(s, '') or '') for s in COLOURS)))
    head.append('</table>')
    out[3:3] = head
    with open(os.path.join(run, 'review.html'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(out))
    return summary


def draw_tool(out, summary, run, sub, here, golden, log):
    tool = log[:-4].upper()
    key = (sub + ' ' if sub else '') + tool
    anchor = (sub + '-' if sub else '') + tool
    pre = sub + '/' if sub else ''
    cases, info = parse(os.path.join(here, log))
    gl = os.path.join(golden, log) if golden else None
    g_cases, g_info = parse(gl) if gl and os.path.exists(gl) else (None, None)
    counts = {}
    for c in cases:
        counts[c[1]] = counts.get(c[1], 0) + 1
    summary.append((key, anchor, counts))
    out.append('<h2 id="%s">%s</h2><table><tr><th>case</th><th>status</th><th>details</th>'
               '<th>card</th><th>reference</th><th>diff</th><th>vs golden</th></tr>' % (anchor, key))
    gstat = {c[0]: c[1] for c in g_cases} if g_cases else {}
    for name, st, det in cases:
        imgs = []
        for suf in 'SRD':
            p = '%s%s/%s_%s.BMP' % (pre, tool, name, suf)
            imgs.append('<a href="%s"><img src="%s"></a>' % (p, p) if os.path.exists(os.path.join(run, p)) else '')
        vs = ''
        if g_cases is not None:
            notes = []
            if name in gstat and gstat[name] != st:
                notes.append('was ' + gstat[name])
            if name not in gstat:
                notes.append('new case')
            a = md5(os.path.join(here, tool, name + '_S.BMP'))
            b = md5(os.path.join(golden, tool, name + '_S.BMP'))
            if a and b and a != b:
                notes.append('picture changed')
            vs = ', '.join(notes) or 'same'
        out.append('<tr%s><td>%s</td><td><span class="st" style="background:%s">%s</span></td><td>%s</td>'
                   '<td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>' % (
                       ' class="chg"' if vs not in ('', 'same') else '', name, COLOURS.get(st, '#444'), st,
                       html.escape(det), imgs[0], imgs[1], imgs[2], html.escape(vs)))
    out.append('</table>')
    if g_info is not None and g_info != info:
        d = '\n'.join(difflib.unified_diff(g_info, info, 'golden', 'this run', lineterm='', n=0))
        out.append('<p><b>INFO lines differ from golden:</b></p><pre>%s</pre>' % html.escape(d))
    out.append('<details><summary>INFO (%d lines)</summary><pre>%s</pre></details>' % (
        len(info), html.escape('\n'.join(info))))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('vmdir', nargs='?')
    ap.add_argument('--dir')
    ap.add_argument('--key')
    ap.add_argument('--bless', action='store_true')
    a = ap.parse_args()
    if not a.vmdir and not a.dir:
        ap.error('VMDIR or --dir')
    key = a.key or os.path.basename(os.path.normpath(a.vmdir or a.dir))
    run = os.path.join(ROOT, 'runs', key, datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    if a.dir:
        shutil.copytree(a.dir, run)
    else:
        try:
            fat = Fat(Disk(vm_disk(a.vmdir)))
        except SystemExit:  # NTFS guest (NT 4 / 2000): ntfs-3g in WSL -- autogui session
            import ntfsread
            ntfsread.copy_tree(vm_disk(a.vmdir), 'CTRES', run)
            fat = None
        if fat:
            c = fat.find('CTRES')
            if c is None:
                raise SystemExit('no C:\\CTRES on the disk')
            fat.copy_tree(c, run)
    golden = os.path.join(ROOT, 'golden', key)
    summary = page(run, golden if os.path.isdir(golden) else None, key)
    for tool, anchor, counts in summary:
        print('%-14s %s' % (tool, ' '.join('%s=%d' % kv for kv in sorted(counts.items()))))
    extras = os.path.join(run, 'EXTRAS.LOG')
    if os.path.exists(extras):
        print(open(extras, encoding='latin-1').read().strip())
    print(os.path.join(run, 'review.html'))
    if a.bless:
        if os.path.isdir(golden):
            shutil.rmtree(golden)
        shutil.copytree(run, golden)
        print('golden updated:', golden)


if __name__ == '__main__':
    main()
