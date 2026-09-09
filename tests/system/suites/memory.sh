#!/usr/bin/env sh
#
# memory.sh: the kernel's account of physical memory at five sizes.
#
# The same kernel boots the test image with 64 MiB, 512 MiB, 2 GiB, 3 GiB
# and 4 GiB, and at each size the four numbers it reports must follow from
# the map it printed and from the size the machine was given: usable and
# unreachable together are the map's available bytes, and with QEMU's fixed
# holes added, the kilobyte at 0x9FC00, the 384 KiB below 1 MiB and the
# 128 KiB at the end of low RAM, they are the size itself; managed, which
# the kernel counts from its frame table and not from the map's arithmetic,
# is usable less reserved, within the partial frames at the ranges' ends.
# That the numbers change with the size is what shows the harness passed
# the size to the boot.
#
# Usage: memory.sh KERNEL IMAGE HARNESS RECOVERY_KERNEL RECOVERY_IMAGE
set -u
KERNEL=$1; IMAGE=$2; HARNESS=$3; RKERNEL=$4; RIMAGE=$5
fail() { echo "memory-test: FAIL: $1" >&2; exit 1; }

# A machine too small is refused by name in the memory report, not faulted
# in. The recovery pair is what fits: at 8 MiB the kernel, its 4 MiB image
# and the tables take everything, and the test image alone is 16 MiB.
LOG="$IMAGE.memory-8.log"
"$HARNESS" --kernel "$RKERNEL" --initrd "$RIMAGE" --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-m --qemu-arg=8 --timeout 30 --ok "memory: refused" --never "vm: fault" \
    --log "$LOG" "expect=memory: refused" > /dev/null 2>&1 \
    || fail "the boot at 8 MiB was not refused by name: $(grep -a 'memory: refused\|vm: fault\|panic' "$LOG" | head -2 | tr -d '\r' | tr '\n' ' ')"
echo "memory-test: 8 MiB: refused by name in the memory report, with no fault: OK"

for mib in 64 512 2048 3072 4096; do
    LOG="$IMAGE.memory-$mib.log"
    "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" --qemu-arg=-cpu --qemu-arg=max \
        --qemu-arg=-m --qemu-arg="$mib" --append "rach2.rescue" \
        --timeout 60 --ok "Type 'help'" --log "$LOG" "expect=Type 'help'" > /dev/null 2>&1 \
        || fail "the boot at $mib MiB"
    line=$(grep -a 'memory: usable' "$LOG" | head -1 | tr -d '\r')
    [ -n "$line" ] || fail "no memory report at $mib MiB"
    usable=$(echo "$line" | sed -n 's/.*usable \([0-9]*\) KiB.*/\1/p')
    unreachable=$(echo "$line" | sed -n 's/.*unreachable \([0-9]*\) KiB.*/\1/p')
    reserved=$(echo "$line" | sed -n 's/.*reserved \([0-9]*\) KiB.*/\1/p')
    managed=$(echo "$line" | sed -n 's/.*managed \([0-9]*\) KiB.*/\1/p')
    [ -n "$usable" ] && [ -n "$unreachable" ] && [ -n "$reserved" ] && [ -n "$managed" ] \
        || fail "the report at $mib MiB is not four numbers: $line"
    # The map's available bytes, from the lines the kernel printed, summed in
    # KiB so the arithmetic stays inside what printf's %d takes.
    available=0
    for range in $(grep -a 'memory: map: .* available' "$LOG" | tr -d '\r' | sed -n 's/.*map: \(0x[0-9A-F]*-0x[0-9A-F]*\) available/\1/p'); do
        start=$(printf '%d' "${range%-*}")
        end=$(printf '%d' "${range#*-}")
        available=$((available + (end - start) / 1024))
    done
    [ "$((usable + unreachable))" -eq "$available" ] \
        || fail "at $mib MiB usable $usable and unreachable $unreachable are not the map's $available KiB"
    [ "$((usable + unreachable + 513))" -eq "$((mib * 1024))" ] \
        || fail "at $mib MiB usable $usable and unreachable $unreachable and the holes are not the size"
    # Managed is counted from the frame table, whole frames only, so it may
    # fall short of the map's arithmetic by the partial frame at a range's end.
    [ "$managed" -le "$((usable - reserved))" ] && [ "$managed" -gt "$((usable - reserved - 32))" ] \
        || fail "at $mib MiB managed $managed is not usable $usable less reserved $reserved"
    echo "memory-test: $mib MiB: usable $usable, unreachable $unreachable, reserved $reserved, managed $managed KiB: OK"
done

# The frames beyond the old 32 MiB window, used: at the two ends of the
# range, the probe maps more than the window held, patterns it, forks over
# it, execs and frees it.
for spec in "64 24" "4096 1024"; do
    mib=${spec% *}; map=${spec#* }
    LOG="$IMAGE.memprobe-$mib.log"
    "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" --qemu-arg=-cpu --qemu-arg=max \
        --qemu-arg=-m --qemu-arg="$mib" --append "rach2.rescue" \
        --timeout 300 --steptmo 240 --ok "MEMPROBE OK" --never "MEMPROBE FAIL" --log "$LOG" \
        "expect=Type 'help'" "send=exec /memprobe $map" "expect=MEMPROBE" > /dev/null 2>&1 \
        || fail "memprobe $map at $mib MiB"
    echo "memory-test: $map MiB mapped, patterned, forked over, execed and freed at $mib MiB: OK"
done

# A frame allocation failing at each step of fork and of exec, at the two
# ends: the caller answers ENOMEM, nothing leaks, nothing surviving changes.
for mib in 64 4096; do
    LOG="$IMAGE.framefail-$mib.log"
    "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" --qemu-arg=-cpu --qemu-arg=max \
        --qemu-arg=-m --qemu-arg="$mib" --append "rach2.rescue rach2.framefail" \
        --timeout 240 --steptmo 180 --ok "MEMPROBE OK" --never "MEMPROBE FAIL" --log "$LOG" \
        "expect=Type 'help'" "send=exec /memprobe fail" \
        "expect=frame allocation failed on purpose" "expect=MEMPROBE" > /dev/null 2>&1 \
        || fail "memprobe fail at $mib MiB"
    echo "memory-test: a frame allocation failing at each step of fork and exec at $mib MiB: OK"
done
echo "memory-test: PASS"
