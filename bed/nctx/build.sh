#!/bin/sh
# build.sh: build the bed's matched native-context stack in the Debian 13
# builder guest of tests/graphics/debian-ref, and make the VM image's variant.
#
#   build.sh fetch      download the tarballs that `pins` names and check them
#   build.sh mesa       build Mesa 26.1.6 with Intel's virtio path
#   build.sh virgl      build virglrenderer 1.3.0 with the i915 renderer
#   build.sh qemu       build Debian's QEMU 11.1.2 against that virglrenderer
#   build.sh image      make nctx.qcow2: the backports kernel and QEMU
#                       packages and the three builds on the prepared
#                       reference image
#   build.sh check      boot nctx.qcow2 and print what it holds
#   build.sh run CMD    boot nctx.qcow2 and run CMD in it, in a copy of this
#                       directory
#   build.sh alone      make nctx-alone.qcow2, a compressed copy of
#                       nctx.qcow2 with no backing file, and SHA256SUMS of
#                       the three builds, to move to another host
#   build.sh rehearse   83-6b's session.sh all in a new overlay of the
#                       prepared image, which has no i915, with
#                       nctx-alone.qcow2 as its second disk
#   build.sh reverse    build the shared-memory backend and module in an
#                       overlay of nctx-alone.qcow2; export a standalone
#                       reverse.qcow2, its kernel and its initrd
#   build.sh all        all of the above, in that order
#
# The tarballs, the builds (mesa-nctx.tar.xz, virglrenderer-nctx.tar.xz and
# qemu-nctx.tar.xz, each with its .log) and the image are in NCTX_CACHE, by default build/cache/nctx
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

# boot DISK [OPTS]: start the guest from DISK, with QEMU's OPTS added, with the std-vga profile that
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
        -nodefaults -no-reboot -drive file="$1",if=virtio,format=qcow2 ${2:-} \
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

# build NAME STEP INPUT: build in a new overlay of the prepared image from
# INPUT, a file of the cache, and bring NAME-nctx.tar.xz and its log out.
build()
{
    session
    tarball=$cache/$3
    [ -f "$tarball" ] || die "$tarball: missing; run build.sh fetch or the build that makes it"
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
    for f in mesa virglrenderer qemu; do
        [ -f "$cache/$f-nctx.tar.xz" ] || die "$f-nctx.tar.xz: missing; build it first"
    done
    rm -f "$cache/nctx.qcow2"
    qemu-img create -q -f qcow2 -F qcow2 -b "$ref/prepared.qcow2" "$work/nctx.qcow2" 32G
    boot "$work/nctx.qcow2"
    echo "nctx: making the image; console in $work/console.log"
    put "$here/guest.sh" "$here/pins" "$work/snapshot" "$here/qemu-nctx" "$here/nctx-env" \
        "$cache/mesa-nctx.tar.xz" "$cache/virglrenderer-nctx.tar.xz" "$cache/qemu-nctx.tar.xz"
    in_guest "sh guest.sh image mesa-nctx.tar.xz virglrenderer-nctx.tar.xz qemu-nctx.tar.xz" > "$work/image.log"
    sed 's/^/nctx: /' "$work/image.log"
    in_guest "cd && rm -rf nctx"
    halt
    mv "$work/nctx.qcow2" "$cache/nctx.qcow2"
    chmod a-w "$cache/nctx.qcow2"
    echo "nctx: result: $cache/nctx.qcow2"
}

# variant CMD: run CMD in ~/nctx of a new overlay of nctx.qcow2, with this
# directory's files copied in, and print its output.
variant()
{
    session
    [ -f "$cache/nctx.qcow2" ] || die "no nctx.qcow2; run build.sh image"
    qemu-img create -q -f qcow2 -F qcow2 -b "$cache/nctx.qcow2" "$work/run.qcow2"
    boot "$work/run.qcow2"
    put "$here/guest.sh" "$here/pins" "$work/snapshot"
    status=0
    in_guest "$1" > "$work/run.log" || status=$?
    sed 's/^/nctx: /' "$work/run.log"
    halt
    return "$status"
}

alone()
{
    [ -f "$cache/nctx.qcow2" ] || die "no nctx.qcow2; run build.sh image"
    rm -f "$cache/nctx-alone.qcow2"
    qemu-img convert -c -O qcow2 "$cache/nctx.qcow2" "$cache/nctx-alone.qcow2.part"
    mv "$cache/nctx-alone.qcow2.part" "$cache/nctx-alone.qcow2"
    chmod a-w "$cache/nctx-alone.qcow2"
    (cd "$cache" && sha256sum mesa-nctx.tar.xz virglrenderer-nctx.tar.xz qemu-nctx.tar.xz > SHA256SUMS &&
        sha256sum nctx-alone.qcow2 > nctx-alone.qcow2.sha256)
    echo "nctx: result: $cache/nctx-alone.qcow2, $(wc -c < "$cache/nctx-alone.qcow2") bytes"
}

# rehearse: session.sh all with REHEARSE=1 as root in a new overlay of the
# prepared image, Debian 13 with Linux 6.12 as on the Lenovo, with
# nctx-alone.qcow2 as its second disk, /dev/vdb; the session's output is
# kept as rehearsal.tar in the cache.
rehearse()
{
    session
    for f in nctx-alone.qcow2 SHA256SUMS; do
        [ -f "$cache/$f" ] || die "$f: missing; run build.sh alone"
    done
    qemu-img create -q -f qcow2 -F qcow2 -b "$ref/prepared.qcow2" "$work/host.qcow2" 32G
    boot "$work/host.qcow2" "-drive file=$cache/nctx-alone.qcow2,if=virtio,format=raw,readonly=on"
    echo "nctx: rehearsing 83-6b's session; console in $work/console.log"
    cp "$key" "$work/id_ed25519"
    put "$here/session.sh" "$here/guest.sh" "$here/nctxcheck.c" "$here/pins" "$work/snapshot" \
        "$here/qemu-nctx" "$here/nctx-env" "$cache/SHA256SUMS" "$work/id_ed25519" \
        "$cache/mesa-nctx.tar.xz" "$cache/virglrenderer-nctx.tar.xz" "$cache/qemu-nctx.tar.xz"
    status=0
    in_guest "sudo REHEARSE=1 sh session.sh all . /dev/vdb" || status=$?
    # shellcheck disable=SC2086 # opts are words
    ssh $opts -p "$port" gfx@127.0.0.1 'sudo tar cf - -C /srv/nctx out' < /dev/null > "$cache/rehearsal.tar"
    in_guest "sudo sh session.sh leave"
    halt
    echo "nctx: result: status $status, output in $cache/rehearsal.tar"
    return "$status"
}

# reverse: build the VM that serves the reversed native-context path.
# The standalone base and its SSH key are also the inputs kept on the Mac
# for the Linux reference; no product image is changed.
reverse()
{
    session
    [ -f "$cache/nctx-alone.qcow2" ] || die "no nctx-alone.qcow2; run build.sh alone"
    [ ! -f "$cache/id_ed25519" ] || key=$cache/id_ed25519
    qemu-img create -q -f qcow2 -F qcow2 -b "$cache/nctx-alone.qcow2" "$work/reverse.qcow2"
    boot "$work/reverse.qcow2"
    echo "nctx: building the reversed VM; console in $work/console.log"
    put "$here/reverse-vm.sh" "$here/pins" "$work/snapshot"
    # shellcheck disable=SC2086 # opts are words
    (cd "$tree" && tar cf - bed/vm/module bed/vm/backend rach2/sys/virtio/vgpushm.h) |
        ssh $opts -p "$port" gfx@127.0.0.1 'mkdir -p nctx/build/source && tar xf - -C nctx/build/source'
    in_guest 'sudo sh reverse-vm.sh prepare build/source'
    for name in vmlinuz initrd.img; do
        # shellcheck disable=SC2086 # opts are words
        ssh $opts -p "$port" gfx@127.0.0.1 "sudo cat /$name" < /dev/null > "$cache/reverse-$name.part"
        mv "$cache/reverse-$name.part" "$cache/reverse-$name"
    done
    halt
    qemu-img convert -O qcow2 "$work/reverse.qcow2" "$cache/reverse.qcow2.part"
    mv "$cache/reverse.qcow2.part" "$cache/reverse.qcow2"
    (cd "$cache" && sha256sum reverse.qcow2 reverse-vmlinuz reverse-initrd.img mesa-nctx.tar.xz \
        > reverse.sha256)
    echo "nctx: result: $cache/reverse.qcow2, reverse-vmlinuz, reverse-initrd.img and reverse.sha256"
}

case ${1:-} in
    fetch) fetch ;;
    mesa) build mesa mesa "$(basename "$(pin mesa_url)")" ;;
    virgl) build virglrenderer virgl "$(basename "$(pin virgl_url)")" ;;
    qemu) build qemu qemu virglrenderer-nctx.tar.xz ;;
    image) image ;;
    check) variant 'sh guest.sh check' ;;
    run) shift; variant "$*" ;;
    alone) alone ;;
    rehearse) rehearse ;;
    reverse) reverse ;;
    all)
        fetch
        (build mesa mesa "$(basename "$(pin mesa_url)")")
        (build virglrenderer virgl "$(basename "$(pin virgl_url)")")
        (build qemu qemu virglrenderer-nctx.tar.xz)
        (image)
        variant 'sh guest.sh check'
        ;;
    *)
        echo "usage: build.sh fetch | mesa | virgl | qemu | image | check | run CMD | alone | rehearse | reverse | all" >&2
        exit 2
        ;;
esac
