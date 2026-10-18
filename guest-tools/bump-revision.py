#!/usr/bin/env python3
"""Raise the Guest Tools revision when the agents' sources changed.

guest-tools/images/SOURCES holds a SHA-256 of the sources the committed
images were built from. When the sources hash differently, the revision in
src/include/86box/guest_tools_version.h goes up by one and SOURCES takes
the new hash; otherwise nothing changes, so building the same sources again
gives the same images. Prints the version.
"""
import hashlib
import os
import re
import sys

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(here)
header = os.path.join(root, 'src', 'include', '86box', 'guest_tools_version.h')
stamp = os.path.join(here, 'images', 'SOURCES')

sources = [
    os.path.join(root, 'src', 'include', '86box', 'guest_tools_proto.h'),
    os.path.join(here, 'README.TXT'),
    os.path.join(here, 'build.sh'),
]
for sub in ('windows', 'win16'):
    folder = os.path.join(here, sub)
    for name in sorted(os.listdir(folder)):
        if name.endswith(('.c', '.h', '.rc', '.py')):
            sources.append(os.path.join(folder, name))

digest = hashlib.sha256()
for path in sources:
    with open(path, 'rb') as f:
        # Line ends do not count: a checkout may convert them.
        digest.update(os.path.relpath(path, root).replace('\\', '/').encode() + b'\0')
        digest.update(f.read().replace(b'\r\n', b'\n') + b'\0')
sha = digest.hexdigest()

text = open(header, encoding='utf-8').read()
major = int(re.search(r'GT_TOOLS_VERSION_MAJOR\s+(\d+)', text).group(1))
minor = int(re.search(r'GT_TOOLS_VERSION_MINOR\s+(\d+)', text).group(1))
rev = int(re.search(r'GT_TOOLS_REVISION\s+(\d+)', text).group(1))

old = open(stamp).read().split()[0] if os.path.exists(stamp) else None
if old is not None and old != sha:
    rev += 1
if old != sha:
    text = re.sub(r'(GT_TOOLS_REVISION\s+)\d+', r'\g<1>%d' % rev, text)
    text = re.sub(r'(GT_TOOLS_VERSION_STR\s+)"[^"]*"', r'\g<1>"%d.%d.%d"' % (major, minor, rev), text)
    with open(header, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    os.makedirs(os.path.dirname(stamp), exist_ok=True)
    with open(stamp, 'w', newline='\n') as f:
        f.write(sha + '\n')

print('%d.%d.%d' % (major, minor, rev))
sys.exit(0)
