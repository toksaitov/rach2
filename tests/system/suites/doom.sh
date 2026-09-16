#!/bin/sh
# doom.sh: the Doom port's memory zone, on a disk root where the machine has
# its free memory to give. One boot: the game started in the background,
# its zone line read after ten seconds with the game still running, and the
# disk commands it cost recorded. The zone is sized from MemFree less
# 8 MiB and capped at 16, so a 32 MiB machine reports 12 MiB (0xc00000);
# the fixed heap this replaced held 6.
#
# Usage: doom.sh KERNEL DISK HARNESS PASSWORD
set -u
KERNEL=$1; DISK=$2; HARNESS=$3; PASSWORD=$4
fail() { echo "doom-test: FAIL: $1" >&2; exit 1; }
LOG="$DISK.doom.log"
if "$HARNESS" --kernel "$KERNEL" --append "rach2.fsdisk rach2.diskdiag" \
    --qemu-arg=-cpu --qemu-arg=max \
    --drive "file=$DISK,format=raw,if=ide,index=0,media=disk" \
    --timeout 120 --steptmo 60 --ok "DOOMDONE" --log "$LOG" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=# " "send=cd && A=\$(sed -n 's/^disk-commands //p' /proc/disk) && (doom > /tmp/doom.txt 2>&1 &) && sleep 10 && B=\$(sed -n 's/^disk-commands //p' /proc/disk) && echo DOOMRUNNING=\$(ps | grep -c '[d]oom') && echo DOOMCOMMANDS=\$((B-A)) && grep -a 'zone memory' /tmp/doom.txt | sed 's/^/DOOM/' ; echo DOOM\"\"DONE" \
    "expect=DOOMDONE" > /dev/null 2>&1; then
    :
else
    fail "the boot"
fi
RUNNING="$(grep -a '^DOOMRUNNING=' "$LOG" | tail -1 | tr -d '\r' | cut -d= -f2)"
COMMANDS="$(grep -a '^DOOMCOMMANDS=' "$LOG" | tail -1 | tr -d '\r' | cut -d= -f2)"
ZONE="$(grep -a '^DOOMzone memory' "$LOG" | tail -1 | tr -d '\r' | sed -n 's/.*, \([0-9a-f]*\) allocated.*/\1/p')"
[ "${RUNNING:-0}" -ge 1 ] || fail "the game was not running after ten seconds"
echo "doom-test: the game runs on the disk root and cost the disk ${COMMANDS:-?} commands in ten seconds: OK"
[ -n "$ZONE" ] || fail "the game printed no zone line"
if [ "$((0x$ZONE))" -ge "$((10 * 1024 * 1024))" ]; then
    echo "doom-test: the zone is 0x$ZONE bytes, sized from the machine's free memory: OK"
else
    fail "the zone is 0x$ZONE bytes; the heap did not grow past the fixed 8 MiB"
fi
echo "doom-test: PASS"
