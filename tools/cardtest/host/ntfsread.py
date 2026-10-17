"""ntfsread.py -- copy a folder (C:\\CTRES) out of an NTFS guest disk.

review.py/pullres.py read FAT disks themselves; for NTFS (Windows NT 4 /
2000 guests) this mounts the first partition read-only with ntfs-3g in WSL
(root) and copies the folder. Raw images only (a dynamic VHD would need
converting first). Read the image while the guest is shut down, or at least
idle: NTFS may hold unwritten data in the guest's cache.

  ntfsread.py IMAGE GUESTDIR DEST
"""
import os, struct, subprocess, sys


def wslpath(p):
    p = os.path.abspath(p).replace('\\', '/')
    return '/mnt/' + p[0].lower() + p[2:]


def copy_tree(image, guestdir, dest):
    with open(image, 'rb') as f:
        mbr = f.read(512)
    off = struct.unpack('<I', mbr[0x1c6:0x1ca])[0] * 512
    os.makedirs(dest, exist_ok=True)
    src = guestdir.strip('\\/').replace('\\', '/')
    script = ('set -e; m=$(mktemp -d); mount -t ntfs-3g -o ro,loop,offset=%d "%s" "$m"; '
              'trap \'umount "$m"; rmdir "$m"\' EXIT; '
              's=$(find "$m" -maxdepth 1 -iname "%s" | head -1); [ -n "$s" ] || { echo "no %s"; exit 3; }; '
              'cp -r "$s"/. "%s"/' % (off, wslpath(image), src.split('/')[0], src, wslpath(dest)))
    env = dict(os.environ, MSYS_NO_PATHCONV='1')
    r = subprocess.run(['wsl', '-u', 'root', '-e', 'bash', '-c', script], capture_output=True, env=env)
    if r.returncode:
        raise SystemExit('ntfsread: ' + (r.stdout + r.stderr).decode('utf-8', 'replace').strip())


if __name__ == '__main__':
    copy_tree(sys.argv[1], sys.argv[2], sys.argv[3])
