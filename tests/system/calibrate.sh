#!/bin/sh
# calibrate.sh WIDTH DIR: the emulator's rate under WIDTH machines at once.
#
# WIDTH copies of the test image boot together on the built kernel, each
# with a raw IDE disk of 64 MiB of zeros made in DIR, and each runs
# `calibprobe compute` and then `calibprobe storage` from the debug shell.
# The machines wait at a barrier before each part, so the measured work
# starts together across the width; the harness stamps the host's
# monotonic clock when it sends a command and when the part's line
# arrives, and the part's time on a machine is the difference, which is
# never less than the work took, since output the harness reads late is
# stamped late. The measurement of a part is the slowest machine's, since
# a set's deadline must hold for the slowest worker.
#
# Prints one line, `compute=S storage=S width=W`, seconds to a thousandth, and
# exits 1 with a reason when a machine did not finish. The calibration's
# own deadlines are not scaled: this script runs before any factor exists.
# The kernel, the image and the emulator are those of the architecture ARCH
# names.
set -u
[ $# -eq 2 ] || { echo "usage: calibrate.sh WIDTH DIR" >&2; exit 2; }
WIDTH=$1; DIR=$2
REPO=$(cd "$(dirname "$0")/../.." && pwd)
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
. "$REPO/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU
HARNESS="$REPO/build/tests/system/harness"
KERNEL="$ARCH_BUILD/rach2/kernel"
IMAGE="$ARCH_BUILD/images/test.krffs"
for f in "$HARNESS" "$KERNEL" "$IMAGE"; do
    [ -e "$f" ] || { echo "calibrate: $f is not built" >&2; exit 1; }
done
mkdir -p "$DIR" || exit 1
rm -f "$DIR"/go1 "$DIR"/go2 "$DIR"/stamp.* "$DIR"/log.*
unset RACH2_DEADLINES RACH2_DEADLINE_FACTOR
dd if=/dev/zero of="$DIR/raw.img" bs=1048576 count=64 2>/dev/null || exit 1

PIDS=""
i=0
while [ "$i" -lt "$WIDTH" ]; do
    cp "$DIR/raw.img" "$DIR/raw.$i.img" 2>/dev/null || exit 1
    "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" --append "rach2.diskdiag" \
        --qemu-arg=-cpu --qemu-arg=max \
        --drive "file=$DIR/raw.$i.img,format=raw,if=ide,index=0,media=disk" \
        --timeout 900 --steptmo 900 --ok "CALIB STORAGE DONE" --never "CALIBPROBE FAIL" \
        --log "$DIR/log.$i" --stamp "$DIR/stamp.$i" \
        "expect=Type 'help'" "barrier=$DIR/go1" "send=exec /calibprobe compute" \
        "expect=CALIB COMPUTE DONE" "expect=Type 'help'" "barrier=$DIR/go2" \
        "send=exec /calibprobe storage" "expect=CALIB STORAGE DONE" > /dev/null 2>&1 &
    PIDS="$PIDS $!"
    i=$((i + 1))
done

# Every machine's harness, waited for one by one: a machine that printed
# its lines and then failed is a failed calibration, not a measurement.
wait_all() {
    rc=0
    for pid in $PIDS; do
        wait "$pid" || rc=1
    done
    return "$rc"
}
alive_all() {
    for pid in $PIDS; do
        kill -0 "$pid" 2>/dev/null || return 1
    done
    return 0
}

# Release a barrier once every machine has stamped the match before it,
# within the boots' own deadline by the clock, and not while a machine has
# died on the way, since the rest would then wait out their deadlines.
release() { # release MATCH-NUMBER FILE
    until=$(( $(date +%s) + 900 ))
    while [ "$(date +%s)" -lt "$until" ]; do
        n=$(cat "$DIR"/stamp.* 2>/dev/null | grep -c "^MATCH $1 ")
        [ "$n" -ge "$WIDTH" ] && { : > "$2"; return 0; }
        alive_all || { echo "calibrate: a machine ended before step $1" >&2; return 1; }
        sleep 0.2
    done
    echo "calibrate: only $n of $WIDTH machines reached step $1" >&2
    return 1
}
stop_all() {
    for pid in $PIDS; do
        kill "$pid" 2>/dev/null
    done
    wait_all
}
release 1 "$DIR/go1" || { stop_all; exit 1; }
release 5 "$DIR/go2" || { stop_all; exit 1; }
wait_all || { echo "calibrate: a machine's harness failed" >&2; exit 1; }

# Each machine's parts: compute is MATCH 4 less SEND 3, storage MATCH 8
# less SEND 7; the slowest of each.
awk -v width="$WIDTH" '
    { split(FILENAME, f, "."); m = f[length(f)]; t[m, $1, $2] = $3; seen[m] = 1 }
    END {
        n = 0
        for (m in seen) {
            if (!((m, "MATCH", 4) in t) || !((m, "MATCH", 8) in t)) { print "calibrate: machine " m " did not finish" > "/dev/stderr"; exit 1 }
            c = t[m, "MATCH", 4] - t[m, "SEND", 3]; s = t[m, "MATCH", 8] - t[m, "SEND", 7]
            if (c > cmax) cmax = c; if (s > smax) smax = s; n++
        }
        if (n != width) { print "calibrate: " n " of " width " machines reported" > "/dev/stderr"; exit 1 }
        printf "compute=%.3f storage=%.3f width=%d\n", cmax, smax, width
    }' "$DIR"/stamp.*
