#!/bin/sh
# vtest-guest.sh: the Debian guest that serves virglrenderer's vtest over
# TCP on QEMU's user network, and the check that draws a triangle through
# bed/vtestrelay.c to it.
#
#   vtest-guest.sh prepare
#   vtest-guest.sh run [-c CPUS] [-m MIB] COMMAND
#   vtest-guest.sh check [-c CPUS] [-m MIB]
#
# `prepare` makes the prepared image, an overlay of the cloud image that
# tests/graphics/debian-ref/image pins, which debian-ref.sh fetches and
# checks: one boot with the seed, whose `network-config` is empty, runs
# setup.sh.in, which installs `packages` from the snapshot that file names,
# adds the services that serve vtest on the guest's port 5555 and turns
# cloud-init off. `run` boots a new overlay of the prepared image with CPUS
# processors (4) and MIB of memory (4096), its port 5555 forwarded from a
# free port on the host's loopback address, waits for the guest to report
# its server ready on the serial console, runs COMMAND in sh on the host with
# VTEST_HOST and VTEST_PORT set to that address, stops the guest and removes
# the overlay. It writes the guest's record, its versions, QEMU's and the
# accelerator, to standard error as `vtest-guest: record:` lines, and exits
# with COMMAND's status.
#
# `check` builds the relay and triangle.c into build/bed with CC (cc),
# CFLAGS and LDLIBS (-lEGL -lGLESv2), draws the triangle on the host's
# llvmpipe, then through the relay to the guest with Mesa's virpipe, and
# passes when both pictures check and their checksums are equal.
#
# The images are in BED_VTEST_CACHE, by default build/cache/bed-vtest of the
# clone whose worktree this is, so that its worktrees share one prepared
# image. ACCEL chooses the accelerator as rach2/qemu_devices.sh says: KVM
# where /dev/kvm opens.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../.." && pwd)
common=$(git -C "$tree" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || echo "$tree/.git")
cache=${BED_VTEST_CACHE:-$(dirname "$common")/build/cache/bed-vtest}
prepared=$cache/prepared.qcow2
pins=$tree/tests/graphics/debian-ref/image
guest_port=5555

die()
{
    echo "vtest-guest: $*" >&2
    exit 1
}

pin()
{
    awk -v n="$1" '$1 == n { print $2 }' "$pins"
}

free_port()
{
    python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])'
}

# machine CPUS MIB OVERLAY: set QEMU's words for the guest in `words`,
# sourcing qemu_devices.sh for the accelerator and the network.
machine()
{
    # shellcheck disable=SC2034 # qemu_devices.sh reads them
    DISPLAY_PROFILE=std-vga DISPLAY_MODE=none ARCH=x86_64 AUDIO_DEVICE=none
    # shellcheck disable=SC2034 # qemu_devices.sh reads them
    NIC=virtio-net-pci NET_HOSTFWD="hostfwd=tcp:127.0.0.1:$port-:$guest_port"
    # shellcheck source=../../rach2/qemu_devices.sh
    . "$tree/rach2/qemu_devices.sh"
    words="$ACCEL_OPTS -smp $1 -m ${2}M -machine q35 -nodefaults -no-reboot"
    words="$words -drive file=$3,if=virtio,format=qcow2 $NET_OPTS -device virtio-rng-pci"
    words="$words $DISPLAY_OPTS"
}

# watch PID LOG MARK QUIET: wait while QEMU's PID runs and its serial LOG
# grows, until the log holds MARK or has not grown for QUIET seconds.
# Succeed when the log holds MARK.
watch()
{
    last=-1 still=0
    while kill -0 "$1" 2>/dev/null; do
        grep -aq "$3" "$2" 2>/dev/null && break
        sleep 2
        size=$(wc -c < "$2" 2>/dev/null || echo 0)
        if [ "$size" -eq "$last" ]; then
            still=$((still + 2))
            [ "$still" -lt "$4" ] || { echo "vtest-guest: no console output for $4 s" >&2; break; }
        else
            last=$size still=0
        fi
    done
    grep -aq "$3" "$2"
}

# base_image: set `base` to the pinned cloud image, which debian-ref.sh
# fetches and checks and names in its result line.
base_image()
{
    base=$(sh "$tree/tests/graphics/debian-ref/debian-ref.sh" fetch |
        sed -n 's/^debian-ref: result: \(.*\) checks$/\1/p')
    [ -f "$base" ] || die "debian-ref.sh fetch named no checked image"
}

prepare()
{
    base_image
    command -v xorriso >/dev/null || die "xorriso makes the seed; install it"
    mkdir -p "$cache"
    work=$cache/prepare
    rm -rf "$work"
    mkdir -p "$work/seed"
    qemu=
    trap 'kill "$qemu" 2>/dev/null || true; rm -rf "$work"' EXIT
    sed -e "s|@SNAPSHOT@|$(pin snapshot)|" -e "s|@PORT@|$guest_port|g" \
        -e "s|@PACKAGES@|$(grep -v '^#' "$here/packages" | tr '\n' ' ')|" \
        "$here/setup.sh.in" > "$work/setup.sh"
    sed -e "s|@SETUP@|$(base64 -w0 < "$work/setup.sh")|" "$here/user-data.in" > "$work/seed/user-data"
    cp "$here/meta-data" "$here/network-config" "$work/seed/"
    xorriso -as mkisofs -quiet -o "$work/seed.iso" -V cidata -J -r "$work/seed"
    rm -f "$prepared"
    qemu-img create -q -f qcow2 -F qcow2 -b "$base" "$work/prepared.qcow2" 8G
    port=$(free_port)
    machine 4 4096 "$work/prepared.qcow2"
    echo "vtest-guest: preparing under $ACCEL_OPTS; console in $work/console.log"
    # shellcheck disable=SC2086 # words
    qemu-system-x86_64 $words -drive file="$work/seed.iso",if=virtio,format=raw,readonly=on \
        -serial file:"$work/console.log" -monitor none &
    qemu=$!
    watch "$qemu" "$work/console.log" 'vtest-guest: \(prepared\|failed\)' 1200 || true
    wait "$qemu" || true
    qemu=
    grep -a 'vtest-guest: ' "$work/console.log" | tr -d '\r' | sed 's/^.*vtest-guest:/vtest-guest:/'
    grep -aq 'vtest-guest: prepared' "$work/console.log" || die "result: preparation failed"
    mv "$work/prepared.qcow2" "$prepared"
    chmod a-w "$prepared"
    echo "vtest-guest: result: prepared $prepared"
}

# boot CPUS MIB: boot an overlay of the prepared image in a new work
# directory and wait for its server; set `qemu`, `work` and `port`.
boot()
{
    [ -f "$prepared" ] || die "no prepared image; run vtest-guest.sh prepare"
    mkdir -p "$tree/build/tmp"
    work=$(mktemp -d "${TMPDIR:-$tree/build/tmp}/vtest-guest.XXXXXX")
    qemu=
    trap 'kill "$qemu" 2>/dev/null || true; rm -rf "$work"' EXIT
    qemu-img create -q -f qcow2 -F qcow2 -b "$prepared" "$work/run.qcow2"
    port=$(free_port)
    machine "$1" "$2" "$work/run.qcow2"
    # shellcheck disable=SC2086 # words
    qemu-system-x86_64 $words -serial file:"$work/console.log" -monitor none &
    qemu=$!
    watch "$qemu" "$work/console.log" 'vtest-guest: \(ready\|failed\)' 300 || true
    if ! grep -aq 'vtest-guest: ready' "$work/console.log"; then
        die "the guest's server is not ready: $(grep -a 'vtest-guest: ' "$work/console.log" | tail -n 3)"
    fi
    {
        grep -a 'vtest-guest: version: ' "$work/console.log" | tr -d '\r' | sed 's/^.*vtest-guest: version: /guest /'
        echo "accelerator $ACCEL"
        echo "cpus $1"
        echo "memory_mib $2"
        echo "qemu $(qemu-system-x86_64 --version | head -n 1)"
        echo "image $(basename "$(pin url)") sha512 $(pin sha512)"
        echo "snapshot $(pin snapshot)"
    } | sed 's/^/vtest-guest: record: /' >&2
}

# halt: stop the guest, whose overlay is thrown away, and remove the work
# directory.
halt()
{
    kill "$qemu" 2>/dev/null || true
    wait "$qemu" 2>/dev/null || true
    qemu=
    rm -rf "$work"
    trap - EXIT
}

# options ARGS: read -c and -m into `cpus` and `mib`; set `shifted` to the
# number of words they took.
options()
{
    cpus=4 mib=4096 OPTIND=1
    while getopts c:m: o; do
        case $o in
            c) cpus=$OPTARG ;;
            m) mib=$OPTARG ;;
            *) exit 2 ;;
        esac
    done
    shifted=$((OPTIND - 1))
}

run()
{
    options "$@"
    shift "$shifted"
    [ $# -gt 0 ] || die "run: no command"
    boot "$cpus" "$mib"
    status=0
    VTEST_HOST=127.0.0.1 VTEST_PORT=$port sh -c "$*" || status=$?
    halt
    return "$status"
}

# draw NAME DRIVER: draw the triangle with Mesa's DRIVER into
# $out/NAME.ppm, its output into $out/NAME.out, which goes to standard error
# too. Return the triangle's status.
draw()
{
    status=0
    LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=$2 "$out/triangle" -o "$out/$1.ppm" > "$out/$1.out" 2>&1 ||
        status=$?
    sed "s/^/vtest-guest: $1: /" "$out/$1.out" >&2
    return "$status"
}

# checksum NAME: print the checksum of the triangle NAME drew.
checksum()
{
    sed -n 's/^triangle: checksum: //p' "$out/$1.out"
}

check()
{
    options "$@"
    out=$tree/build/bed
    mkdir -p "$out"
    # shellcheck disable=SC2086 # the flags are words
    ${CC:-cc} ${CFLAGS:--O2} -o "$out/vtestrelay" "$here/../vtestrelay.c"
    # shellcheck disable=SC2086 # the flags are words
    ${CC:-cc} ${CFLAGS:--O2} -o "$out/triangle" "$here/triangle.c" ${LDLIBS:--lEGL -lGLESv2}
    draw llvmpipe llvmpipe || die "result: the llvmpipe triangle failed"
    want=$(checksum llvmpipe)
    boot "$cpus" "$mib"
    sock=$work/vtest.sock
    "$out/vtestrelay" -1 -v -s "$sock" 127.0.0.1 "$port" 2> "$out/relay.out" &
    relay=$!
    trap 'kill "$relay" "$qemu" 2>/dev/null || true; rm -rf "$work"' EXIT
    tries=0
    until [ -S "$sock" ]; do
        tries=$((tries + 1))
        [ "$tries" -lt 10 ] || die "the relay did not listen"
        sleep 1
    done
    drew=0
    # A variable set before a function's name is unspecified in POSIX sh.
    VTEST_SOCKET_NAME=$sock
    export VTEST_SOCKET_NAME
    draw virpipe virpipe || drew=$?
    relayed=0
    wait "$relay" || relayed=$?
    sed 's/^/vtest-guest: /' "$out/relay.out" >&2
    if [ "$drew" -ne 0 ] || [ "$relayed" -ne 0 ]; then
        grep -a -v 'vtest-guest: ' "$work/console.log" | tr -d '\r' | tail -n 20 |
            sed 's/^/vtest-guest: console: /' >&2
        halt
        die "result: through the relay, the triangle exited $drew and the relay $relayed"
    fi
    halt
    grep -q '^triangle: renderer: virgl' "$out/virpipe.out" ||
        die "result: the virpipe triangle did not draw on virgl"
    got=$(checksum virpipe)
    [ "$got" = "$want" ] || die "result: the checksums differ: llvmpipe $want, through the relay $got"
    echo "vtest-guest: result: the triangle through the relay equals llvmpipe's, $got"
}

case ${1:-} in
    prepare) prepare ;;
    run) shift; run "$@" ;;
    check) shift; check "$@" ;;
    *)
        echo "usage: vtest-guest.sh prepare | run [-c CPUS] [-m MIB] COMMAND | check [-c CPUS] [-m MIB]" >&2
        exit 2
        ;;
esac
