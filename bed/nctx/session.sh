#!/bin/sh
# session.sh: 83-6b's check of the native-context stack on a Linux host with
# i915, run as root from a copy of this directory: on the Lenovo's Debian, or
# with REHEARSE=1 in a Debian guest that has no i915, where the guest's GL
# runs on virgl in place of native context.
#
#   session.sh setup DIR        check DIR's files by its SHA256SUMS, unpack
#                               the three builds at /, and record the host
#   session.sh capsets          guest.sh capsets on the GPU's render node
#   session.sh up IMAGE         boot IMAGE, the stack's guest, under the
#                               bed's QEMU and build nctxcheck in it
#   session.sh build            build nctxcheck again in the running guest
#   session.sh check NAME ARGS  nctxcheck NAME ARGS in the guest, with the
#                               host's i915 events counted around it
#   session.sh down             power the guest off
#   session.sh all DIR IMAGE    each of the above in order, every check at
#                               its session size
#   session.sh leave [--now]    stop the guest, remove the builds and the
#                               work, and with --now power off by rtcwake
#
# Each step writes OUT/STEP.txt, OUT being /srv/nctx/out, and prints it.
# GUEST_MIB and GUEST_CPUS (4) size the guest; a check that runs past 900 s
# in the guest is stopped there.
set -u

here=$(cd "$(dirname "$0")" && pwd)
top=/opt/rach2-nctx
W=/srv/nctx
OUT=$W/out
PORT=2290
REHEARSE=${REHEARSE:-0}
# The guest takes 4 GiB on a host of 12 GiB or more, else 2 GiB, which
# leaves the host the memory the pressure check's textures need.
GUEST_MIB=${GUEST_MIB:-$(awk '/MemTotal/ { print ($2 >= 12 * 1048576) ? 4096 : 2048 }' /proc/meminfo)}
GUEST_CPUS=${GUEST_CPUS:-4}
T=/sys/kernel/tracing
SH=/sys/kernel/mm/transparent_hugepage/shmem_enabled
# The i915 events counted around each check: objects made, bindings into
# the GPU's address space, their removal, evictions to make room, the
# shrinker's passes under memory pressure, and requests.
EVENTS="i915_gem_object_create i915_vma_bind i915_vma_unbind i915_gem_evict i915_gem_evict_node i915_gem_evict_vm i915_gem_shrink i915_request_add"

say() { echo "$(date -u +%T) $*"; }

die()
{
    echo "session: $*" >&2
    exit 1
}

# node: the render node of the host's GPU, i915's; vgem's in the rehearsal.
node()
{
    want=i915
    [ "$REHEARSE" = 1 ] && { modprobe vgem; want=vgem; }
    for n in /dev/dri/renderD*; do
        d=$(basename "$(readlink -f "/sys/class/drm/$(basename "$n")/device/driver" 2>/dev/null)")
        case $(readlink -f "/sys/class/drm/$(basename "$n")") in
            */vgem/*) d=vgem ;;
            *) ;;
        esac
        [ "$d" = "$want" ] && { echo "$n"; return 0; }
    done
    return 1
}

# g CMD: CMD in the guest as gfx.
g()
{
    ssh -i "$W/id_ed25519" -o BatchMode=yes -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5 \
        -o ServerAliveInterval=15 -p "$PORT" gfx@127.0.0.1 "$@"
}

setup()
{
    dir=$(cd "$1" && pwd)
    say "setup: from $dir"
    (cd "$dir" && sha256sum -c SHA256SUMS) || die "setup: files differ from SHA256SUMS"
    mkdir -p "$W"
    install -m 600 "$dir/id_ed25519" "$W/id_ed25519"
    for t in mesa virglrenderer qemu; do
        tar -xJf "$dir/$t-nctx.tar.xz" -C / || die "setup: $t-nctx.tar.xz"
    done
    install -m 755 "$here/qemu-nctx" "$top/"
    install -m 644 "$here/nctx-env" "$top/"
    # What the builds need at run time from Debian 13, which the Lenovo's
    # Debian mostly has: QEMU's firmware and libraries, and the capset
    # probe's tools. Mesa, the kernel and Debian's QEMU are held so that no
    # dependency moves the host the earlier references were taken on.
    dpkg-query -W -f '${Package} ${Version}\n' > "$W/packages-before.txt"
    hold=$(dpkg-query -W -f '${db:Status-Abbrev} ${Package}\n' 'linux-image-*' \
        'libgl1-mesa-dri' 'mesa-*' 'libegl-mesa0' 'libgbm1' 'libglx-mesa0' \
        'qemu-system-x86' 2>/dev/null | awk '$1 == "ii" { print $2 }')
    # shellcheck disable=SC2086 # a list of names
    apt-mark hold $hold > /dev/null
    DEBIAN_FRONTEND=noninteractive apt-get -q -y install --no-install-recommends \
        qemu-system-data seabios libepoxy0 libgbm1 libslirp0 libpixman-1-0 \
        libvulkan1 libdrm2 libglib2.0-0t64 libfdt1 cpio busybox-static kmod pciutils \
        > "$W/apt.log" 2>&1 || { tail -n 20 "$W/apt.log"; die "setup: apt-get failed"; }
    # shellcheck disable=SC2086 # a list of names
    apt-mark unhold $hold > /dev/null
    dpkg-query -W -f '${Package} ${Version}\n' > "$W/packages-after.txt"
    echo "packages added or changed:"
    diff "$W/packages-before.txt" "$W/packages-after.txt" | grep '^>' || echo "none"
    missing=$(ldd "$top/qemu/bin/qemu-system-x86_64" | grep 'not found')
    [ -z "$missing" ] || die "setup: QEMU lacks: $missing"
    # The settings of the Linux references in QEMU on the Lenovo (the
    # graphics plan's risk 15): the IOMMU's translation off, which the boot
    # line gives, and shmem's huge pages never.
    cat "$SH" > "$W/shmem-before"
    echo never > "$SH"
    echo "kernel: $(uname -r)"
    echo "cmdline: $(cat /proc/cmdline)"
    echo "shmem_enabled: $(cat "$SH")"
    grep -E 'MemTotal|MemAvailable|SwapTotal' /proc/meminfo
    echo "cpus: $(nproc)"
    lspci -nnk -d ::0300 2>/dev/null
    echo "render node: $(node || echo none)"
    echo "qemu: $("$top/qemu-nctx" --version | head -n 1)"
    echo "qemu: $("$top/qemu-nctx" -device virtio-vga-gl,help 2>&1 | grep -o 'drm_native_context=[^ ]*' || echo 'no drm_native_context')"
    echo "kvm: $([ -w /dev/kvm ] && echo yes || echo no)"
    echo "i915 events: $(for e in $EVENTS; do [ -d "$T/events/i915/$e" ] && printf '%s ' "$e"; done)"
    df -h / "$W" | sed 1d
}

capsets()
{
    n=$(node) || die "capsets: no render node"
    # The DRM renderer's probe logs the GPU's PCI identity at debug.
    VIRGL_LOG_LEVEL=debug HOME=$W sh "$here/guest.sh" capsets "$n"
}

up()
{
    image=$1
    n=$(node) || die "up: no render node"
    nctx=on accel=kvm cpu="-cpu host"
    # The rehearsal's host has no i915, and virglrenderer's start fails
    # whole without it when native context is on (Readme, Design), so its
    # guest runs virgl; a third-level guest runs under TCG.
    [ "$REHEARSE" = 1 ] && { nctx=off; accel=tcg; cpu="-cpu max"; }
    [ -w /dev/kvm ] || { accel=tcg; cpu="-cpu max"; }
    [ ! -s "$W/qemu.pid" ] || ! kill -0 "$(cat "$W/qemu.pid")" 2>/dev/null ||
        die "up: a guest runs already"
    say "up: $image, drm_native_context=$nctx, $accel, render node $n, $GUEST_CPUS CPUs, $GUEST_MIB MiB"
    # Native context needs blob resources and a host-visible window, and
    # blobs need the guest's memory shared. virtio-vga-gl gives Debian's
    # GRUB the VGA it needs under SeaBIOS.
    # shellcheck disable=SC2086 # cpu is words
    VIRGL_LOG_LEVEL=info setsid "$top/qemu-nctx" -accel "$accel" $cpu \
        -smp "$GUEST_CPUS" -m "${GUEST_MIB}M" \
        -object memory-backend-memfd,id=mem,size="${GUEST_MIB}M",share=on \
        -machine q35,memory-backend=mem -nodefaults -no-reboot \
        -drive file="$image",if=virtio,format=qcow2,snapshot=on \
        -netdev user,id=net0,hostfwd=tcp:127.0.0.1:$PORT-:22 \
        -device virtio-net-pci,netdev=net0,romfile= -device virtio-rng-pci \
        -vga none -device virtio-vga-gl,blob=true,hostmem=4G,drm_native_context=$nctx \
        -display egl-headless,rendernode="$n" \
        -serial file:"$W/console.log" -monitor none \
        > "$OUT/qemu.log" 2>&1 < /dev/null &
    echo $! > "$W/qemu.pid"
    t0=$(date +%s)
    limit=600
    [ "$accel" = tcg ] && limit=1800
    until g true 2>/dev/null < /dev/null; do
        kill -0 "$(cat "$W/qemu.pid")" 2>/dev/null ||
            { cat "$OUT/qemu.log"; tail -c 2000 "$W/console.log"; die "up: QEMU ended before SSH answered"; }
        [ $(($(date +%s) - t0)) -lt $limit ] ||
            { tail -c 2000 "$W/console.log"; die "up: no SSH in $limit s"; }
        sleep 5
    done
    say "up: SSH answered after $(($(date +%s) - t0)) s"
    build
    echo "guest: kernel: $(g uname -r < /dev/null)"
    g 'sudo dmesg' < /dev/null | grep -i -E 'virtio.gpu|\[drm\]|cap set' | sed 's/^/guest: /'
    # The first of the card's directories in debugfs, which root alone reads.
    g "sudo sh -c 'for f in /sys/kernel/debug/dri/*/virtio-gpu-features; do cat \$f; break; done'" < /dev/null | sed 's/^/guest: /'
    g 'ls -l /dev/dri' < /dev/null | sed 's/^/guest: /'
    echo "qemu's log:"
    cat "$OUT/qemu.log"
}

# build: copy nctxcheck.c into the guest and build it there.
build()
{
    g 'cat > nctxcheck.c' < "$here/nctxcheck.c"
    g 'cc -std=c99 -O2 -Wall -Wextra -o nctxcheck nctxcheck.c -ldl' < /dev/null ||
        die "build: nctxcheck did not build"
    echo "build: nctxcheck built"
}

# hist_on, hist_off: count each i915 event in the kernel by a histogram
# trigger, which drops nothing however many fire; hist_off prints the
# counts and removes the triggers.
hist_on()
{
    for e in $EVENTS; do
        [ -w "$T/events/i915/$e/trigger" ] || continue
        if ! { echo 'hist:keys=common_pid' > "$T/events/i915/$e/trigger"; } 2>/dev/null; then
            echo "host: $e: the histogram trigger is refused"
        fi
    done
    return 0
}

hist_off()
{
    found=0
    for e in $EVENTS; do
        f=$T/events/i915/$e
        [ -r "$f/hist" ] || continue
        found=1
        echo "host: $e: $(awk '/^ *Hits:/ { print $2 }' "$f/hist")"
        echo '!hist:keys=common_pid' > "$f/trigger"
    done
    [ "$found" = 1 ] || echo "host: no i915 events here"
}

check()
{
    name=$1
    shift
    pid=$(cat "$W/qemu.pid" 2>/dev/null) || die "check: no guest"
    kill -0 "$pid" 2>/dev/null || die "check: the guest has gone"
    d0=$(dmesg | wc -l)
    q0=$(wc -l < "$OUT/qemu.log")
    gd0=$(g 'sudo dmesg | wc -l' < /dev/null)
    hist_on
    say "check: $name $*"
    status=0
    g ". $top/nctx-env && timeout 900 ./nctxcheck $name $*" < /dev/null || status=$?
    say "check: $name: status $status"
    hist_off
    echo "host: dmesg:"
    dmesg | sed "1,${d0}d"
    echo "host: qemu's log:"
    sed "1,${q0}d" "$OUT/qemu.log"
    echo "host: guest $(kill -0 "$pid" 2>/dev/null && echo running || echo gone)"
    echo "guest: dmesg:"
    g "sudo dmesg | sed '1,${gd0:-0}d'" 2>/dev/null < /dev/null
    return "$status"
}

down()
{
    pid=$(cat "$W/qemu.pid" 2>/dev/null) || { echo "down: no guest"; return 0; }
    g 'sudo systemctl poweroff' < /dev/null 2>/dev/null
    i=0
    while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 120 ]; do
        sleep 1
        i=$((i + 1))
    done
    kill -0 "$pid" 2>/dev/null && { kill "$pid"; say "down: killed QEMU"; }
    rm -f "$W/qemu.pid"
    say "down: QEMU ended"
    echo "qemu's last lines:"
    tail -n 5 "$OUT/qemu.log"
}

leave()
{
    [ -s "$W/qemu.pid" ] && kill "$(cat "$W/qemu.pid")" 2>/dev/null
    [ -s "$W/shmem-before" ] &&
        sed 's/.*\[\(.*\)\].*/\1/' "$W/shmem-before" > "$SH"
    echo "shmem_enabled: $(cat "$SH")"
    rm -rf "$top" "$W"
    echo "removed: $top $W"
    df -h / | sed 1d
    if [ "${1:-}" = --now ]; then
        say "leave: rtcwake -m off -s 120 in 2 s"
        sync
        nohup sh -c 'sleep 2; rtcwake -m off -s 120' > /dev/null 2>&1 &
    fi
}

# step NAME CMD...: run CMD, its output in OUT/NAME.txt and on stdout.
step()
{
    s=$1
    shift
    mkdir -p "$OUT"
    { "$@"; echo "session: $s: status $?"; } 2>&1 | tee "$OUT/$s.txt"
    tail -n 1 "$OUT/$s.txt" | grep -q 'status 0$'
}

# pressure_mib: the textures' total, past the 2 GiB that Haswell's GPU
# address space holds, so that i915 must evict, and within what the host has
# free beside the guest: 3072 MiB at most, in 16 MiB steps; 64 MiB in the
# rehearsal, whose software GL proves the path, not the size.
pressure_mib()
{
    [ "$REHEARSE" = 1 ] && { echo 64; return; }
    avail=$(awk '/MemAvailable/ { print int($2 / 1024) }' /proc/meminfo)
    m=$((avail - 1536))
    [ "$m" -gt 3072 ] && m=3072
    echo $((m / 16 * 16))
}

all()
{
    step setup setup "$1" || die "all: setup failed"
    step capsets capsets
    step up up "$2" || die "all: the guest did not come up"
    step info check info
    step triangle check triangle
    step pressure check pressure "$(pressure_mib)"
    if [ "$REHEARSE" = 1 ]; then
        step submit check submit 1000
        step fence check fence
        step hang check hang 1000
    else
        step submit check submit 10000
        step fence check fence
        step hang check hang 2147483647
    fi
    step down down
    grep -h 'result:\|session: .*status' "$OUT"/*.txt
}

case ${1:-} in
    setup) step setup setup "$2" ;;
    capsets) step capsets capsets ;;
    up) step up up "$2" ;;
    build) step build build ;;
    check) shift; n=$1; shift; step "$n" check "$n" "$@" ;;
    down) step down down ;;
    all) all "$2" "$3" ;;
    leave) leave "${2:-}" ;;
    *) sed -n '2,23p' "$0" >&2; exit 2 ;;
esac
