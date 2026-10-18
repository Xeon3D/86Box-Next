"""guiresults.py -- the GUI-benchmark (AUTOGUI) part of review.py's page.

C:\\CTRES\\GUI holds one <NAME>.LOG per AUTOGUI script plus <NAME>\\*.BMP
screenshots, and exported result files (ZDWB\\*.TXT/CSV, SPECVP\\*). For each
script this shows its CASE / RESULT / ERROR / MANUAL lines, the dialogs it
dismissed, its exported results, and OCR text of score screenshots (shot
names containing "score", "result" or "res"), using tesseract in WSL; the
OCR text is cached next to the picture as <shot>.ocr.txt.
"""
import html, os, re, subprocess

STATUS = {'PASS': '#2e7d32', 'WARN': '#b26a00', 'FAIL': '#c62828', 'SKIP': '#666', 'MANUAL': '#1565c0'}


def wslpath(p):
    p = os.path.abspath(p).replace('\\', '/')
    return '/mnt/' + p[0].lower() + p[2:]


def ocr(bmp):
    cache = bmp[:-4] + '.ocr.txt'
    if os.path.exists(cache):
        return open(cache, encoding='utf-8', errors='replace').read()
    try:
        env = dict(os.environ, MSYS_NO_PATHCONV='1')
        r = subprocess.run(['wsl', '-e', 'tesseract', wslpath(bmp), '-', '--psm', '6'], capture_output=True,
                           timeout=120, env=env)
        text = r.stdout.decode('utf-8', 'replace').strip()
    except Exception as e:  # no WSL / no tesseract: say so, don't fail the page
        text = '(OCR unavailable: %s)' % e
    with open(cache, 'w', encoding='utf-8') as f:
        f.write(text)
    return text


def parse(log):
    rows = {'CASE': [], 'RESULT': [], 'ERROR': [], 'MANUAL': [], 'DISMISS': [], 'RULE': [], 'SHOT': []}
    for line in open(log, encoding='latin-1').read().splitlines():
        m = re.match(r'\[\s*[\d.]+\]\s+(\w+)\s+(.*)$', line)
        if m and m.group(1) in rows:
            rows[m.group(1)].append(m.group(2))
    return rows


def draw(out, summary, run):
    g = os.path.join(run, 'GUI')
    if not os.path.isdir(g):
        return
    out.append('<h2 id="GUI">GUI benchmarks (AUTOGUI)</h2>')
    for log in sorted(f for f in os.listdir(g) if f.upper().endswith('.LOG')):
        name = log[:-4].upper()
        if name.startswith('LEARN') or name == 'DUMP':
            continue
        rows = parse(os.path.join(g, log))
        counts = {}
        for c in rows['CASE']:
            st = (c.split() + ['?', '?'])[1]
            counts[st] = counts.get(st, 0) + 1
        if rows['ERROR']:
            counts['FAIL'] = counts.get('FAIL', 0) + len(rows['ERROR'])
        if rows['MANUAL']:
            counts['MANUAL'] = len(rows['MANUAL'])
        anchor = 'GUI-' + name
        summary.append(('GUI ' + name, anchor, counts))
        out.append('<h3 id="%s">%s</h3>' % (anchor, name))
        if rows['RESULT']:
            out.append('<table><tr><th>result</th><th>value</th></tr>%s</table>' % ''.join(
                '<tr><td>%s</td><td>%s</td></tr>' % tuple(html.escape(x) for x in (r.split(' ', 1) + [''])[:2])
                for r in rows['RESULT']))
        for key in ('CASE', 'ERROR', 'MANUAL'):
            if rows[key]:
                out.append('<p><b>%s</b></p><pre>%s</pre>' % (key, html.escape('\n'.join(rows[key]))))
        if rows['DISMISS'] or rows['RULE']:
            out.append('<details><summary>dialogs answered (%d)</summary><pre>%s</pre></details>' % (
                len(rows['DISMISS']) + len(rows['RULE']), html.escape('\n'.join(rows['RULE'] + rows['DISMISS']))))
        # exported result files of this benchmark (e.g. GUI/ZDWB/*.TXT)
        sub = os.path.join(g, name)
        if os.path.isdir(sub):
            for f in sorted(os.listdir(sub)):
                if f.upper().endswith(('.TXT', '.CSV')) and not f.endswith('.ocr.txt'):
                    body = open(os.path.join(sub, f), encoding='latin-1').read()
                    out.append('<details><summary>%s</summary><pre>%s</pre></details>' % (
                        html.escape(f), html.escape(body[:20000])))
            shots = sorted(f for f in os.listdir(sub) if f.upper().endswith('.BMP'))
            scores = [f for f in shots if re.search(r'score|result|_res', f, re.I)]
            for f in scores:
                p = 'GUI/%s/%s' % (name, f)
                out.append('<p><a href="%s"><img src="%s" style="width:480px"></a></p><pre>OCR %s\n%s</pre>' % (
                    p, p, html.escape(f), html.escape(ocr(os.path.join(sub, f)))))
            if shots:
                out.append('<details><summary>all screenshots (%d)</summary>%s</details>' % (
                    len(shots), ''.join('<a href="GUI/%s/%s"><img src="GUI/%s/%s" title="%s"></a>' % (
                        name, f, name, f, html.escape(f)) for f in shots)))
