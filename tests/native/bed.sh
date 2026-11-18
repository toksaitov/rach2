#!/bin/sh
# bed.sh: the test bed's rungs through a VM on the Lenovo's GPU, as the
# native cases bed/* run 83-7's step in the release's boot.
#
#   bed.sh shm SUMS
#   bed.sh forward virgl|venus SUMS
#
# Run as root by etude-guest in the case's workspace, in a mount namespace of
# the case's own, on the GNU root with its KVM layer and the Intel driver
# bound. The workspace holds the runners (tests/kvm/bed-shm.sh,
# tests/assign/gpu.sh, bed/forward/forward.sh), vgshmprobe, the harness
# (bed/measure) with the graphics runners it uses, and 82-4b's check.sh with
# the source of 78-10d's crcmodes, which the root's cc, TinyCC, builds here
# with -static, as it requires of every program (tcc(1)). The VMs'
# disks, which only an x86-64 Linux host builds, are on the data partition,
# /dev/disk0p6, in native/bed: vm.disk, the bed's VM raw, and guest.qcow2,
# guest.tar and id_ed25519, the forward guest's disk, archive and key. SUMS,
# tests/kvm/ref/lenovo-bed.sha256, has the SHA-256 of each. The partition is
# mounted read-only; QEMU opens each disk with snapshot=on and writes nothing
# to it.
#
#   shm      bed-shm.sh -w -g 0000:00:02.0: the session on the region's
#            virtio-gpu card drawing through the VM's i915 on the GPU, each
#            scene's figure on the rungs shm-virgl and shm-venus, glmark2 as
#            the load, the VM killed under it, the session headless on
#            llvmpipe and back on Rach2's driver (83-7.5's shm rows, 83-7.6)
#   forward  forward.sh's glmark2 scene in the Debian guest of QEMU on
#            the render node check.sh finds for i915, through virgl or
#            Venus (83-7.5's forward rows)
#
# Before the run check.sh reads the GPU's state and the panel's pipe CRC,
# and after it reads them again: the driver i915, the same CRC, Crocus and
# both readbacks. The desktop's session is started again after each check.
# The last line is `bed: result: pass` or `fail: why`.
set -eu

here=$(pwd)
data=/dev/disk0p6
gpu=0000:00:02.0
pidfile=/run/rach2-desktop.pid
socket=/run/user/1001/wayland-0
mounted=''

fail()
{
    echo "bed: result: fail: $*"
    exit 1
}

# shellcheck disable=SC2329 # the exit trap runs it
cleanup()
{
    [ -z "$mounted" ] || umount "$mounted" 2> /dev/null || umount -l "$mounted" 2> /dev/null || :
    session_start || :
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# session_start: have the desktop's supervisor start the session again, as
# check.sh ends it, and wait for its socket, 600 tries of 0.1 s at most.
session_start()
{
    [ -S "$socket" ] && pgrep -x weston > /dev/null && return 0
    [ -f "$pidfile" ] || return 1
    kill -HUP "$(cat "$pidfile")"
    tries=0
    until [ -S "$socket" ] && pgrep -x weston > /dev/null; do
        tries=$((tries + 1))
        [ "$tries" -lt 600 ] || return 1
        sleep 0.1
    done
}

case ${1:-}:$# in
    shm:2) rung=shm sums=$2 ;;
    forward:3) rung=forward profile=$2 sums=$3 ;;
    *)
        echo "usage: bed.sh shm SUMS | bed.sh forward virgl|venus SUMS" >&2
        exit 2
        ;;
esac
sums=$(cd "$(dirname "$sums")" && pwd)/$(basename "$sums")
[ -f "$sums" ] || fail "no $sums"

# The kit: the runners, the harness and its runners apart, crcmodes built.
rm -rf kit out
mkdir -p kit/measure/runners out
for f in bed-shm.sh gpu.sh forward.sh check.sh; do
    cp "$f" kit/
done
for f in measure.sh piglit.txt latency.c webgl.html; do
    cp "$f" kit/measure/
done
for f in common.sh games.sh piglit.sh; do
    cp "$f" kit/measure/runners/
done
cc -std=c99 -static -O2 -o kit/crcmodes crcmodes.c > crcmodes.build 2>&1 ||
    fail "crcmodes did not build: $(tail -n 3 crcmodes.build)"
chmod -R a+rX kit

# The disks, checked in place on the partition mounted read-only.
mkdir -p data
mount -t ext2 -o ro "$data" data || fail "$data would not mount read-only"
mounted=$here/data
bed=$here/data/native/bed
(cd "$bed" && grep -v '^#' "$sums" | sha256sum -c) > disks.check 2>&1 || {
    cat disks.check
    fail "a disk differs from $sums"
}

sh kit/check.sh before > check-before.txt 2>&1 || :
cat check-before.txt
grep -q '^check: before: result: pass' check-before.txt || fail "check.sh before did not pass"
want=$(sed -n 's/^check: before: crcmodes: mode 1366x768 aspect crc \(0x[0-9a-f]*\).*/\1/p' check-before.txt | head -n 1)
session_start || fail "the session did not start after check.sh"

status=0
case $rung in
    shm)
        # The game scene is deferred until milestone 86 (#5259).
        MEASURE_RUNNERS=$here/kit/measure/runners sh kit/bed-shm.sh -d "$bed/vm.disk" \
            -p ./vgshmprobe -w -x "$here/kit/measure" -S 'glmark2 piglit venus:glmark2' \
            -g "$gpu" -o "$here/out" > run.out 2>&1 || status=$?
        cat run.out
        grep -q '^bed-shm: result: pass' run.out || status=1
        ;;
    forward)
        cp "$bed/id_ed25519" key && chmod 600 key
        if [ "$profile" = venus ]; then
            export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/intel_hasvk_icd.json
        fi
        # i915's render node as check.sh found it, since a card that has
        # not gone yet may hold renderD128.
        node=$(sed -n 's/^check: before: driver i915, card[0-9]*, \(renderD[0-9]*\)$/\1/p' check-before.txt)
        RENDER_NODE=/dev/dri/${node:-renderD128} sh kit/forward.sh -o "$here/out" -f qcow2 \
            -d "$bed/guest.qcow2" -k key -g "$bed/guest.tar" -M glmark2 "$profile" > run.out 2>&1 ||
            status=$?
        cat run.out
        ;;
esac

sh kit/check.sh after > check-after.txt 2>&1 || :
cat check-after.txt
session_start || fail "the session did not start after check.sh"
[ "$status" = 0 ] || fail "the $rung run failed, status $status"
grep -q '^check: after: result: pass' check-after.txt || fail "check.sh after did not pass"
grep -q "crcmodes: mode 1366x768 aspect crc ${want:-none}" check-after.txt ||
    fail "the pipe's CRC is not ${want:-none}, as before the run"
echo "bed: result: pass"
