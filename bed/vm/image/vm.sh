#!/bin/sh
# vm.sh: the Debian VM that serves Rach2's shared-memory GPU through
# bed/vm/backend, and the check that drives it from bed/shm/peer.c over
# QEMU's ivshmem-doorbell.
#
#   vm.sh prepare [-n]
#   vm.sh check [-V | -b] [-c CPUS] [-m MIB]
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
# intel-iommu in place of the module. With -b, once the peer has drawn,
# the VM's memory is taken until MemAvailable reads 0 and held there for
# 30 s, and the peer then asks for eight backings of 64 MiB of it in turn,
# for resources it made before, and for a texture of 4 MiB, which the
# backend counts as 16 MiB, 16 bytes a texel being the most of any format
# but those of 64-bit channels: it must refuse each with ERR_OUT_OF_MEMORY, where a pin of memory the VM has
# not got, or a texture the GPU's driver would fill, would wake its OOM
# killer, and answer the commands after. Once the fill has ended, the backend is moved into a
# memory cgroup of 64 MiB and the peer asks for a backing of 1 GiB, which
# the VM's counts allow and the cgroup cannot hold: the backend must refuse
# it when its pin's allocation fails, where the cgroup's OOM killer would
# end the backend.
#
# `rach2` makes rach2.qcow2 beside the prepared image, an overlay of it that
# Rach2 boots under its own KVM (tests/cases/machine.bed-shm.json): the
# tree's module and backend built in the VM and installed, and a service
# that at each boot loads the module and runs the backend surfaceless on
# the ivshmem device, `bed-vm: ` before each of its lines on the console,
# counting for bed/measure/profile.sh when QEMU gives the firmware entry
# opt/rach2/vgpu-profile, and at its end, once it has killed what the
# backend left of the service, `bed-vm: backend: ended: status S` with the
# kernel's lines of a kill or a crash; one of bed-vm-memory, which writes
# `bed-vm: memory:` lines there as the VM's memory moves, through the
# backend's end too; and one that does to the backend the words Rach2's
# test writes to the VM's second serial port; no getty on the console,
# whose hangup of the tty would silence them; gfx takes the development
# key, and perf is there. With -o it also writes the image
# whole as the raw disk RAW, the form etude's overlays take as their base.
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

# fill: the VM's part of -b, as a script for its sh: a service that takes
# anonymous memory until MemAvailable reads 0 and keeps it there, each
# piece as large as MemAvailable less 64 MiB, the last ones 1 MiB past it,
# and is killed 30 s on by timeout(1), so that the kernel gives the memory
# back without the VM's programs, which a VM held so cannot run; its
# oom_score_adj of 1000 makes it the OOM killer's first victim. The script
# returns once MemAvailable has first read 0 and prints how many pieces
# that took.
fill()
{
    cat <<'FILL'
set -eu
command -v python3 > /dev/null || { echo "bed-vm: fill: no python3" >&2; exit 1; }
sudo rm -f /run/bed-fill.zero
sudo tee /run/bed-fill.py > /dev/null <<'HOG'
import mmap, time
def avail():
    with open("/proc/meminfo") as f:
        for line in f:
            if line.startswith("MemAvailable:"):
                return int(line.split()[1])
with open("/proc/self/oom_score_adj", "w") as f:
    f.write("1000")
held, chunk = [], b"\1" * (1 << 20)
while True:
    a = avail()
    if a <= 0:
        try:
            open("/run/bed-fill.zero", "x").write(str(len(held)))
        except FileExistsError:
            pass
        time.sleep(0.1)
        continue
    n = (a - 65536) // 1024 if a > 131072 else a // 1024 + 1
    m = mmap.mmap(-1, n << 20)
    for i in range(n):
        m[i << 20:(i + 1) << 20] = chunk
    held.append(m)
HOG
sudo systemd-run --quiet --unit=bed-fill /usr/bin/timeout -s KILL 30 python3 /run/bed-fill.py
n=600
until [ -e /run/bed-fill.zero ] || [ "$n" -eq 0 ]; do
    sleep 0.1
    n=$((n - 1))
done
echo "bed-vm: fill: MemAvailable read 0 after $(cat /run/bed-fill.zero 2>/dev/null || echo none) pieces"
FILL
}

# peer_waits FILE: wait, 600 s at most, until the peer waits for FILE.
peer_waits()
{
    n=600
    until grep -q "^peer: backing: waiting for $1\$" "$work/peer.out" 2>/dev/null; do
        n=$((n - 1))
        [ "$n" -gt 0 ] || return 1
        sleep 1
    done
}

# pen: the VM's part of the last ask, as a script for its sh: once the
# fill has ended, which gives its memory back, move the backend into a new
# memory cgroup that holds 64 MiB, where what it allocates from then on is
# charged, and print MemAvailable.
pen()
{
    cat <<'PEN'
set -eu
while systemctl -q is-active bed-fill; do
    sleep 1
done
cg=/sys/fs/cgroup/bed-pin
sudo mkdir -p "$cg"
echo 64M | sudo tee "$cg/memory.max" > /dev/null
echo 0 | sudo tee "$cg/memory.swap.max" > /dev/null 2>&1 || true
pgrep -x vgpu-backend | sudo tee "$cg/cgroup.procs" > /dev/null
echo "bed-vm: fill: the backend in $cg, $(grep MemAvailable /proc/meminfo)"
PEN
}

# filler: the host's part of -b: once the peer waits for $work/go, fill the
# VM's memory and start the peer's eight asks by making the file; once it
# waits for $work/last, wait for the fill's end, pen the backend in its
# cgroup and start the last ask, which that cgroup cannot hold.
filler()
{
    peer_waits "$work/go" || return 1
    fill | vm 'sh -s' > "$work/fill.out" 2>&1 || true
    : > "$work/go"
    peer_waits "$work/last" || return 1
    pen | vm 'sh -s' >> "$work/fill.out" 2>&1 || true
    : > "$work/last"
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
    if [ "$backing" = 1 ]; then
        expect "MemAvailable read 0 before the backings were asked for" \
            "$(sed -n 's/^bed-vm: fill: MemAvailable read 0 after [0-9]* pieces$/yes/p' "$work/fill.out")" yes
        expect "backings of 64 MiB refused" \
            "$(grep -c '^peer: backing: resource [0-9]*, 67108864 bytes: refused: out of memory$' "$p")" 8
        expect "backings of 64 MiB refused by the VM's counts" \
            "$(grep -c "^backend: back: resource [0-9]*: refused: 67108864 bytes: no room in the VM's counts: " "$b")" 8
        expect "a texture refused while MemAvailable read 0" \
            "$(grep -c '^peer: texture: resource [0-9]*, 1024x1024: refused: out of memory$' "$p")" 1
        expect "the texture refused by the VM's counts" \
            "$(grep -c "^backend: create: resource [0-9]*: refused: 16777216 bytes: no room in the VM's counts$" "$b")" 1
        expect "the backing of 1 GiB refused" \
            "$(grep -c '^peer: backing: resource [0-9]*, 1073741824 bytes: refused: out of memory$' "$p")" 1
        expect "the backend moved into a cgroup of 64 MiB" \
            "$(grep -c '^bed-vm: fill: the backend in /sys/fs/cgroup/bed-pin, MemAvailable:' "$work/fill.out")" 1
        expect "the backing of 1 GiB refused by its pin" \
            "$(grep -c '^backend: back: resource [0-9]*: refused: 1073741824 bytes: the pin: Cannot allocate memory$' "$b")" 1
        expect "kills by the backend's memory cgroup" \
            "$(grep -c 'Memory cgroup out of memory' "$work/dmesg.out")" 0
    fi
    expect "the peer's end" "$(tail -n 1 "$p")" "peer: result: ok"
    [ "$bad" -eq 0 ]
}

check()
{
    vfio=0 cpus=4 mib=4096 backing=0
    while getopts Vbc:m: o; do
        case $o in
            V) vfio=1 ;;
            b) backing=1 ;;
            c) cpus=$OPTARG ;;
            m) mib=$OPTARG ;;
            *) exit 2 ;;
        esac
    done
    [ "$vfio$backing" != 11 ] || die "-b needs the module's pins, which -V does without"
    [ -f "$prepared" ] || die "no prepared image; run vm.sh prepare"
    [ "$(cat "$prepared.key" 2>/dev/null)" = "$(seedkey)" ] ||
        die "the prepared image is not the one this tree's seed makes; run vm.sh prepare"
    out=$tree/build/bed/vm
    mkdir -p "$out" "$tree/build/tmp"
    ${CC:-cc} -std=c99 -O2 -o "$out/peer" "$tree/bed/shm/peer.c"
    "$out/peer" -W "$out/frame.rec"
    work=$(mktemp -d "${TMPDIR:-$tree/build/tmp}/bed-vm.XXXXXX")
    qemu='' peer='' filling=''
    trap 'kill "$qemu" "$peer" "$filling" 2>/dev/null || true; rm -rf "$work"' EXIT
    asks=''
    [ "$backing" = 0 ] || asks="-b 67108864 -g $work/go -B 1073741824 -G $work/last"
    # shellcheck disable=SC2086 # asks are words
    "$out/peer" -S "$work/ivs.sock" -s "$region" -t 600 -r "$out/frame.rec" -o -v $asks \
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
    if [ "$backing" = 1 ]; then
        filler &
        filling=$!
    fi
    status=0
    vm 'sh -s' < "$work/guest.sh" > "$work/backend.out" 2>&1 || status=$?
    # A backend that ran to its end has seen the peer leave.
    [ "$status" -eq 0 ] || kill "$peer" 2>/dev/null || true
    wait "$peer" || true
    peer=
    [ -z "$filling" ] || wait "$filling" || true
    filling=
    vm 'sudo dmesg' </dev/null > "$work/dmesg.out" 2>&1 || true
    vm 'sudo systemctl poweroff' </dev/null >/dev/null 2>&1 || true
    watch "$qemu" "$work/console.log" 'reboot: Power down' 120 || kill "$qemu" 2>/dev/null || true
    wait "$qemu" 2>/dev/null || true
    qemu=
    grep -v '^backend: virgl: \|kmsro: driver missing' "$work/backend.out" | sed 's/^/bed-vm: /' >&2
    [ "$backing" = 0 ] || cat "$work/fill.out" >&2
    # The OOM killer's lines and the call trace of the allocation that woke it.
    grep -i -A 40 'invoked oom-killer' "$work/dmesg.out" | grep -v '^\[ *[0-9.]*\] *\[ *[0-9]*\] ' |
        sed -n '1,/Killed process/p' | sed 's/^/bed-vm: kernel: /' >&2
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
# The services below write the console, ttyS0, and no one logs in there.
# The getty's unit hangs up the tty as it starts, which leaves a descriptor
# opened before it writing nothing; with Type=idle it starts after them.
sudo systemctl mask serial-getty@ttyS0.service
make -s -C bed/vm/module
make -s -C bed/vm/backend
sudo install -m 644 bed/vm/module/vgpushm.ko /usr/local/lib/vgpushm.ko
sudo install -m 755 bed/vm/backend/vgpu-backend /usr/local/sbin/vgpu-backend
# bed/measure/profile.sh reaches the VM as gfx with the development key.
cat krffs/rach2_demo_ed25519.pub >> ~/.ssh/authorized_keys
sudo tee /usr/local/sbin/bed-vm-backend > /dev/null <<'RUN'
#!/bin/sh
# bed-vm-backend: serve the ivshmem device to Rach2 until the VM stops.
# GL and Venus render on the Intel GPU the VM was given, once i915 has made
# its render node: GL on the node, which Mesa's surfaceless EGL finds
# first, and Venus through Mesa's Intel Vulkan drivers alone. Without one,
# GL renders on llvmpipe and Venus on lavapipe.
# A restart through bed-vm-control finds the module loaded.
grep -q '^vgpushm ' /proc/modules || insmod /usr/local/lib/vgpushm.ko || exit 1
soft=1
for d in /sys/bus/pci/devices/*; do
    [ "$(cat "$d/vendor")" = 0x8086 ] || continue
    case $(cat "$d/class") in 0x03*) ;; *) continue ;; esac
    i=0
    while ! ls "$d"/drm/renderD* > /dev/null 2>&1 && [ "$i" -lt 60 ]; do
        sleep 1
        i=$((i + 1))
    done
    icds=$(ls /usr/share/vulkan/icd.d/intel_*icd*.json 2>/dev/null | tr '\n' : | sed 's/:$//')
    if ls "$d"/drm/renderD* > /dev/null 2>&1; then
        soft=''
        echo "bed-vm: gpu: ${d##*/}: GL on $(ls "$d"/drm | grep renderD)" > /dev/ttyS0
    fi
    if ls "$d"/drm/renderD* > /dev/null 2>&1 && [ -n "$icds" ]; then
        export VK_DRIVER_FILES="$icds"
        echo "bed-vm: gpu: ${d##*/} on $(readlink "$d/driver" | sed 's|.*/||'), Venus on $icds" > /dev/ttyS0
    else
        echo "bed-vm: gpu: ${d##*/}: no render node after 60 s or no Intel driver; Venus on lavapipe" > /dev/ttyS0
    fi
    break
done
[ -z "$soft" ] || export LIBGL_ALWAYS_SOFTWARE=1
# QEMU's firmware entry opt/rach2/vgpu-profile, which bed-shm.sh gives with
# BED_VM_PROFILE_PORT, has the backend count into /run/vgpu-profile, and
# read back each flush with its CRC when the entry is 1. The second serial
# port, which only Rach2's test of the backend's loss gives the VM, arms the
# hold that its row of a long command asks for.
set --
modprobe qemu_fw_cfg 2> /dev/null || :
cfg=/sys/firmware/qemu_fw_cfg/by_name/opt/rach2/vgpu-profile/raw
if [ -r "$cfg" ]; then
    set -- -T /run/vgpu-profile
    [ "$(cat "$cfg")" != 1 ] || set -- "$@" -C
    echo "bed-vm: profile: $*" > /dev/ttyS0
fi
! stty -F /dev/ttyS1 > /dev/null 2>&1 || set -- "$@" -H
# The entry opt/rach2/vrend-debug, which bed-shm.sh gives with
# BED_VM_VREND_DEBUG, is virglrenderer's VREND_DEBUG: `khr` logs each GL
# error with the driver's own message, as `backend: virgl: error: ERROR:`.
cfg=/sys/firmware/qemu_fw_cfg/by_name/opt/rach2/vrend-debug/raw
if [ -r "$cfg" ]; then
    export VREND_DEBUG="$(cat "$cfg")"
    echo "bed-vm: vrend-debug: $VREND_DEBUG" > /dev/ttyS0
fi
# The backend's lines reach the console through sed, over a FIFO that the
# backend and what it starts, the render server and its workers, hold open.
# The backend's end alone ends the service: once it has ended, every other
# process of the service's cgroup but this shell and sed is killed, which
# frees every object they held and closes their ends of the FIFO, and then
# the end's lines go to sed, which ends when this shell closes its own.
fifo=/run/bed-vm-backend.$$
rm -f "$fifo"
mkfifo -m 600 "$fifo" || exit 1
sed -u 's/^/bed-vm: /' < "$fifo" > /dev/ttyS0 &
out=$!
exec 3> "$fifo"
rm -f "$fifo"
/usr/local/sbin/vgpu-backend -v "$@" >&3 2>&3
status=$?
# Kill until the cgroup holds none of them, a process forked meanwhile and
# one that is dying included; a pid read that has gone is passed over.
procs=/sys/fs/cgroup$(sed -n 's/^0:://p' /proc/self/cgroup)/cgroup.procs
left=''
while :; do
    alive=0
    for pid in $(cat "$procs"); do
        [ "$pid" != $$ ] && [ "$pid" != "$out" ] || continue
        kill -KILL "$pid" 2> /dev/null || continue
        alive=$((alive + 1))
    done
    [ -n "$left" ] || left=$alive
    [ "$alive" -gt 0 ] || break
    sleep 0.1
done
# How the backend ended, 128 and the signal's number for a kill, and the
# kernel's lines of a kill or a crash, which the console's log level hides.
echo "backend: ended: status $status" >&3
echo "backend: ended: $left of its processes left, killed and gone" >&3
dmesg | grep -E 'invoked oom-killer|Out of memory|Killed process|segfault at|traps: ' |
    sed 's/^/kernel: /' >&3
exec 3>&-
wait "$out"
RUN
sudo tee /usr/local/sbin/bed-vm-memory > /dev/null <<'RUN'
#!/bin/sh
# bed-vm-memory: the VM's memory on the console, read once a second and
# written as a line only when MemAvailable or the bytes of i915's objects
# with pages have moved 64 MiB or more since the last line, so that a run's
# least MemAvailable and most i915 memory are on record while a steady VM
# writes nothing: each byte of the console is an exit to QEMU. A line gives
# the least MemAvailable and the most i915 memory of the seconds since the
# line before; `i915 none` without i915's debugfs file. A line is written
# also when i915's file comes or goes.
step=65536
gem=''
shown=-1 shown_gem=0 shown_i915='' least='' most=0
while :; do
    avail=0
    while read -r name value _; do
        if [ "$name" = MemAvailable: ]; then
            avail=$value
            break
        fi
    done < /proc/meminfo
    # i915's file, sought until found, since i915 may bind after the
    # sampler starts, and again when it has gone with its device.
    if [ -z "$gem" ] || [ ! -r "$gem" ]; then
        gem=''
        for f in /sys/kernel/debug/dri/*/i915_gem_objects; do
            if [ -r "$f" ]; then
                gem=$f
                break
            fi
        done
    fi
    # Linux's i915_gem_object_info: "N shrinkable [F free] objects, B bytes".
    objects='' bytes=0
    [ -z "$gem" ] || read -r objects _ _ _ _ bytes _ < "$gem" || objects=''
    case $avail$bytes in *[!0-9]*) avail=0 bytes=0 objects='' ;; esac
    kib=$((bytes / 1024))
    if [ -z "$least" ] || [ "$avail" -lt "$least" ]; then least=$avail; fi
    [ "$kib" -le "$most" ] || most=$kib
    moved=$((avail - shown))
    gem_moved=$((kib - shown_gem))
    i915=${objects:+y}
    if [ "$shown" -lt 0 ] || [ "$moved" -ge "$step" ] || [ "$moved" -le "-$step" ] ||
        [ "$gem_moved" -ge "$step" ] || [ "$gem_moved" -le "-$step" ] ||
        [ "$i915" != "$shown_i915" ]; then
        if [ -n "$objects" ]; then
            echo "bed-vm: memory: available $avail kB, least $least kB; i915 $objects objects, $kib kB, most $most kB"
        else
            echo "bed-vm: memory: available $avail kB, least $least kB; i915 none"
        fi
        shown=$avail shown_gem=$kib shown_i915=$i915 least=$avail most=$kib
    fi
    sleep 1
done
RUN
sudo chmod 755 /usr/local/sbin/bed-vm-backend /usr/local/sbin/bed-vm-memory
# The sampler is a service of its own, so that it goes on through the
# backend's end and restart, and its lines show what the end gave back.
sudo tee /etc/systemd/system/bed-vm-memory.service > /dev/null <<'UNIT'
[Unit]
Description=Write the VM's memory on the console as it moves
After=bed-vm-ready.service
[Service]
Type=simple
ExecStart=/bin/sh -c 'exec /usr/local/sbin/bed-vm-memory > /dev/ttyS0 2>&1'
[Install]
WantedBy=multi-user.target
UNIT
sudo systemctl enable bed-vm-memory.service
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
sudo tee /usr/local/sbin/bed-vm-control > /dev/null <<'RUN'
#!/bin/sh
# bed-vm-control: do to the backend each word a line of the VM's second
# serial port, ttyS1, brings, as Rach2's test of the backend's loss writes
# them: `stop`, `cont` and `kill` send it SIGSTOP, SIGCONT and SIGKILL,
# `hold` sends it SIGUSR2, which has it hold the next new backing 45 s,
# and `refuse` SIGWINCH, which has it refuse the next new backing, both
# under the -H that bed-vm-backend gives it when this port is there, and
# `start` starts its service again. Each done is said on the console as
# `bed-vm: control: WORD: done`. A VM without the port has nothing to do.
stty -F /dev/ttyS1 raw -echo 2> /dev/null || exit 0
exec < /dev/ttyS1
stty raw -echo
while read -r word; do
    case $word in
        stop) pkill -STOP -x vgpu-backend ;;
        cont) pkill -CONT -x vgpu-backend ;;
        kill) pkill -KILL -x vgpu-backend ;;
        hold) pkill -USR2 -x vgpu-backend ;;
        refuse) pkill -WINCH -x vgpu-backend ;;
        start) systemctl restart bed-vm-backend.service ;;
        *) echo "bed-vm: control: $word: unknown" > /dev/ttyS0; continue ;;
    esac
    echo "bed-vm: control: $word: done" > /dev/ttyS0
done
RUN
sudo chmod 755 /usr/local/sbin/bed-vm-control
sudo tee /etc/systemd/system/bed-vm-control.service > /dev/null <<'UNIT'
[Unit]
Description=Do to the backend what Rach2's test asks on ttyS1
After=bed-vm-ready.service
[Service]
Type=simple
ExecStart=/usr/local/sbin/bed-vm-control
[Install]
WantedBy=multi-user.target
UNIT
sudo systemctl enable bed-vm-control.service
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
    (cd "$tree" && tar cf - bed/vm/module bed/vm/backend rach2/sys/virtio/vgpushm.h \
        krffs/rach2_demo_ed25519.pub) |
        vm 'rm -rf bed krffs && tar xf -'
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
        echo "usage: vm.sh prepare [-n] | check [-V | -b] [-c CPUS] [-m MIB] | rach2 [-o RAW]" >&2
        exit 2
        ;;
esac
