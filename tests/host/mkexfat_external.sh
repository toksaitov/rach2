#!/bin/sh
# mkexfat_external.sh DIR: an exFAT volume another implementation wrote, for
# the rows of exfat_hosttest.c that read one, with a manifest of what went in.
# On macOS it is formatted by hdiutil (which runs newfs_exfat) and filled
# through the mount hdiutil gives; on any other host, by exfatprogs'
# mkfs.exfat and Linux's own driver, in etude's tooling machine
# (`etude provision tooling`, tools/etude/Readme.md), which runs under
# the accelerator RACH2_QEMU_ACCEL names. Writes DIR/exfat_external.img,
# DIR/exfat_external.manifest (name|size|mtime as an epoch second) and the
# source files under DIR/exfat_src/. Exits 3 when the host has neither
# hdiutil and newfs_exfat nor a made tooling machine, which the test reports
# as a failure, and 1 when the volume cannot be made. An existing root Linux
# tooling job uses its own formatter and driver without another machine.
#
# mkexfat_external.sh -p FILE readies the writer, as the host case's
# preparation does through `make build/tests/host/exfat_writer`: on a host
# without hdiutil and newfs_exfat it makes the tooling machine when it is not
# made, and FILE has its `made` line, as tests/system/tooling_ready.sh writes
# it; on macOS FILE has `hdiutil`, and in an existing Linux tooling job it
# has `linux-tooling`. FILE is written only when its line
# changes, so that the preparation's record of it stays the same. Exits 0, 1
# when the machine cannot be made, and 3 when the host has neither hdiutil
# and newfs_exfat nor etude.
set -u
ETUDE=build/tools/etude/etude
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

# Remove the private Linux mount and scratch after any exit.
# shellcheck disable=SC2329 # the exit trap calls it
cleanup_linux() {
    _rc=$?
    if [ -n "$_linux_work" ]; then
        if mountpoint -q "$_linux_work/mnt"; then
            umount "$_linux_work/mnt" || exit 1
        fi
        rm -rf "$_linux_work" || exit 1
    fi
    exit "$_rc"
}

# Format a private image inside the existing root Linux tooling job.
linux_fixture() {
    command -v mountpoint >/dev/null 2>&1 || exit 1
    _linux_work=
    trap cleanup_linux 0
    trap 'exit 130' INT
    trap 'exit 143' TERM HUP
    _linux_work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-exfat.XXXXXX") || exit 1
    mkdir "$_linux_work/mnt" || exit 1
    truncate -s 16M "$IMG" || exit 1
    mkfs.exfat -L EXTGATE "$IMG" >/dev/null || exit 1
    mount -t exfat -o loop,iocharset=utf8 "$IMG" "$_linux_work/mnt" || exit 1
    cp -R "$SRC/." "$_linux_work/mnt/" || exit 1
    sync
    (cd "$_linux_work/mnt" &&
        for f in hello.txt pattern.bin "Sub Dir/inner.txt" empty "$ACCENT" "$LONG"; do
            _size=$(stat -c %s "$f") || exit 1
            _mtime=$(stat -c %Y "$f") || exit 1
            printf '%s|%s|%s\n' "$f" "$_size" "$_mtime" || exit 1
        done) > "$DIR/exfat_external.manifest" || exit 1
    exit 0
}

if command -v hdiutil >/dev/null 2>&1 && command -v newfs_exfat >/dev/null 2>&1; then
    host=mac
elif [ "$(uname -s)" = Linux ] && [ "$(id -u)" = 0 ] &&
     [ -n "${ETUDE_WORK:-}" ] && [ -d "$ETUDE_WORK" ] &&
     [ -b "${ETUDE_DISK1:-}" ]; then
    host=linux
elif [ -x "$ETUDE" ]; then
    host=tooling
else
    exit 3
fi
if [ "${1:-}" = -p ]; then
    OUT=${2:?file}
    case $host in
        tooling) exec sh tests/system/tooling_ready.sh "$OUT" ;;
        mac) line=hdiutil ;;
        linux) command -v mkfs.exfat >/dev/null 2>&1 || exit 1; line=linux-tooling ;;
    esac
    [ "$(cat "$OUT" 2>/dev/null)" = "$line" ] && exit 0
    echo "$line" > "$OUT.tmp" && mv "$OUT.tmp" "$OUT" || exit 1
    exit 0
fi
DIR=${1:?directory}
IMG="$DIR/exfat_external.img"
SRC="$DIR/exfat_src"
[ "$host" != tooling ] || "$ETUDE" provision tooling check >/dev/null 2>&1 || exit 3
rm -rf "$IMG" "$SRC" "$DIR/exfat_external.manifest" "$DIR/exfat_job.sh"
make_src "$SRC" || exit 1
[ "$host" != linux ] || linux_fixture
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
"$ETUDE" provision tooling run -d "$IMG" -o exfat_external.manifest -O "$DIR" "$DIR/exfat_job.sh" >&2 || exit 1
rm -f "$DIR/exfat_job.sh"
[ -s "$DIR/exfat_external.manifest" ] || exit 1
exit 0
