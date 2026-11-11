#!/bin/sh
# vm.sh: the Debian VM that serves Rach2's shared-memory GPU through
# bed/vm/backend, and the check that drives it from bed/shm/peer.c over
# QEMU's ivshmem-doorbell.
#
#   vm.sh prepare [-n]
#   vm.sh check [-V] [-c CPUS] [-m MIB]
#   vm.sh rach2 [-o RAW]
#
# `prepare` makes the prepared image, an overlay of the cloud image that
# tests/graphics/debian-ref/image pins, which debian-ref.sh fetches and
# checks: one boot with the seed, whose `network-config` is empty, runs
# setup.sh.in, which installs `packages` from the snapshot that file names,
# rebuilds Debian's virglrenderer with GBM allocation, builds vkms with
# vkms.patch, readies the kernel for vfio, adds the service that writes
# `bed-vm: ready` to the serial console at each boot and turns cloud-init
# off. The image's key, the SHA-256 of the seed's files and the pins, is kept
# beside it; with -n `prepare` does nothing when an image of the same key
# exists.
#
# `check` builds the peer into build/bed/vm, starts it as QEMU's ivshmem
# server with a region of 32 MiB, boots a new overlay of the prepared image
# with CPUS processors (4) and MIB of memory (4096) and the region on an
# ivshmem-doorbell of two vectors, builds the tree's module and backend in
# the VM, and runs the backend on vkms with its writeback, direct scanout
# and Venus's import probe while the peer draws: a 2D image, then the
# recorded display frame, which it reads back, shows whole and in part, and
# hands to a Venus context. It passes when each frame the writeback copied
# has the CRC the peer expects of it, the import's CRC is the frame's, the
# frame was scanned out directly once, and the doorbells woke both sides.
# With -V the backend takes the device through vfio-pci under QEMU's
# intel-iommu in place of the module.
#
# `rach2` makes rach2.qcow2 beside the prepared image, an overlay of it that
# Rach2 boots under its own KVM (tests/cases/machine.bed-shm.json): the
# tree's module and backend built in the VM and installed, and a service
# that at each boot loads the module and runs the backend surfaceless on
# the ivshmem device, `bed-vm: ` before each of its lines on the console.
# With -o it also writes the image whole as the raw disk RAW, the form
# etude's overlays take as their base.
#
# The images and the account's key are in BED_VM_CACHE, by default
# build/cache/bed-vm of the clone whose worktree this is, so that its
# worktrees share one prepared image. ACCEL chooses the accelerator as
# rach2/qemu_devices.sh says: KVM where /dev/kvm opens.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../../.." && pwd)
common=$(git -C "$tree" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || echo "$tree/.git")
cache=${BED_VM_CACHE:-$(dirname "$common")/build/cache/bed-vm}
prepared=$cache/prepared.qcow2
key=$cache/id_ed25519
pins=$tree/tests/graphics/debian-ref/image
region=33554432

die()
{
    echo "bed-vm: $*" >&2
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

# seedkey: the key of the image that the seed's files and the pins make.
seedkey()
{
    sha256sum "$here/setup.sh.in" "$here/packages" "$here/user-data.in" "$here/meta-data" \
        "$here/network-config" "$here/vkms.patch" "$pins" | awk '{ print $1 }' |
        sha256sum | awk '{ print $1 }'
}

# machine CPUS MIB OVERLAY: set QEMU's words for the VM in `words`, sourcing
# qemu_devices.sh for the accelerator and the network.
machine()
{
    # shellcheck disable=SC2034 # qemu_devices.sh reads them
    DISPLAY_PROFILE=std-vga DISPLAY_MODE=none ARCH=x86_64 AUDIO_DEVICE=none
    # shellcheck disable=SC2034 # qemu_devices.sh reads them
    NIC=virtio-net-pci NET_HOSTFWD="hostfwd=tcp:127.0.0.1:$port-:22" MEMORY=${2}M
    # shellcheck source=../../../rach2/qemu_devices.sh
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
            [ "$still" -lt "$4" ] || { echo "bed-vm: no console output for $4 s" >&2; break; }
        else
            last=$size still=0
        fi
    done
    grep -aq "$3" "$2"
}

prepare()
{
    if [ "${1:-}" = -n ] && [ -f "$prepared" ] &&
        [ "$(cat "$prepared.key" 2>/dev/null)" = "$(seedkey)" ]; then
        return 0
    fi
    base=$(sh "$tree/tests/graphics/debian-ref/debian-ref.sh" fetch |
        sed -n 's/^debian-ref: result: \(.*\) checks$/\1/p')
    [ -f "$base" ] || die "debian-ref.sh fetch named no checked image"
    command -v xorriso >/dev/null || die "xorriso makes the seed; install it"
    mkdir -p "$cache"
    [ -f "$key" ] || ssh-keygen -q -t ed25519 -N '' -C bed-vm -f "$key"
    work=$cache/prepare
    rm -rf "$work"
    mkdir -p "$work/seed"
    qemu=
    trap 'kill "$qemu" 2>/dev/null || true; rm -rf "$work"' EXIT
    sed -e "s|@SNAPSHOT@|$(pin snapshot)|" \
        -e "s|@PACKAGES@|$(grep -v '^#' "$here/packages" | tr '\n' ' ')|" \
        -e "s|@VKMS_PATCH@|$(base64 -w0 < "$here/vkms.patch")|" \
        "$here/setup.sh.in" > "$work/setup.sh"
    sed -e "s|@KEY@|$(cat "$key.pub")|" -e "s|@SETUP@|$(base64 -w0 < "$work/setup.sh")|" \
        "$here/user-data.in" > "$work/seed/user-data"
    cp "$here/meta-data" "$here/network-config" "$work/seed/"
    xorriso -as mkisofs -quiet -o "$work/seed.iso" -V cidata -J -r "$work/seed"
    rm -f "$prepared" "$prepared.key"
    qemu-img create -q -f qcow2 -F qcow2 -b "$base" "$work/prepared.qcow2" 16G
    port=$(free_port)
    machine 8 8192 "$work/prepared.qcow2"
    echo "bed-vm: preparing under $ACCEL_OPTS; console in $work/console.log"
    # shellcheck disable=SC2086 # words
    qemu-system-x86_64 $words -drive file="$work/seed.iso",if=virtio,format=raw,readonly=on \
        -serial file:"$work/console.log" -monitor none &
    qemu=$!
    watch "$qemu" "$work/console.log" 'bed-vm: \(prepared\|failed\)' 1800 || true
    wait "$qemu" || true
    qemu=
    grep -a 'bed-vm: ' "$work/console.log" | tr -d '\r' | sed 's/^.*bed-vm:/bed-vm:/'
    grep -aq 'bed-vm: prepared' "$work/console.log" || die "result: preparation failed"
    mv "$work/prepared.qcow2" "$prepared"
    chmod a-w "$prepared"
    seedkey > "$prepared.key"
    echo "bed-vm: result: prepared $prepared"
}

# vm COMMAND: run COMMAND in sh as the user gfx in the VM.
vm()
{
    # shellcheck disable=SC2086 # opts are words
    ssh $opts -p "$port" gfx@127.0.0.1 "$@"
}

# guest VFIO: the VM's part of the check, as a script for its sh: build the
# module and the backend from the tree's copy in ~/bed, take the device by
# the module, or by vfio-pci when VFIO is 1, and run the backend on vkms
# until the driver detaches.
guest()
{
    cat <<GUEST
set -eu
cd ~
make -s -C bed/vm/module
make -s -C bed/vm/backend
card=
for c in /sys/class/drm/card[0-9]*; do
    # vkms's card hangs from a platform device of its own name.
    case \$(readlink -f "\$c/device") in
        */vkms) card=/dev/dri/\${c##*/} ;;
    esac
done
[ -n "\$card" ] || { echo "bed-vm: vkms has no card" >&2; exit 1; }
from=
if [ $1 = 1 ]; then
    bdf=\$(basename "\$(dirname "\$(grep -l 0x1110 /sys/bus/pci/devices/*/device)")")
    sudo modprobe vfio-pci
    echo vfio-pci | sudo tee "/sys/bus/pci/devices/\$bdf/driver_override" > /dev/null
    echo "\$bdf" | sudo tee /sys/bus/pci/drivers_probe > /dev/null
    from="-V \$bdf"
else
    sudo insmod bed/vm/module/vgpushm.ko
fi
sudo LIBGL_ALWAYS_SOFTWARE=1 bed/vm/backend/vgpu-backend -1 -k "\$card" -w -d -I \$from
GUEST
}

# expect WHAT GOT WANT: say whether GOT equals WANT, and count a difference.
expect()
{
    if [ -n "$2" ] && [ "$2" = "$3" ]; then
        echo "bed-vm: $1: $2"
    else
        echo "bed-vm: $1: ${2:-none}, expected ${3:-none}" >&2
        bad=$((bad + 1))
    fi
}

# judge: compare the backend's output in $work/backend.out with the peer's
# in $work/peer.out; succeed when every check holds.
judge()
{
    bad=0
    b=$work/backend.out p=$work/peer.out
    want=$(sed -n 's/^peer: flush: sent crc //p; s/^peer: show: .*, crc //p' "$p" | tr '\n' ' ')
    got=$(sed -n 's/^backend: scanout: writeback .*, crc //p' "$b" | tr '\n' ' ')
    expect "frames written back" "$got" "$want"
    expect "Venus's import" "$(sed -n 's/^backend: venus: imported .*, crc //p' "$b")" \
        "$(sed -n 's/^peer: readback: resource .*, 1024x768, crc \([0-9a-f]*\).*/\1/p' "$p")"
    expect "direct scanouts" "$(sed -n 's/^backend: result: kms .* direct \([0-9]*\),.*/\1/p' "$b")" 1
    wakes=$(sed -n 's/^backend: result: .* wakes \([0-9]*\)+.*/\1/p' "$b")
    expect "backend woken by the queue's doorbell" "$([ "${wakes:-0}" -gt 0 ] && echo yes)" yes
    woken=$(sed -n 's/^peer: doorbells: rang [0-9]*, woken //p' "$p")
    expect "peer woken by the VM's doorbell" "$([ "${woken:-0}" -gt 0 ] && echo yes)" yes
    expect "the peer's end" "$(tail -n 1 "$p")" "peer: result: ok"
    [ "$bad" -eq 0 ]
}

check()
{
    vfio=0 cpus=4 mib=4096
    while getopts Vc:m: o; do
        case $o in
            V) vfio=1 ;;
            c) cpus=$OPTARG ;;
            m) mib=$OPTARG ;;
            *) exit 2 ;;
        esac
    done
    [ -f "$prepared" ] || die "no prepared image; run vm.sh prepare"
    [ "$(cat "$prepared.key" 2>/dev/null)" = "$(seedkey)" ] ||
        die "the prepared image is not the one this tree's seed makes; run vm.sh prepare"
    out=$tree/build/bed/vm
    mkdir -p "$out" "$tree/build/tmp"
    ${CC:-cc} -std=c99 -O2 -o "$out/peer" "$tree/bed/shm/peer.c"
    "$out/peer" -W "$out/frame.rec"
    work=$(mktemp -d "${TMPDIR:-$tree/build/tmp}/bed-vm.XXXXXX")
    qemu='' peer=''
    trap 'kill "$qemu" "$peer" 2>/dev/null || true; rm -rf "$work"' EXIT
    "$out/peer" -S "$work/ivs.sock" -s "$region" -t 600 -r "$out/frame.rec" -o -v \
        > "$work/peer.out" 2>&1 &
    peer=$!
    tries=0
    until [ -S "$work/ivs.sock" ]; do
        tries=$((tries + 1))
        [ "$tries" -lt 10 ] || die "the peer did not listen"
        sleep 1
    done
    qemu-img create -q -f qcow2 -F qcow2 -b "$prepared" "$work/run.qcow2"
    port=$(free_port)
    machine "$cpus" "$mib" "$work/run.qcow2"
    if [ "$vfio" = 1 ]; then
        # Before the device it translates; without interrupt remapping, which
        # QEMU's ivshmem cannot route through (bed/vm/Readme.md).
        words="$words -device intel-iommu,intremap=off,caching-mode=on"
    fi
    words="$words -chardev socket,path=$work/ivs.sock,id=ivs"
    words="$words -device ivshmem-doorbell,chardev=ivs,vectors=2"
    # shellcheck disable=SC2086 # words
    qemu-system-x86_64 $words -serial file:"$work/console.log" -monitor none &
    qemu=$!
    watch "$qemu" "$work/console.log" 'bed-vm: ready' 300 ||
        die "the VM is not ready: $(tr -d '\r' < "$work/console.log" | tail -n 5)"
    opts="-i $key -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
    tries=0
    until vm true 2>/dev/null </dev/null; do
        tries=$((tries + 1))
        [ "$tries" -lt 60 ] || die "SSH did not answer in 60 tries"
        sleep 1
    done
    tr -d '\r' < "$work/console.log" | sed -n 's/^.*bed-vm: version: /bed-vm: record: /p' >&2
    {
        echo "accelerator $ACCEL"
        echo "qemu $(qemu-system-x86_64 --version | head -n 1)"
        echo "image $(basename "$(pin url)") sha512 $(pin sha512)"
        echo "snapshot $(pin snapshot)"
        echo "device $([ "$vfio" = 1 ] && echo vfio-pci || echo vgpushm)"
    } | sed 's/^/bed-vm: record: /' >&2
    (cd "$tree" && tar cf - bed/vm/module bed/vm/backend rach2/sys/virtio/vgpushm.h) |
        vm 'rm -rf bed && tar xf -'
    guest "$vfio" > "$work/guest.sh"
    status=0
    vm 'sh -s' < "$work/guest.sh" > "$work/backend.out" 2>&1 || status=$?
    # A backend that ran to its end has seen the peer leave.
    [ "$status" -eq 0 ] || kill "$peer" 2>/dev/null || true
    wait "$peer" || true
    peer=
    vm 'sudo systemctl poweroff' </dev/null >/dev/null 2>&1 || true
    watch "$qemu" "$work/console.log" 'reboot: Power down' 120 || kill "$qemu" 2>/dev/null || true
    wait "$qemu" 2>/dev/null || true
    qemu=
    grep -v '^backend: virgl: \|kmsro: driver missing' "$work/backend.out" | sed 's/^/bed-vm: /' >&2
    sed 's/^/bed-vm: /' "$work/peer.out" >&2
    [ "$status" -eq 0 ] || die "result: the backend exited $status"
    judge || die "result: the VM's frames differ from the peer's"
    echo "bed-vm: result: the VM drew and showed the peer's frames"
}

# The VM's part of `rach2`, as a script for its sh: build the module and the
# backend from the tree's copy in ~/bed, install them, and enable the
# service that runs the backend at each boot. Under Rach2's KVM on the
# testing PC the VM runs three hypervisors deep, where each exit crosses
# them all and the guest runs some ten times slower than systemd's defaults
# allow for: so it waits on no clock to start a unit or find a device, has
# no service watchdogs, keeps its console quiet but for errors, whose every
# byte is an exit to QEMU, and loads no AppArmor profiles.
baked()
{
    cat <<'GUEST'
set -eu
cd ~
sudo mkdir -p /etc/systemd/system.conf.d /etc/default/grub.d
printf '[Manager]\nDefaultTimeoutStartSec=infinity\nDefaultDeviceTimeoutSec=infinity\n' |
    sudo tee /etc/systemd/system.conf.d/rach2.conf > /dev/null
printf '%s\n' 'GRUB_TIMEOUT=0' \
    'GRUB_CMDLINE_LINUX_DEFAULT="$GRUB_CMDLINE_LINUX_DEFAULT quiet loglevel=3 apparmor=0 systemd.service_watchdogs=0"' |
    sudo tee /etc/default/grub.d/99-rach2.cfg > /dev/null
sudo update-grub
make -s -C bed/vm/module
make -s -C bed/vm/backend
sudo install -m 644 bed/vm/module/vgpushm.ko /usr/local/lib/vgpushm.ko
sudo install -m 755 bed/vm/backend/vgpu-backend /usr/local/sbin/vgpu-backend
sudo tee /usr/local/sbin/bed-vm-backend > /dev/null <<'RUN'
#!/bin/sh
# bed-vm-backend: serve the ivshmem device to Rach2 until the VM stops.
insmod /usr/local/lib/vgpushm.ko || exit 1
LIBGL_ALWAYS_SOFTWARE=1 /usr/local/sbin/vgpu-backend -v 2>&1 | sed -u 's/^/bed-vm: /' > /dev/ttyS0
RUN
sudo chmod 755 /usr/local/sbin/bed-vm-backend
sudo tee /etc/systemd/system/bed-vm-backend.service > /dev/null <<'UNIT'
[Unit]
Description=Serve Rach2's virtio-gpu from the ivshmem device
After=bed-vm-ready.service
ConditionPathExists=/sys/bus/pci/devices
[Service]
Type=simple
ExecStart=/usr/local/sbin/bed-vm-backend
[Install]
WantedBy=multi-user.target
UNIT
sudo systemctl enable bed-vm-backend.service
GUEST
}

rach2()
{
    raw=''
    while getopts o: o; do
        case $o in
            o) raw=$OPTARG ;;
            *) exit 2 ;;
        esac
    done
    [ -f "$prepared" ] || die "no prepared image; run vm.sh prepare"
    [ "$(cat "$prepared.key" 2>/dev/null)" = "$(seedkey)" ] ||
        die "the prepared image is not the one this tree's seed makes; run vm.sh prepare"
    mkdir -p "$tree/build/tmp"
    work=$(mktemp -d "${TMPDIR:-$tree/build/tmp}/bed-vm.XXXXXX")
    qemu=''
    trap 'kill "$qemu" 2>/dev/null || true; rm -rf "$work" ${raw:+"$raw.tmp"}' EXIT
    qemu-img create -q -f qcow2 -F qcow2 -b "$prepared" "$work/rach2.qcow2"
    port=$(free_port)
    machine 4 4096 "$work/rach2.qcow2"
    # shellcheck disable=SC2086 # words
    qemu-system-x86_64 $words -serial file:"$work/console.log" -monitor none &
    qemu=$!
    watch "$qemu" "$work/console.log" 'bed-vm: ready' 300 ||
        die "the VM is not ready: $(tr -d '\r' < "$work/console.log" | tail -n 5)"
    opts="-i $key -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
    tries=0
    until vm true 2>/dev/null </dev/null; do
        tries=$((tries + 1))
        [ "$tries" -lt 60 ] || die "SSH did not answer in 60 tries"
        sleep 1
    done
    (cd "$tree" && tar cf - bed/vm/module bed/vm/backend rach2/sys/virtio/vgpushm.h) |
        vm 'rm -rf bed && tar xf -'
    baked > "$work/baked.sh"
    vm 'sh -s' < "$work/baked.sh" || die "result: the backend did not build"
    vm 'sudo systemctl poweroff' </dev/null >/dev/null 2>&1 || true
    watch "$qemu" "$work/console.log" 'reboot: Power down' 120 || kill "$qemu" 2>/dev/null || true
    wait "$qemu" 2>/dev/null || true
    qemu=
    mv "$work/rach2.qcow2" "$cache/rach2.qcow2"
    if [ -n "$raw" ]; then
        qemu-img convert -O raw "$cache/rach2.qcow2" "$raw.tmp"
        mv "$raw.tmp" "$raw"
    fi
    echo "bed-vm: result: made $cache/rach2.qcow2${raw:+ and $raw}"
}

case ${1:-} in
    prepare) shift; prepare "$@" ;;
    check) shift; check "$@" ;;
    rach2) shift; rach2 "$@" ;;
    *)
        echo "usage: vm.sh prepare [-n] | check [-V] [-c CPUS] [-m MIB] | rach2 [-o RAW]" >&2
        exit 2
        ;;
esac
