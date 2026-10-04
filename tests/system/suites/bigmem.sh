#!/usr/bin/env sh
#
# bigmem.sh: all of an x86-64 machine's eight gigabytes, used.
#
# The x86-64 kernel boots the test image with 8 GiB, more than QEMU's PC
# puts below 4 GiB: the RAM past the first 3 GiB is mapped from 4 GiB up.
# The memory report must count all of it usable and none unreachable, and
# with QEMU's fixed holes, the 513 KiB the memory suite names, the usable
# bytes are the size itself; managed, counted from the frame table, is
# usable less reserved within the partial frames at the ranges' ends; and
# /proc/meminfo's MemTotal, which `memprobe total` prints, is the managed
# count. Then memprobe maps 5 GiB, more than lies below 4 GiB, so that its
# pages must take frames above it, writes each page a pattern of its own
# index and reads every one back, forks over the mapping, execs a fresh
# image that must find its pages zeroed, and frees it, as the memory suite's
# smaller rows do.
#
# Last the kernel boots with 72 GiB on a processor without 1 GiB pages, whose
# direct map stops at 64 GiB, since each of its GiB is a page directory the
# boot carves below 32 MiB. QEMU's RAM is the first 3 GiB and 69 GiB from
# 4 GiB, so the report must count 63 GiB less the holes usable and the
# 9 GiB past 64 GiB unreachable, and the boot, whose frame table lies at the
# top of the usable memory, must reach the shell through 2 MiB pages.
#
# Usage: bigmem.sh KERNEL IMAGE HARNESS
set -u
KERNEL=$1; IMAGE=$2; HARNESS=$3
# What this suite writes under the images directory starts with `bigmem-test`.
OUT="$(dirname "$IMAGE")/bigmem-test"
MIB=8192
MAP=5120
fail() { echo "bigmem-test: FAIL: $1" >&2; exit 1; }

LOG="$OUT.boot.log"
"$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-m --qemu-arg="$MIB" --append "rach2.rescue" \
    --timeout 120 --steptmo 60 --ok "MEMPROBE MemTotal" --never "MEMPROBE FAIL" --log "$LOG" \
    "expect=Type 'help'" "send=exec /memprobe total" "expect=MEMPROBE MemTotal" > /dev/null 2>&1 \
    || fail "the boot at $MIB MiB"
line=$(grep -a 'memory: usable' "$LOG" | head -1 | tr -d '\r')
[ -n "$line" ] || fail "no memory report at $MIB MiB"
usable=$(echo "$line" | sed -n 's/.*usable \([0-9]*\) KiB.*/\1/p')
unreachable=$(echo "$line" | sed -n 's/.*unreachable \([0-9]*\) KiB.*/\1/p')
reserved=$(echo "$line" | sed -n 's/.*reserved \([0-9]*\) KiB.*/\1/p')
managed=$(echo "$line" | sed -n 's/.*managed \([0-9]*\) KiB.*/\1/p')
[ -n "$usable" ] && [ -n "$unreachable" ] && [ -n "$reserved" ] && [ -n "$managed" ] \
    || fail "the report at $MIB MiB is not four numbers: $line"
[ "$unreachable" -eq 0 ] || fail "$unreachable KiB of $MIB MiB is unreachable"
[ "$((usable + 513))" -eq "$((MIB * 1024))" ] \
    || fail "usable $usable KiB and the holes are not the machine's $MIB MiB"
[ "$managed" -le "$((usable - reserved))" ] && [ "$managed" -gt "$((usable - reserved - 32))" ] \
    || fail "managed $managed is not usable $usable less reserved $reserved"
total=$(grep -a 'MEMPROBE MemTotal' "$LOG" | head -1 | tr -d '\r' |
    sed -n 's/.*MemTotal \([0-9]*\) kB.*/\1/p')
[ -n "$total" ] || fail "memprobe printed no MemTotal"
[ "$total" -eq "$managed" ] || fail "MemTotal $total kB is not the $managed KiB the kernel manages"
echo "bigmem-test: $MIB MiB: usable $usable, unreachable 0, reserved $reserved, managed and MemTotal $managed KiB: OK"

LOG="$OUT.memprobe.log"
"$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-m --qemu-arg="$MIB" --append "rach2.rescue" \
    --timeout 900 --steptmo 840 --ok "MEMPROBE OK" --never "MEMPROBE FAIL" --log "$LOG" \
    "expect=Type 'help'" "send=exec /memprobe $MAP" "expect=MEMPROBE" > /dev/null 2>&1 \
    || fail "memprobe $MAP at $MIB MiB: $(grep -a 'MEMPROBE\|vm: fault\|panic' "$LOG" | head -2 | tr -d '\r' | tr '\n' ' ')"
echo "bigmem-test: $MAP MiB, past what lies below 4 GiB, mapped, patterned, forked over, execed and freed: OK"

MIB=73728
LOG="$OUT.nogib.log"
"$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" --qemu-arg=-cpu --qemu-arg=max,pdpe1gb=off \
    --qemu-arg=-m --qemu-arg="$MIB" --append "rach2.rescue" \
    --timeout 120 --steptmo 60 --ok "MEMPROBE MemTotal" --never "MEMPROBE FAIL" --log "$LOG" \
    "expect=Type 'help'" "send=exec /memprobe total" "expect=MEMPROBE MemTotal" > /dev/null 2>&1 \
    || fail "the boot at $MIB MiB without 1 GiB pages"
line=$(grep -a 'memory: usable' "$LOG" | head -1 | tr -d '\r')
usable=$(echo "$line" | sed -n 's/.*usable \([0-9]*\) KiB.*/\1/p')
unreachable=$(echo "$line" | sed -n 's/.*unreachable \([0-9]*\) KiB.*/\1/p')
[ -n "$usable" ] && [ -n "$unreachable" ] || fail "no memory report at $MIB MiB without 1 GiB pages"
[ "$unreachable" -eq $((9 * 1024 * 1024)) ] \
    || fail "$unreachable KiB of $MIB MiB is unreachable without 1 GiB pages, not the 9 GiB past 64 GiB"
[ "$((usable + 513))" -eq $((63 * 1024 * 1024)) ] \
    || fail "usable $usable KiB and the holes are not the 63 GiB below 64 GiB"
echo "bigmem-test: $MIB MiB without 1 GiB pages: usable $usable, unreachable $unreachable KiB, past 64 GiB: OK"
echo "bigmem-test: PASS"
