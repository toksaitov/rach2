#!/bin/sh
# build.sh: build the bed's matched native-context stack in the Debian 13
# builder guest of tests/graphics/debian-ref, and make the VM image's variant.
#
#   build.sh fetch      download the tarballs that `pins` names and check them
#   build.sh mesa       build Mesa 26.1.6 with Intel's virtio path
#   build.sh virgl      build virglrenderer 1.3.0 with the i915 renderer
#   build.sh image      make nctx.qcow2: the backports kernel, QEMU 11.1 and
#                       both builds on the prepared reference image
#   build.sh check      boot nctx.qcow2 and print what it holds
#   build.sh all        all of the above, in that order
#
# The tarballs, the builds (mesa-nctx.tar.xz, virglrenderer-nctx.tar.xz, each
# with its .log) and the image are in NCTX_CACHE, by default build/cache/nctx
# of the clone whose worktree this is. The builder is a new overlay of
# debian-ref's prepared image for each build, removed afterwards; `prepare`
# that image first. ACCEL chooses the accelerator as rach2/qemu_devices.sh
# says: KVM where /dev/kvm opens.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../.." && pwd)
common=$(git -C "$tree" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || echo "$tree/.git")
cache=${NCTX_CACHE:-$(dirname "$common")/build/cache/nctx}
ref=${DEBIAN_REF_CACHE:-$(dirname "$common")/build/cache/debian-ref}
key=$ref/id_ed25519
cpus=${NCTX_CPUS:-16}
mib=${NCTX_MIB:-16384}

die()
{
    echo "nctx: $*" >&2
    exit 1
}

pin()
{
    awk -v n="$1" '$1 == n { print $2 }' "$here/pins"
}

snapshot()
{
    awk '$1 == "snapshot" { print $2 }' "$tree/tests/graphics/debian-ref/image"
}

fetch()
{
    mkdir -p "$cache"
    for p in mesa virgl; do
        url=$(pin "${p}_url")
        f=$cache/$(basename "$url")
        if [ ! -f "$f" ]; then
            echo "nctx: fetching $url"
            curl -sSfL -o "$f.part" "$url"
            mv "$f.part" "$f"
        fi
        [ "$(wc -c < "$f")" -eq "$(pin "${p}_size")" ] || die "$f: not $(pin "${p}_size") bytes"
        echo "$(pin "${p}_sha256")  $f" | sha256sum -c --quiet - || die "$f: SHA-256 differs"
        echo "nctx: result: $f checks"
    done
}

free_port()
{
    python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])'
}

# boot DISK: start the guest from DISK, with the std-vga profile that
# Debian's GRUB needs under SeaBIOS, and wait for SSH. Sets `qemu`, `port`
# and `opts`; the caller's EXIT trap stops QEMU.
boot()
{
    # shellcheck disable=SC2034 # qemu_devices.sh reads them
    DISPLAY_PROFILE=std-vga DISPLAY_MODE=none ARCH=x86_64 AUDIO_DEVICE=none
    port=$(free_port)
    # shellcheck disable=SC2034 # qemu_devices.sh reads them
    NIC=virtio-net-pci NET_HOSTFWD="hostfwd=tcp:127.0.0.1:$port-:22"
    # shellcheck source=../../rach2/qemu_devices.sh
    . "$tree/rach2/qemu_devices.sh"
    # shellcheck disable=SC2086 # the options are words
    qemu-system-x86_64 $ACCEL_OPTS -smp "$cpus" -m "${mib}M" -machine q35 \
        -nodefaults -no-reboot -drive file="$1",if=virtio,format=qcow2 \
        $NET_OPTS -device virtio-rng-pci $DISPLAY_OPTS \
        -serial file:"$work/console.log" -monitor none &
    qemu=$!
    opts="-i $key -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
    tries=0
    # shellcheck disable=SC2086 # opts are words
    until ssh $opts -o ConnectTimeout=5 -p "$port" gfx@127.0.0.1 true 2>/dev/null </dev/null; do
        kill -0 "$qemu" 2>/dev/null || die "QEMU ended before SSH answered: $(tail -c 400 "$work/console.log")"
        tries=$((tries + 1))
        [ "$tries" -lt 120 ] || die "SSH did not answer in 120 tries"
        sleep 1
    done
}

# in CMD: run CMD in the guest's ~/nctx as gfx.
in_guest()
{
    # shellcheck disable=SC2086 # opts are words
    ssh $opts -p "$port" gfx@127.0.0.1 "cd nctx && $*" </dev/null
}

# put FILE...: copy FILEs into the guest's ~/nctx.
put()
{
    # shellcheck disable=SC2086 # opts are words
    ssh $opts -p "$port" gfx@127.0.0.1 'mkdir -p nctx' </dev/null
    # shellcheck disable=SC2086 # opts are words
    scp -q $opts -P "$port" "$@" gfx@127.0.0.1:nctx/
}

# get FILE...: copy the guest's ~/nctx/FILEs into the cache.
get()
{
    for f in "$@"; do
        # shellcheck disable=SC2086 # opts are words
        scp -q $opts -P "$port" "gfx@127.0.0.1:nctx/$f" "$cache/$f.part"
        mv "$cache/$f.part" "$cache/$f"
    done
}

# halt: power the guest off and wait for QEMU to end.
halt()
{
    # shellcheck disable=SC2086 # opts are words
    ssh $opts -p "$port" gfx@127.0.0.1 'sudo systemctl poweroff' </dev/null 2>/dev/null || true
    wait "$qemu" 2>/dev/null || true
    qemu=
}

# session: a scratch directory and an EXIT trap that stops QEMU and removes it.
session()
{
    [ -f "$ref/prepared.qcow2" ] || die "no prepared image; run tests/graphics/debian-ref/debian-ref.sh prepare"
    mkdir -p "$cache"
    work=$(mktemp -d "${TMPDIR:-$tree/build/tmp}/nctx.XXXXXX")
    qemu=
    trap 'kill "$qemu" 2>/dev/null || true; rm -rf "$work"' EXIT
    snapshot > "$work/snapshot"
}

# build NAME STEP: build in a new overlay of the prepared image and bring
# NAME-nctx.tar.xz and its log out.
build()
{
    session
    tarball=$cache/$(basename "$(pin "${2}_url")")
    [ -f "$tarball" ] || die "$tarball: missing; run build.sh fetch"
    qemu-img create -q -f qcow2 -F qcow2 -b "$ref/prepared.qcow2" "$work/build.qcow2" 32G
    boot "$work/build.qcow2"
    echo "nctx: building $1 in the builder guest; console in $work/console.log"
    put "$here/guest.sh" "$here/pins" "$work/snapshot" "$tarball"
    in_guest "sh guest.sh $2 $(basename "$tarball") $1-nctx.tar.xz"
    get "$1-nctx.tar.xz" "$1-nctx.tar.xz.log"
    halt
    sed 's/^/nctx: /' "$cache/$1-nctx.tar.xz.log"
    echo "nctx: result: $cache/$1-nctx.tar.xz"
}

image()
{
    session
    for f in mesa virglrenderer; do
        [ -f "$cache/$f-nctx.tar.xz" ] || die "$f-nctx.tar.xz: missing; build it first"
    done
    rm -f "$cache/nctx.qcow2"
    qemu-img create -q -f qcow2 -F qcow2 -b "$ref/prepared.qcow2" "$work/nctx.qcow2" 32G
    boot "$work/nctx.qcow2"
    echo "nctx: making the image; console in $work/console.log"
    put "$here/guest.sh" "$here/pins" "$work/snapshot" "$here/qemu-nctx" "$here/nctx-env" \
        "$cache/mesa-nctx.tar.xz" "$cache/virglrenderer-nctx.tar.xz"
    in_guest "sh guest.sh image mesa-nctx.tar.xz virglrenderer-nctx.tar.xz" | sed 's/^/nctx: /'
    in_guest "cd && rm -rf nctx"
    halt
    mv "$work/nctx.qcow2" "$cache/nctx.qcow2"
    chmod a-w "$cache/nctx.qcow2"
    echo "nctx: result: $cache/nctx.qcow2"
}

check()
{
    session
    [ -f "$cache/nctx.qcow2" ] || die "no nctx.qcow2; run build.sh image"
    qemu-img create -q -f qcow2 -F qcow2 -b "$cache/nctx.qcow2" "$work/check.qcow2"
    boot "$work/check.qcow2"
    put "$here/guest.sh" "$here/pins" "$work/snapshot"
    in_guest "sh guest.sh check" | sed 's/^/nctx: /'
    halt
}

case ${1:-} in
    fetch) fetch ;;
    mesa) build mesa mesa ;;
    virgl) build virglrenderer virgl ;;
    image) image ;;
    check) check ;;
    all) fetch; (build mesa mesa); (build virglrenderer virgl); (image); check ;;
    *)
        echo "usage: build.sh fetch | mesa | virgl | image | check | all" >&2
        exit 2
        ;;
esac
