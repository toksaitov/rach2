#!/bin/sh
# mkexfat_external.sh DIR: an exFAT volume another implementation wrote, for
# the rows of exfat_hosttest.c that read one: formatted by the host's own
# tools and filled through the host's own driver, with a manifest of what went
# in. On macOS that is hdiutil (which runs newfs_exfat) and the mount it gives.
# Writes DIR/exfat_external.img, DIR/exfat_external.manifest (name|size|
# mtime as an epoch second) and the source files under DIR/exfat_src/.
# Exits 3 when the host has no such tools, which the test reports as rows
# skipped, not passed.
set -u
DIR=${1:?directory}
command -v hdiutil >/dev/null 2>&1 && command -v newfs_exfat >/dev/null 2>&1 || exit 3
IMG="$DIR/exfat_external.img"
SRC="$DIR/exfat_src"
rm -rf "$IMG" "$SRC" "$DIR/exfat_external.manifest"
mkdir -p "$SRC/Sub Dir"
printf 'hello exfat\n' > "$SRC/hello.txt"
python3 -c 'import sys; sys.stdout.buffer.write(bytes(range(256)) * 400)' > "$SRC/pattern.bin"
printf 'inner\n' > "$SRC/Sub Dir/inner.txt"
: > "$SRC/empty"
ACCENT="$(printf 'e\303\251t\303\251.txt')" # e-acute t e-acute, kept ASCII here
printf 'accent\n' > "$SRC/$ACCENT"
printf 'x' > "$SRC/A name of some length that takes more than one name entry.txt"
hdiutil create -size 16m -layout NONE -fs ExFAT -volname EXTGATE "$IMG" >/dev/null 2>&1 || exit 1
mv "$IMG.dmg" "$IMG" 2>/dev/null || true
MNT=$(hdiutil attach -nobrowse -imagekey diskimage-class=CRawDiskImage "$IMG" 2>/dev/null | awk -F'\t' '{print $NF}' | tail -1)
[ -n "$MNT" ] && [ -d "$MNT" ] || exit 1
cp -R "$SRC/." "$MNT/" || { hdiutil detach "$MNT" >/dev/null 2>&1; exit 1; }
sync
(cd "$MNT" && for f in hello.txt pattern.bin "Sub Dir/inner.txt" empty "$ACCENT" \
    "A name of some length that takes more than one name entry.txt"; do
    printf '%s|%s|%s\n' "$f" "$(stat -f %z "$f")" "$(stat -f %m "$f")"
done) > "$DIR/exfat_external.manifest"
hdiutil detach "$MNT" >/dev/null 2>&1 || exit 1
exit 0
