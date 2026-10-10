#!/bin/sh
# Build 86Box-Next Release N, keep its zip and stage it in every rig.
#
#   tools/make-release.sh N      (run from the repo root, Git Bash)
#
# 1. Builds build-static with NEXT_RELEASE=N: the title bar reads
#    "86Box-Next 7.0 - Release N".
# 2. Packages the stripped 86Box-Next.exe and isp-server.exe with COPYING as
#    ../RELEASES-ARCHIVE/86Box-Next-Release-N-win64.zip -- the archive keeps
#    every release's zip; an existing one is never overwritten.
# 3. Stages exactly those exes: 86Box-Next.exe into every rig in ../Latest
#    (as tools/stage-rig.sh picks them), isp-server.exe into ../Latest.
#
# Publishing stays a separate step (it is printed at the end): tag the
# commit release-N, push the tag, gh release create with the zip.
set -e
N=$1
case "$N" in
    "" | *[!0-9A-Za-z._-]*) echo "usage: tools/make-release.sh N" >&2; exit 1 ;;
esac
NAME=86Box-Next-Release-$N
ARCHIVE=../RELEASES-ARCHIVE
ZIP=$ARCHIVE/$NAME-win64.zip
OUT=../Latest
UCRT=/c/msys64/ucrt64

[ -e "$ZIP" ] && { echo "$ZIP already exists" >&2; exit 1; }
# Releases are Qt 6 only; build-qt5 exists just to keep the sources building with Qt 5.
grep -q "^USE_QT6:BOOL=ON$" build-static/CMakeCache.txt 2>/dev/null ||
    { echo "build-static is not a Qt 6 build (USE_QT6=ON): releases are Qt 6 only" >&2; exit 1; }
[ -z "$(git status --porcelain --untracked-files=no)" ] || { echo "uncommitted changes: commit first" >&2; exit 1; }
if tasklist //FI "IMAGENAME eq 86Box-Next.exe" 2>/dev/null | grep -qi "86Box-Next.exe" ||
   tasklist //FI "IMAGENAME eq isp-server.exe" 2>/dev/null | grep -qi "isp-server.exe"; then
    echo "86Box-Next or isp-server is running; close it first" >&2
    exit 2
fi

# The build, with git off the MSYS2 PATH (see CLAUDE.md).
(
    export PATH="$UCRT/bin:/c/msys64/usr/bin:$PATH" MSYSTEM=UCRT64
    NEXT_RELEASE=$N cmake --build build-static
)

# Inside the build tree, not mktemp's /tmp: Git Bash and the native strip
# would each take /tmp for a different folder.
STAGE=build-static/release-stage
rm -rf "$STAGE"
mkdir -p "$STAGE/$NAME"
"$UCRT/bin/strip" -o "$STAGE/$NAME/86Box-Next.exe" build-static/src/86Box-Next.exe
"$UCRT/bin/strip" -o "$STAGE/$NAME/isp-server.exe" build-static/isp-server/isp-server.exe
cp COPYING "$STAGE/$NAME/"
mkdir -p "$ARCHIVE"
ZIPWIN=$(cygpath -w "$(cd "$ARCHIVE" && pwd)")\\$NAME-win64.zip
SRCWIN=$(cygpath -w "$(pwd)/$STAGE/$NAME")
powershell -NoProfile -Command "Compress-Archive -Path '$SRCWIN' -DestinationPath '$ZIPWIN'"
[ -f "$ZIP" ] || { echo "no $ZIP made" >&2; exit 1; }
echo "Archived $ZIP"

for d in "$OUT"/*/; do
    d=${d%/}
    if [ -f "$d/86box.cfg" ] || [ -f "$d/86Box-Next.exe" ]; then
        cp "$STAGE/$NAME/86Box-Next.exe" "$d/86Box-Next.exe"
        echo "Staged Release $N in $d"
    fi
done
cp "$STAGE/$NAME/isp-server.exe" "$OUT/isp-server.exe"
echo "Staged isp-server in $OUT"
rm -rf "$STAGE"

echo
echo "To publish:"
echo "  git tag -a release-$N -m \"86Box-Next Release $N\" && git push origin release-$N"
echo "  gh release create release-$N \"$ZIP\" -R Xeon3D/86Box-Next --title \"86Box-Next Release $N\" --notes-file <notes.md> --verify-tag"
