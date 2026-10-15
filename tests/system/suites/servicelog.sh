#!/usr/bin/env sh
#
# servicelog.sh: the log init's services write to is bounded, within a boot and
# at the next one.
#
# init sends what its services print to /var/log/services and empties the file
# when it reaches its cap, 256 KiB, leaving one line that says so, so a service
# that fails in a loop cannot fill a root (`bound_service_log` in
# utils/init.c). It looks at the file every sixty turns of its loop, and a turn
# that reaps a child is shorter than an idle one, so orphans to reap bring the
# look sooner. A boot that finds the file at its cap, left so by the boots
# before, removes it before any service writes, and writes no line
# (`open_service_log`).
#
# The root is an ext2 volume made here with init-shell as its init, BusyBox as
# its shell and `update` as the one service init starts, so a boot comes to a
# root shell at the console. The first boot fills the log past the cap, leaves
# eighty orphans to init and requires the file to be emptied down to init's
# line; then it fills the file again and powers off, which unmounts the root
# cleanly. The second boot requires the file to have been removed: small, and
# without the line the emptying writes.
#
# Usage: servicelog.sh KERNEL HARNESS MKMBR INIT UPDATE BUSYBOX DIR
#
# INIT is the init-shell binary, which tests/system/servicelog_mutate.sh builds
# from a private copy. The disk and the consoles are in DIR.
#

REPO=$(cd "$(dirname "$0")/../../.." && pwd)
# shellcheck source=tests/system/deadline.sh
. "$REPO/tests/system/deadline.sh"
set -u
KERNEL=$1; HARNESS=$2; MKMBR=$3; INIT=$4; UPDATE=$5; BUSYBOX=$6; DIR=$7
fail() { echo "servicelog-test: FAIL: $1" >&2; exit 1; }
note() { echo "servicelog-test: $1"; }

. "$REPO/tools/setup/e2fsprogs_path.sh"

mkdir -p "$DIR"
TREE="$DIR/tree"
VOL="$DIR/root.vol"
DISK="$DIR/root.disk"
rm -rf "$TREE" "$VOL" "$DISK" "$DIR"/boot*.log "$DIR"/boot*.harness
mkdir -p "$TREE/sbin" "$TREE/bin" "$TREE/etc" "$TREE/var/log" || fail "the staging tree"
cp "$INIT" "$TREE/sbin/init" || fail "the init"
cp "$UPDATE" "$TREE/bin/update" || fail "update"
cp "$BUSYBOX" "$TREE/bin/busybox" || fail "busybox"
cp "$BUSYBOX" "$TREE/bin/sh" || fail "the shell"
chmod 755 "$TREE/sbin/init" "$TREE/bin/update" "$TREE/bin/busybox" "$TREE/bin/sh"
# The login shell reads this, and the line it prints is the token a boot waits
# for: the prompt alone is too short to tell from the kernel's own lines.
printf 'export PATH=/bin\necho servicelog: shell up\n' > "$TREE/etc/profile"
PROFILE="sparse_super,large_file,filetype,^dir_index,resize_inode,^ext_attr,^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize"
: > "$VOL"
E2FSPROGS_FAKE_TIME=1 mke2fs -q -F -t ext2 -b 4096 -I 128 -O "$PROFILE" -d "$TREE" "$VOL" 16M \
    > "$DIR/mke2fs.log" 2>&1 || fail "the ext2 volume; see $DIR/mke2fs.log"
{
    dd if=/dev/zero bs=1048576 count=1 2>/dev/null
    cat "$VOL"
    dd if=/dev/zero bs=1048576 count=1 2>/dev/null
} > "$DISK" || fail "the disk"
"$MKMBR" --lba "$DISK" 2048:32768:83 > /dev/null || fail "the disk's table"
rm -rf "$TREE" "$VOL"

boot() { # boot NAME STEPS...: one boot of the disk, its console in DIR/NAME.log
    name=$1
    shift
    "$HARNESS" --kernel "$KERNEL" --qemu-arg=-cpu --qemu-arg=max \
        --drive "file=$DISK,format=raw,if=ide,index=0,media=disk" \
        --append "rach2.root=ext2" --timeout 600 --steptmo 300 --log "$DIR/$name.log" \
        "$@" > "$DIR/$name.harness" 2>&1
}

# The value `NAME=VALUE` a boot printed last for NAME.
value() { # value LOG NAME
    grep -a "^$2=" "$1" | tail -1 | tr -d '\r' | cut -d= -f2
}

# The first boot. The orphans are the children of subshells that have exited,
# and init reaps each in a turn of its own. The wait is the guest's own,
# bounded, so that a kernel whose init never empties the file prints the
# count and does not hold the boot to the step's limit.
TRIES=$(scaled 150)
# interval: the guest polls the log's size each second until init bounds it
if ! boot boot1 --ok "FIRSTDONE" --must-exit \
    "boot=servicelog: shell up" \
    "send=dd if=/dev/zero bs=1024 count=300 >> /var/log/services 2>/dev/null; echo FILLED\"\"=\$(wc -c < /var/log/services)" \
    "expect=FILLED=" \
    "send=i=0; while [ \$i -lt 80 ]; do (true &); i=\$((i + 1)); done; echo ORPHANS\"\"LEFT" \
    "expect=ORPHANSLEFT" \
    "send=n=0; while [ \$n -lt $TRIES ] && [ \$(wc -c < /var/log/services) -ge 262144 ]; do sleep 1; n=\$((n + 1)); done; echo WAITED\"\"=\$n" \
    "expect=WAITED=" \
    "send=echo SIZE\"\"=\$(wc -c < /var/log/services); echo NOTES\"\"=\$(grep -c 'reached its limit' /var/log/services)" \
    "expect=NOTES=" \
    "send=dd if=/dev/zero bs=1024 count=300 >> /var/log/services 2>/dev/null; sync; echo REFILLED\"\"=\$(wc -c < /var/log/services)" \
    "expect=REFILLED=" \
    "send=echo FIRST\"\"DONE; poweroff" \
    "expect=FIRSTDONE"; then
    sed -n 's/^HARNESS: /servicelog-test: /p' "$DIR/boot1.harness" >&2
    fail "the first boot did not run to its power-off; see $DIR/boot1.log"
fi
FILLED=$(value "$DIR/boot1.log" FILLED)
SIZE=$(value "$DIR/boot1.log" SIZE)
NOTES=$(value "$DIR/boot1.log" NOTES)
REFILLED=$(value "$DIR/boot1.log" REFILLED)
[ -n "$FILLED" ] && [ "$FILLED" -ge 262144 ] ||
    fail "the log was filled to '$FILLED' bytes, not past its cap"
if [ -n "$SIZE" ] && [ "$SIZE" -lt 4096 ] && [ "$NOTES" = 1 ]; then
    note "a log past its cap is emptied within the boot, to the line that says so: OK"
else
    fail "the log was not emptied within the boot: $SIZE bytes, $NOTES lines saying so"
fi
[ -n "$REFILLED" ] && [ "$REFILLED" -ge 262144 ] ||
    fail "the log was refilled to '$REFILLED' bytes, not past its cap"
grep -aq 'shutdown: root: unmounted' "$DIR/boot1.log" ||
    fail "the first boot did not unmount the root at its power-off"

# The second boot, on the root the first left.
if ! boot boot2 --ok "SECONDDONE" \
    "boot=servicelog: shell up" \
    "send=echo SIZE\"\"=\$(wc -c < /var/log/services); echo NOTES\"\"=\$(grep -c 'reached its limit' /var/log/services); echo SECOND\"\"DONE" \
    "expect=SECONDDONE"; then
    sed -n 's/^HARNESS: /servicelog-test: /p' "$DIR/boot2.harness" >&2
    fail "the second boot did not come to its shell; see $DIR/boot2.log"
fi
SIZE=$(value "$DIR/boot2.log" SIZE)
NOTES=$(value "$DIR/boot2.log" NOTES)
if [ -n "$SIZE" ] && [ "$SIZE" -lt 4096 ] && [ "$NOTES" = 0 ]; then
    note "a log the boot before left past its cap is removed at the next boot: OK"
else
    fail "the log the boot before left full was not removed: $SIZE bytes, $NOTES lines saying it was emptied"
fi
rm -f "$DISK"
note "PASS"
