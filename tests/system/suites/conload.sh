#!/usr/bin/env sh
#
# conload.sh: every line typed at the console under load arrives.
#
# `conloadprobe` runs five spinners and a writer on a FAT32 volume and reads
# the lines this script types in four stages, each announced by the probe:
# 200 typed back to back into a parked reader, 60 typed while nothing reads,
# 60 typed after an interrupt character to a foreground child, the character
# itself typed behind 200 lines nobody was reading, and 40 typed before the
# probe execs itself. Each line is `L NNNN payload SUM`, SUM the Adler-32 of
# the line before it; the probe requires every line in order with its sum,
# and some bytes of the first stage moved by the port's interrupt, its time
# printed and not judged. The lines of a stage are written
# to the guest as one stream, more than the discipline holds, so the rest
# waits at the UART; the interrupt character behind them is the case where
# ordinary input is held back and the signal must not be held with it.
#
# Usage: conload.sh KERNEL IMAGE HARNESS DISK
set -u
KERNEL=$1; IMAGE=$2; HARNESS=$3; DISK=$4
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
fail() { echo "conload-test: FAIL: $1" >&2; exit 1; }

# lines FIRST COUNT > FILE: the numbered lines with their sums.
lines() {
    awk -v first="$1" -v count="$2" 'BEGIN {
        alphabet = "abcdefghijklmnopqrstuvwxyz0123456789"
        for (n = first; n < first + count; n++) {
            payload = ""
            for (i = 0; i < 20; i++) payload = payload substr(alphabet, ((n + i * 7) % 36) + 1, 1)
            body = sprintf("L %04d %s", n, payload)
            a = 1; b = 0
            for (i = 1; i <= length(body); i++) {
                c = substr(body, i, 1)
                a = (a + index(" !\"#$%&'"'"'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~", c) + 31) % 65521
                b = (b + a) % 65521
            }
            printf "%s %08x\n", body, b * 65536 + a
        }
    }'
}

# 14 MiB of FAT32 at LBA 2048.
dd if=/dev/zero of="$DISK" bs=1048576 count=16 2>/dev/null || fail "the disk"
"$REPO/build/tools/image/mkmbr" --lba "$DISK" 2048:28672:0c >/dev/null || fail "mkmbr"
mformat -i "$DISK"@@1048576 -F -T 28672 -v LOAD :: || fail "mformat"
lines 0 200 > "$DISK.a" || fail "the lines"
lines 200 60 > "$DISK.b"
lines 260 60 > "$DISK.c"
lines 320 40 > "$DISK.d"

if "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
    --qemu-arg=-cpu --qemu-arg=max \
    --drive "file=$DISK,format=raw,if=ide,index=0,media=disk" \
    --append "rach2.rescue" \
    --timeout 120 --steptmo 30 --ok "CONLOAD OK" --never "CONLOAD FAIL" --log "$DISK.log" \
    "expect=Type 'help'" "send=exec /conloadprobe" \
    "expect=CONLOAD A" "sendfile=$DISK.a" \
    "expect=CONLOAD B" "sendfile=$DISK.b" \
    "expect=CONLOAD C" "sendfile=$DISK.a" 'sendraw=\x03' "sendfile=$DISK.c" \
    "expect=CONLOAD D" "sendfile=$DISK.d" \
    "expect=CONLOAD OK" > /dev/null 2>&1; then
    grep -a 'CONLOAD A OK' "$DISK.log" | tail -1 | tr -d '\r' | sed 's/^/conload-test: /'
    echo "conload-test: 360 lines typed under load, every one in order with its sum: OK"
else
    grep -a 'CONLOAD\|console' "$DISK.log" | tr -d '\r' >&2 || true
    fail "the console under load"
fi
