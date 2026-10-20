#!/bin/sh
# mkexfat_external.sh DIR: an exFAT volume another implementation wrote, for
# the rows of exfat_hosttest.c that read one, with a manifest of what went in.
# On macOS it is formatted by hdiutil (which runs newfs_exfat) and filled
# through the mount hdiutil gives; on any other host, by exfatprogs'
# mkfs.exfat and Linux's own driver, in etude's tooling machine
# (build/tools/etude/etude-tooling, tools/etude/Readme.md), which runs under
# the accelerator RACH2_QEMU_ACCEL names. Writes DIR/exfat_external.img,
# DIR/exfat_external.manifest (name|size|mtime as an epoch second) and the
# source files under DIR/exfat_src/. Exits 3 when the host has neither
# hdiutil and newfs_exfat nor a made tooling machine, which the test reports
# as rows not run.
set -u
DIR=${1:?directory}
TOOLING=build/tools/etude/etude-tooling
IMG="$DIR/exfat_external.img"
SRC="$DIR/exfat_src"
ACCENT="$(printf 'e\303\251t\303\251.txt')" # e-acute t e-acute, kept ASCII here
LONG="A name of some length that takes more than one name entry.txt"

# make_src D: the six files under D. The text is the job's too.
# shellcheck disable=SC2016 # the text expands where it is evaluated
MAKE_SRC='make_src() {
    mkdir -p "$1/Sub Dir" || return 1
    printf "hello exfat\\n" > "$1/hello.txt"
    python3 -c "import sys; sys.stdout.buffer.write(bytes(range(256)) * 400)" > "$1/pattern.bin"
    printf "inner\\n" > "$1/Sub Dir/inner.txt"
    : > "$1/empty"
    printf "accent\\n" > "$1/$ACCENT"
    printf x > "$1/$LONG"
}'
eval "$MAKE_SRC"

if command -v hdiutil >/dev/null 2>&1 && command -v newfs_exfat >/dev/null 2>&1; then
    host=mac
elif [ -x "$TOOLING" ] && "$TOOLING" check >/dev/null 2>&1; then
    host=tooling
else
    exit 3
fi
rm -rf "$IMG" "$SRC" "$DIR/exfat_external.manifest" "$DIR/exfat_job.sh"
make_src "$SRC" || exit 1
if [ "$host" = mac ]; then
    hdiutil create -size 16m -layout NONE -fs ExFAT -volname EXTGATE "$IMG" >/dev/null 2>&1 || exit 1
    mv "$IMG.dmg" "$IMG" 2>/dev/null || true
    MNT=$(hdiutil attach -nobrowse -imagekey diskimage-class=CRawDiskImage "$IMG" 2>/dev/null | awk -F'\t' '{print $NF}' | tail -1)
    [ -n "$MNT" ] && [ -d "$MNT" ] || exit 1
    cp -R "$SRC/." "$MNT/" || { hdiutil detach "$MNT" >/dev/null 2>&1; exit 1; }
    sync
    (cd "$MNT" && for f in hello.txt pattern.bin "Sub Dir/inner.txt" empty "$ACCENT" "$LONG"; do
        printf '%s|%s|%s\n' "$f" "$(stat -f %z "$f")" "$(stat -f %m "$f")"
    done) > "$DIR/exfat_external.manifest"
    hdiutil detach "$MNT" >/dev/null 2>&1 || exit 1
    exit 0
fi

# The job makes the same files in the machine, from make_src's own text, so
# that no name passes through the FAT job volume.
truncate -s 16M "$IMG" || exit 1
{
    echo 'set -eu'
    printf "ACCENT='%s'\nLONG='%s'\n" "$ACCENT" "$LONG"
    printf '%s\n' "$MAKE_SRC"
    cat <<'EOF'
mkfs.exfat -L EXTGATE "$ETUDE_DISK1" > /dev/null
make_src src
mkdir mnt
mount -t exfat -o iocharset=utf8 "$ETUDE_DISK1" mnt
cp -R src/. mnt/
sync
(cd mnt && for f in hello.txt pattern.bin "Sub Dir/inner.txt" empty "$ACCENT" "$LONG"; do
    printf '%s|%s|%s\n' "$f" "$(stat -c %s "$f")" "$(stat -c %Y "$f")"
done) > exfat_external.manifest
umount mnt
EOF
} > "$DIR/exfat_job.sh" || exit 1
"$TOOLING" run -d "$IMG" -o exfat_external.manifest -O "$DIR" "$DIR/exfat_job.sh" >&2 || exit 1
rm -f "$DIR/exfat_job.sh"
[ -s "$DIR/exfat_external.manifest" ] || exit 1
exit 0
