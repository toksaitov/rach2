#!/bin/sh
# bed-shm.sh: Rach2's virtio-gpu over the region it shares with the test
# bed's VM, run under Rach2's KVM (tests/cases/machine.bed-shm.json).
#
#   sh bed-shm.sh -d DISK [-p PROBE] [-c CRC] [-k NOCOPY | -w [-l LOAD] | -u]
#       [-x MEASURE [-S SCENES]] [-g BDF] [-o DIR] [-m MIB] [-n CPUS]
#
# ivshmemd attaches a region of 32 MiB to /dev/vgpu-shm, and two more
# attaches are refused; QEMU boots DISK, the VM that `bed/vm/image/vm.sh
# rach2` makes, under -accel kvm with the region on an ivshmem-doorbell of
# two vectors; once virtio-gpu has bound over the region, PROBE draws the
# display frame with virgl in the VM and reads it back, and its CRC must be
# CRC, llvmpipe's. The doorbells must have carried the round trips with no
# exit to QEMU: every doorbell the VM rang reached Rach2 through a KVM
# ioeventfd and every one Rach2 rang through an irqfd. Unless -u is given,
# piglit.sh, beside this script, then runs the tests of readback.txt, which
# draw and read back, on the region's card as the desktop user, as the bed's
# piglit scene runs them: each must pass on virgl, with no refusal of a
# blob's export on the VM's console and no refused page list in the kernel's
# log (`readback`). Then QEMU is stopped with fenced submissions pending and
# killed: ivshmemd must detach with the VM's death, every fence must fail
# and a wait after the death must too.
#
# With -k the draw, the doorbells and the death with fences pending give
# way to the buffers mapped from the VM's own pages: NOCOPY, nocopy.c, is
# built with the GNU root's GCC and run on Venus. Its rounds must keep both
# buffers coherent in fence order, the VM's backend must have answered
# their page lists, and their maps must be gone once the program has ended
# (`pages`, `coherent`, `release`); the probe's refusals must be refused
# (`refuse`); the timings of 83-5's comparison are kept in DIR/measure.txt
# (`measure`); and with a mapping held and touched the VM is killed: the
# touch after must raise SIGBUS, and every map and the frames it held must
# be gone (`revoke`, `leak`).
#
# With -w the draw, the doorbells and the death give way to the desktop
# session on the VM's GPU: once virtio-gpu has bound over the region,
# rach2-desktop's session runs Weston on its card with GL through virgl in
# the VM, SIGHUP starting it there unless the session moved there itself,
# and with -g virgl must name the Intel GPU the VM draws on (`session`).
# The load draws as the desktop user while Rach2's driver rings the VM
# (`load`): LOAD is glmark2-wayland on the session, the default, or the
# harness's scene `piglit`, surfaceless on the card's render node, `webgl`
# or `game` on the session, which needs MEASURE, each for 600 s, piglit's
# list again and again; the doorbells are counted once the scene has
# started, after the harness's latency. With MEASURE, the directory of
# bed/measure/measure.sh, each scene of SCENES (`glmark2`) runs on the
# session first, on the rung shm-virgl, or shm-venus for a scene written
# `venus:SCENE`, whose GL goes through Zink on Venus, and its lines are
# printed as figures. Then QEMU is stopped, four fenced submissions are
# left pending and QEMU is killed under the load, which must still run:
# every fence must fail, the load must end, glmark2 before Mesa's virgl
# has said the kernel refused 100000 of its submissions since the death,
# and ivshmemd must detach (`death`); and the session must
# start again, headless with GL on llvmpipe, where glmark2 draws again
# (`fallback`). With -g as well, the session must come back to Rach2's
# driver on the GPU gpu.sh gave back, with the pipe's CRC the same over four
# frames after the first two (`return`); when gpu.sh gave the GPU back
# before the old session had gone, the session starts on it at once, which
# is both its fallback and its return. Each load and each figure runs in a
# process group of its own, which the script ends with the run.
#
# With -u the console first loses a card of the host's own, as it does on
# the Lenovo when gpu.sh takes the GPU from the Intel driver: Weston is
# stopped, the host's virtio-gpu card is opened and closed, so that the
# console takes it back from that master onto a buffer of its own, and the
# card is unplugged through its debugfs `virtio_gpu_reset`; the console
# must have taken the card back with its screen set, and left it
# (`unplug`). With no card, the
# console takes the region's card as virtio-gpu binds over the region,
# inside the bind on the transport's thread, and commits its buffer there;
# the bind must return within 60 s of the card's registration, and the
# log must show the console on the region's card with its screen set
# (`console`), before the draw. A bind that has not returned fails the
# row with where the transport's thread sleeps and whether an open of the
# card, which reaps the device's queues, let it go on.
#
# With -g, which needs -k or -w, the VM also has the host's PCI function
# BDF, the Lenovo's GPU: gpu.sh, beside this script, takes it from Rach2's
# Intel driver, starts QEMU with it on vfio-pci and the disk on AHCI, kills
# QEMU when this script sends it SIGUSR1, and gives the function back to
# the driver, which must pass gpu.sh's own checks once the VM has gone
# (`handoff`). Every run of NOCOPY must then be on the GPU, Venus's
# integrated GPU of Intel's, not on lavapipe, to which the VM falls back
# when its driver does not bind: NOCOPY fails without one, and `coherent`
# fails unless the device it names is one.
#
# Each check prints one line, `bed-shm: CHECK: pass` or `fail` with the
# reason, and the last line is `bed-shm: result: pass` or `fail`, after the
# VM's last 80 lines of console, `bed-shm: vm: LINE`, on a failure. The
# console of the VM, ivshmemd's output and the counters are kept in DIR.
# Any exit, by its end or by SIGTERM, SIGINT or SIGHUP, kills QEMU, has
# gpu.sh give the function back, and ends the load's and the figure's
# groups, TERM and then KILL.
set -u
disk='' probe=./vgshmprobe crc=6e8e217c out=/tmp/bed-shm mib=2048 cpus=2 nocopy='' bdf=''
desk=0 unplug=0 measure='' loadscene=glmark2 scenes=glmark2
icd=/usr/share/vulkan/icd.d/venus_icd.x86_64.json
gpush=$(dirname "$0")/gpu.sh
while getopts d:p:c:k:wul:x:S:g:o:m:n: o; do
    case $o in
        w) desk=1 ;;
        u) unplug=1 ;;
        l) loadscene=$OPTARG ;;
        x) measure=$OPTARG ;;
        S) scenes=$OPTARG ;;
        d) disk=$OPTARG ;;
        p) probe=$OPTARG ;;
        c) crc=$OPTARG ;;
        k) nocopy=$OPTARG ;;
        g) bdf=$OPTARG ;;
        o) out=$OPTARG ;;
        m) mib=$OPTARG ;;
        n) cpus=$OPTARG ;;
        *) exit 2 ;;
    esac
done
[ -n "$disk" ] || {
    echo "usage: bed-shm.sh -d DISK [-p PROBE] [-c CRC] [-k NOCOPY | -w [-l LOAD] | -u] [-x MEASURE [-S SCENES]] [-g BDF] [-o DIR] [-m MIB] [-n CPUS]" >&2
    exit 2
}
case $loadscene in
    glmark2) ;;
    piglit | webgl | game) [ -n "$measure" ] || { echo "bed-shm: -l $loadscene needs -x" >&2; exit 2; } ;;
    *) echo "bed-shm: no load $loadscene; glmark2, piglit, webgl or game" >&2; exit 2 ;;
esac
# The GPU's MSIs reach the VM through irqfds and would count as doorbells
# Rach2 rang, so -g runs only the nocopy or the desktop rows, which count
# no irqfd.
[ -z "$bdf" ] || [ -n "$nocopy" ] || [ "$desk" = 1 ] || { echo "bed-shm: -g needs -k or -w" >&2; exit 2; }
[ -z "$bdf" ] || [ -f "$gpush" ] || { echo "bed-shm: no $gpush for -g" >&2; exit 2; }
[ -z "$nocopy" ] || [ "$desk" = 0 ] || { echo "bed-shm: -k and -w are apart" >&2; exit 2; }
[ "$unplug" = 0 ] || { [ -z "$nocopy$bdf" ] && [ "$desk" = 0 ]; } ||
    { echo "bed-shm: -u is apart from -k, -w and -g" >&2; exit 2; }
daemon='' qemu='' gpu='' waiter='' holder='' pid='' load='' figpid='' umon='' bad=0
# NOCOPY_INTEL, for nocopy: with -g, the GPU's Venus device or a failure.
intel=''
[ -z "$bdf" ] || intel=1

# end_group PID: end the process group that PID leads, which setsid made,
# with TERM and, once 20 seconds have passed, KILL, and reap its leader,
# so that a load's scene, its browser and its compositor end with it. The
# harness's own stop takes up to 15 seconds.
end_group()
{
    [ -n "$1" ] || return 0
    kill -TERM "-$1" 2>/dev/null
    n=200
    while kill -0 "-$1" 2>/dev/null && [ "$n" -gt 0 ]; do
        sleep 0.1
        n=$((n - 1))
    done
    kill -KILL "-$1" 2>/dev/null
    wait "$1" 2>/dev/null
}
# gpu.sh, sent SIGTERM, kills its QEMU and gives the function back.
trap 'end_group "$load"; end_group "$figpid"; kill -9 $qemu $waiter $holder $pid 2>/dev/null; kill $daemon $gpu $umon 2>/dev/null; wait 2>/dev/null' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
rm -rf "$out"
mkdir -p "$out"
# Where the desktop's log stood before anything here ran.
offset=$(wc -c < /var/log/services 2>/dev/null || echo 0)
console=$out/console.log qemu_err=$out/qemu.out
[ -z "$bdf" ] || console=$out/gpu/console qemu_err=$out/gpu/qemu.err

pass() { echo "bed-shm: $1: pass${2:+: $2}"; }
fail() { echo "bed-shm: $1: fail: $2"; bad=$((bad + 1)); }

# stat NAME: a count of /proc/stat.
stat() { awk -v n="$1" '$1 == n { print $2 }' /proc/stat; }

# info FIELD: a field of ivshmemd -i's report, as `name value`.
info() { ivshmemd -i | tr ',' '\n' | sed -n "s/^.* $1 \([0-9/]*\).*$/\1/p" | head -n 1; }

# busy PID: the processor time of process PID, in ticks, which grows while
# a guest of it runs and stays while the guest waits halted.
busy() { awk '{ print $14 + $15 }' "/proc/$1/stat" 2>/dev/null || echo 0; }

# quiet FILE SILENCE [PID]: one second of a wait, which `last=-1 still=0`
# starts; it fails once process PID has ended, or once for SILENCE seconds
# FILE has not grown and PID has used no processor time.
quiet()
{
    [ -z "${3:-}" ] || kill -0 "$3" 2>/dev/null || return 1
    sleep 1
    size=$(command stat -c %s "$1" 2>/dev/null || echo 0):$([ -z "${3:-}" ] || busy "$3")
    if [ "$size" = "$last" ]; then
        still=$((still + 1))
        [ "$still" -lt "$2" ] || return 1
    else
        last=$size still=0
    fi
}

# until_line FILE PATTERN SILENCE [PID]: wait until FILE has a line
# matching PATTERN, while quiet FILE SILENCE PID holds.
until_line()
{
    last=-1 still=0
    until grep -q "$2" "$1" 2>/dev/null; do
        quiet "$1" "$3" "${4:-}" || return 1
    done
}

# refused: how many of the load's submissions Mesa's virgl has said the
# kernel refused since the death, "got error from kernel" in the load's
# output from byte `at` on, read up to the output's size at the call, which
# stat reads without reading the file, and counted to 100000 at most; 0 for
# a load other than glmark2.
refused()
{
    [ "$loadscene" = glmark2 ] || { echo 0; return 0; }
    to=$(command stat -c %s "$loadout")
    tail -c "+$((at + 1))" "$loadout" | head -c "$((to - at))" |
        grep -m 100000 -c 'got error from kernel' || :
}

# klog_count PATTERN: how many lines of the kernel's log match PATTERN.
klog_count() { dmesg | grep -c "$1"; }

# klog_more PATTERN COUNT: wait until more than COUNT lines of the kernel's
# log match PATTERN, looking once a second, 60 times at most.
klog_more()
{
    n=60
    while [ "$(klog_count "$1")" -le "$2" ]; do
        n=$((n - 1))
        [ "$n" -gt 0 ] || return 1
        sleep 1
    done
}

# With -u, the console onto the host's virtio-gpu card and the card away.
if [ "$unplug" = 1 ]; then
    reset='' host=''
    # The card's directory is named by its bus address, and dri/N, N the
    # card's minor, links to it.
    for f in /sys/kernel/debug/dri/*/virtio_gpu_reset; do
        [ -e "$f" ] || continue
        dir=${f%/virtio_gpu_reset}
        dir=${dir##*/}
        for link in /sys/kernel/debug/dri/[0-9]*; do
            n=${link##*/}
            case $n in *[!0-9]*) continue ;; esac
            [ "$n" -lt 128 ] && [ "$(readlink "$link")" = "$dir" ] || continue
            reset=$f host=card$n
            break
        done
        [ -z "$reset" ] || break
    done
    pkill -x weston 2>/dev/null || :
    n=30
    while pgrep -x weston > /dev/null && [ "$n" -gt 0 ]; do
        sleep 1
        n=$((n - 1))
    done
    took=$(klog_count "console: $host: taken back from a master")
    placed=$(klog_count "console: $host: [0-9]*x[0-9]*, the font at")
    left=$(klog_count "console: $host: left")
    # The first opener of a card with no master becomes its master; at the
    # close the console takes the card back onto a buffer of its own. The
    # take-back is logged whether or not its buffer was placed; the size
    # and font line follows only a placed one, without which the console
    # holds no buffer, its leaving marks nothing lost, and the region's
    # card is only probed.
    [ -z "$host" ] || : <> "/dev/dri/$host"
    if [ -n "$reset" ] && klog_more "console: $host: taken back from a master" "$took" &&
        klog_more "console: $host: [0-9]*x[0-9]*, the font at" "$placed" &&
        echo 1 > "$reset" && klog_more "console: $host: left" "$left"; then
        pass unplug "the console took $host back from a master onto a buffer of its own, and $host left"
    else
        fail unplug "card '$host': $(dmesg | grep 'console: ' | tail -n 3 | tr '\n' ' ')"
    fi
    klog_from=$(dmesg | wc -l)
fi

# The attach, once, and two refused.
ivshmemd -s 32M "$out/ivs.sock" > "$out/ivshmemd.out" 2>&1 &
daemon=$!
until_line "$out/ivshmemd.out" 'listening on' 30 || true
refused=0
for i in 1 2; do
    if ivshmemd -s 32M "$out/other$i.sock" > "$out/refused$i.out" 2>&1; then
        :
    elif grep -q 'attach: refused.*busy' "$out/refused$i.out"; then
        refused=$((refused + 1))
    fi
done
if grep -q 'attached: 33554432 bytes' "$out/ivshmemd.out" && [ "$refused" -eq 2 ]; then
    pass attach "attached once, refused twice"
else
    fail attach "$(cat "$out/ivshmemd.out" "$out/refused1.out" | head -n 3 | tr '\n' ' ')"
fi

# The VM, under Rach2's KVM, with the region on its doorbell device. Its
# disk takes no ioeventfd, and QEMU raises a disk's interrupts without an
# irqfd, so that both counts are the doorbells'. With -g, gpu.sh starts it
# with the function, and $qemu is QEMU's own process, from its pid file;
# QEMU's messages are then in gpu.sh's DIR/gpu/qemu.err.
if [ -z "$bdf" ]; then
    TMPDIR=$out qemu-system-x86_64 -accel kvm -machine q35,kernel-irqchip=split -cpu host -smp "$cpus" \
        -m "${mib}M" -nodefaults -no-reboot -display none -monitor none \
        -drive file="$disk",if=none,id=vm,format=raw,snapshot=on \
        -device virtio-blk-pci,drive=vm,ioeventfd=off \
        -chardev socket,path="$out/ivs.sock",id=ivs -device ivshmem-doorbell,chardev=ivs,vectors=2 \
        -serial file:"$console" > "$out/qemu.out" 2>&1 &
    qemu=$!
else
    TMPDIR=$out bash "$gpush" -d "$bdf" -m "$mib" -s "$cpus" -w 0 -o "$out/gpu" -D "$disk" -- \
        -pidfile "$out/qemu.pid" \
        -chardev socket,path="$out/ivs.sock",id=ivs -device ivshmem-doorbell,chardev=ivs,vectors=2 \
        > "$out/gpu.log" 2>&1 &
    gpu=$!
    last=-1 still=0
    until [ -s "$out/qemu.pid" ]; do
        quiet "$out/gpu.log" 120 "$gpu" || break
    done
    qemu=$(cat "$out/qemu.pid" 2>/dev/null)
fi

# kill_vm: kill QEMU and wait for its end; with -g, through gpu.sh, which
# then gives the function back.
kill_vm()
{
    if [ -n "$gpu" ]; then
        kill -USR1 "$gpu" 2>/dev/null
        gpu_status=0
        wait "$gpu" 2>/dev/null || gpu_status=$?
        gpu=''
    else
        kill -9 "$qemu" 2>/dev/null
        wait "$qemu" 2>/dev/null
    fi
    qemu=''
}

# handoff: with -g, gpu.sh's verdict on the function's return to the driver.
handoff()
{
    [ -n "$bdf" ] || return 0
    [ -z "$gpu" ] || kill_vm
    if [ "${gpu_status:-1}" = 0 ] && grep -q '^gpu: result: pass' "$out/gpu.log"; then
        pass handoff "$(sed -n 's/^gpu: result: pass: //p' "$out/gpu.log")"
    else
        fail handoff "status ${gpu_status:-none}: $(grep '^gpu: result\|^gpu: evidence' "$out/gpu.log" | tr '\n' ' ')"
    fi
}
# The VM's backend binds virtio-gpu once its service starts, before the
# VM's boot has ended.
# With -u the bind must return within 60 s of the region's card's
# registration, since the VM keeps QEMU busy whether or not it does.
if until_line "$out/ivshmemd.out" 'QEMU connected' 60 "$qemu"; then
    last=-1 still=0 registered=0
    until [ "$(info bound)" = 1 ]; do
        quiet "$console" 600 "$qemu" || break
        [ "$unplug" = 1 ] || continue
        dmesg | tail -n "+$((klog_from + 1))" | grep -q 'drm: virtio_gpu: card' || continue
        registered=$((registered + 1))
        [ "$registered" -lt 60 ] || break
    done
fi
ivshmemd -i > "$out/info-bound.txt" 2>&1
# With -u, the console's place on the region's card, inside the bind.
if [ "$unplug" = 1 ]; then
    dmesg | tail -n "+$((klog_from + 1))" > "$out/dmesg-bind.txt"
    card=$(sed -n 's/^.*drm: virtio_gpu: \(card[0-9]*\)$/\1/p' "$out/dmesg-bind.txt" | tail -n 1)
    stuck=''
    if [ "$(info bound)" != 1 ] && [ -n "$card" ]; then
        # The bind has not returned: where the transport's thread sleeps,
        # then whether an open of the card, which reaps the device's
        # queues, lets it go on.
        t=$(pgrep -x vgpu-shm | head -n 1)
        stuck="vgpu-shm sleeps in $(cat "/proc/$t/wchan" 2>/dev/null || echo '?')"
        : <> "/dev/dri/$card"
        n=60
        until [ "$(info bound)" = 1 ] || [ "$n" -eq 0 ]; do
            sleep 1
            n=$((n - 1))
        done
        if [ "$(info bound)" = 1 ]; then
            stuck="$stuck, and the bind returned once $card was opened, whose open reaps the queues"
        else
            stuck="$stuck, and an open of $card did not end the bind"
        fi
        dmesg | tail -n "+$((klog_from + 1))" > "$out/dmesg-bind.txt"
    fi
    if [ -z "$stuck" ] && [ "$(info bound)" = 1 ] && [ -n "$card" ] &&
        grep -q "console: $card: a native display" "$out/dmesg-bind.txt" &&
        ! grep -q "console: $card: screen not set" "$out/dmesg-bind.txt"; then
        pass console "virtio-gpu bound over the region as $card with the console placed on it"
    else
        fail console "${stuck:+$stuck; }bound $(info bound), card '$card': $(grep 'console: \|virtio_gpu: command' "$out/dmesg-bind.txt" | tail -n 3 | tr '\n' ' ')"
    fi
fi

# figure COMMAND...: one measurement, its lines added to DIR/measure.txt
# and printed, each as `bed-shm: figure: LINE`, after `bed-shm: measuring:
# COMMAND`; Venus is selected for nocopy; ok=0 when it fails, with
# ivshmemd's counts. Every 30 s while
# it runs, ivshmemd's counts and the VM's last lines show how far it has
# got.
figure()
{
    echo "bed-shm: measuring: $*"
    VK_DRIVER_FILES=$icd NOCOPY_INTEL=$intel "$@" > "$out/figure.out" 2>&1 &
    pid=$! s=0
    while kill -0 "$pid" 2>/dev/null; do
        sleep 1
        s=$((s + 1))
        [ $((s % 30)) -ne 0 ] && continue
        echo "bed-shm: running: $s s: $(ivshmemd -i 2>&1 | tr '\n' ' ')"
        tr -d '\r' < "$console" | tail -n 3 | sed 's/^/bed-shm: running: vm: /'
    done
    figure_status=0
    wait "$pid" || figure_status=$?
    pid=''
    [ "$figure_status" -eq 0 ] || { ok=0; echo "bed-shm: failed: $(ivshmemd -i 2>&1 | tr '\n' ' ')"; }
    cat "$out/figure.out" >> "$out/measure.txt"
    sed 's/^/bed-shm: figure: /' "$out/figure.out"
}

# nocopy_rows: the buffers mapped from the VM's pages, then the VM killed
# with one mapping live.
nocopy_rows()
{
    if [ "$(info bound)" != 1 ]; then
        for c in build pages coherent release refuse measure revoke leak; do
            fail "$c" "virtio-gpu did not bind over the region: $(tail -n 2 "$console" "$qemu_err" 2>&1 | tr '\n' ' ')"
        done
        return
    fi
    if gcc -std=c99 -O2 -o "$out/nocopy" "$nocopy" -ldl > "$out/build.out" 2>&1; then
        pass build "nocopy built"
    else
        fail build "$(head -n 3 "$out/build.out" | tr '\n' ' ')"
    fi
    maps0=$(stat kvm_gpa_maps) pages0=$(stat kvm_gpa_map_pages)
    round_bytes=1048576
    VK_DRIVER_FILES=$icd NOCOPY_INTEL=$intel "$out/nocopy" rounds 16 "$round_bytes" > "$out/rounds.out" 2>&1
    ivshmemd -i > "$out/info-rounds.txt" 2>&1
    pins=$(grep -c 'backend: pin: resource' "$console")
    if awk -v maps="$maps0" -v pages="$pages0" -v bytes="$round_bytes" '
        /^NOCOPY MAPPED: kvm_gpa_maps [0-9]+, kvm_gpa_map_pages [0-9]+$/ {
            held = $4 + 0 > maps && $6 >= pages + 2 * bytes / 4096
        }
        END { exit !(held && maps ~ /^[0-9]+$/ && pages ~ /^[0-9]+$/) }
        ' "$out/rounds.out" && [ "$pins" -gt 0 ]; then
        pass pages "$pins blobs pinned, $(info maps) maps, $(sed -n 's/^NOCOPY MAPPED: //p' "$out/rounds.out")"
    else
        fail pages "pins $pins: $(grep 'NOCOPY' "$out/rounds.out" | tr '\n' ' ')"
    fi
    device=$(sed -n 's/^NOCOPY DEVICE: //p' "$out/rounds.out")
    if [ -n "$intel" ] && ! echo "$device" | grep -q ', vendor 0x8086, integrated GPU, '; then
        fail coherent "not on the GPU: ${device:-no device}: $(grep 'NOCOPY' "$out/rounds.out" | tr '\n' ' ')"
    elif grep -q '^NOCOPY OK' "$out/rounds.out"; then
        pass coherent "$(sed -n 's/^NOCOPY ROUNDS: //p' "$out/rounds.out"); $device"
    else
        fail coherent "$(grep 'NOCOPY' "$out/rounds.out" | tr '\n' ' ')"
    fi
    if [ "$(stat kvm_gpa_maps)" = "$maps0" ] && [ "$(stat kvm_gpa_map_pages)" = "$pages0" ] &&
        [ "$(info map_live)" = 0 ]; then
        pass release "kvm_gpa_maps $maps0, kvm_gpa_map_pages $pages0, map_live 0"
    else
        fail release "kvm_gpa_maps $maps0 to $(stat kvm_gpa_maps), kvm_gpa_map_pages $pages0 to $(stat kvm_gpa_map_pages), map_live $(info map_live)"
    fi
    # The VM's console comes behind its work, at the serial port's pace.
    "$probe" refuse > "$out/refuse.out" 2>&1
    until_line "$console" 'backend: map: resource .*: refused: not exported' 60 "$qemu" ||
        true
    if grep -q 'VGSHMPROBE OK' "$out/refuse.out" &&
        grep -q 'backend: map: resource .*: refused: not exported' "$console"; then
        pass refuse "$(sed -n 's/^VGSHMPROBE REFUSE: //p' "$out/refuse.out" | tr '\n' ';')"
    else
        fail refuse "$(grep 'VGSHMPROBE' "$out/refuse.out" | tr '\n' ' ')"
    fi
    { qemu-system-x86_64 --version | head -n 1; uname -sr; } > "$out/measure.txt" 2>&1
    sed 's/^/bed-shm: figure: /' "$out/measure.txt"
    ok=1
    for b in 4096 65536 1048576 16777216 67108864; do
        figure "$out/nocopy" serial 8 "$b"
    done
    # A mapping's every page comes at its first fault, so no touch of a
    # buffer takes a fault for each page.
    faults=$(sed -n 's/^NOCOPY SERIAL: .*; touch faults most \([0-9]*\)$/\1/p' "$out/measure.txt" |
        sort -n | tail -n 1)
    if [ -z "$faults" ] || [ "$faults" -gt 16 ]; then
        echo "NOCOPY FAIL: a buffer's touch took ${faults:-an unread count of} faults, over 16" \
            >> "$out/measure.txt"
        ok=0
    fi
    figure "$out/nocopy" held 32 65536
    figure "$out/nocopy" bandwidth 67108864
    for b in 4096 1048576 4194304; do
        figure "$probe" measure "$b"
    done
    if [ "$ok" = 1 ]; then
        pass measure "$(grep -c '^NOCOPY [A-Z]*:\|^VGSHMPROBE MEASURE' "$out/measure.txt") figures"
    else
        fail measure "$(grep 'FAIL' "$out/measure.txt" | head -n 2 | tr '\n' ' ')"
    fi

    # The VM's death with a mapping live and touched.
    VK_DRIVER_FILES=$icd NOCOPY_INTEL=$intel "$out/nocopy" hold 1048576 > "$out/hold.out" 2>&1 &
    holder=$!
    until_line "$out/hold.out" 'NOCOPY HELD\|NOCOPY FAIL' 120 "$holder" || true
    ivshmemd -i > "$out/info-held.txt" 2>&1
    held=$(stat kvm_gpa_map_pages)
    kill_vm
    until_line "$out/hold.out" 'NOCOPY SIGBUS\|NOCOPY FAIL' 60 "$holder" || true
    kill -9 "$holder" 2>/dev/null
    wait "$holder" 2>/dev/null
    holder=''
    wait "$daemon" 2>/dev/null
    daemon=''
    if grep -q '^NOCOPY SIGBUS' "$out/hold.out" && grep -q '^NOCOPY HELD' "$out/hold.out"; then
        pass revoke "$(sed -n 's/^NOCOPY SIGBUS: //p' "$out/hold.out")"
    else
        fail revoke "$(grep 'NOCOPY' "$out/hold.out" | tr '\n' ' ')"
    fi
    if [ "$(stat kvm_gpa_maps)" = "$maps0" ] && [ "$(stat kvm_gpa_map_pages)" = "$pages0" ] &&
        [ "$held" -gt "$pages0" ]; then
        pass leak "kvm_gpa_map_pages $pages0, $held while held, $(stat kvm_gpa_map_pages) after"
    else
        fail leak "kvm_gpa_maps $maps0 to $(stat kvm_gpa_maps), kvm_gpa_map_pages $pages0, $held while held, $(stat kvm_gpa_map_pages) after"
    fi
}

# still_counts: wait until the doorbells' counts, both sides' and KVM's,
# are the same at two looks a second apart, 10 looks at most, and leave
# that sample in `still` as `woken rung ioeventfd irqfd`, the transport's
# counts as ivshmemd gives them. Returns 1, with `still` empty, when no two
# looks agree.
still_counts()
{
    n=10 last='' still=''
    while [ "$n" -gt 0 ]; do
        now="$(info woken) $(info rung) $(stat kvm_ioeventfd_signals) $(stat kvm_irqfd_raises)"
        if [ "$now" = "$last" ]; then
            still=$now
            return 0
        fi
        last=$now n=$((n - 1))
        sleep 1
    done
    return 1
}

# refusals: the VM console's refusals of a blob's export for its page list.
refusals() { tr -d '\r' < "$console" 2>/dev/null | grep -c 'backend: map: resource .*: refused: not exported'; }

# readback: readback.txt's piglit tests on the region's card, one at a time
# on EGL's surfaceless platform as the desktop user, as the bed's piglit
# scene runs them (bed/measure/measure.sh), and the refusals they met.
readback()
{
    kit=$(cd "$(dirname "$0")" && pwd)
    rb=$out/readback
    mkdir -p "$rb"
    chown desktop "$rb"
    exports0=$(refusals) klog0=$(klog_count 'refused: its page list')
    su -s /bin/sh desktop -c "exec sh $kit/piglit.sh -g -o $rb -l $kit/readback.txt" \
        > "$out/readback.out" 2>&1
    piglit_status=$?
    # The VM's console comes behind its work, at the serial port's pace:
    # wait until it has been silent for 5 s, 120 s at most.
    last=-1 still=0 n=120
    while [ "$n" -gt 0 ] && quiet "$console" 5 "$qemu"; do
        n=$((n - 1))
    done
    exports=$(($(refusals) - exports0)) klog=$(($(klog_count 'refused: its page list') - klog0))
    renderer=$(sed -n 's/^renderer //p' "$rb/record" 2>/dev/null | head -n 1)
    results=''
    passed=0 tests=0
    while IFS= read -r t; do
        case $t in '' | '#'*) continue ;; esac
        tests=$((tests + 1))
        r=$(awk -v t="$t" 'substr($0, 1, length(t) + 1) == t " " { print substr($0, length(t) + 2) }' \
            "$rb/readback.results" 2>/dev/null)
        [ "$r" != pass ] || passed=$((passed + 1))
        results="$results${results:+, }${t##*@} ${r:-none}"
    done < "$kit/readback.txt"
    what="$results; $exports refused exports, $klog refused page lists; renderer ${renderer:-unknown}"
    if [ "$piglit_status" = 0 ] && [ "$passed" = "$tests" ] && [ "$exports" = 0 ] && [ "$klog" = 0 ] &&
        echo "$renderer" | grep -q '^virgl'; then
        pass readback "$what"
    else
        fail readback "piglit.sh status $piglit_status: $what"
    fi
}

# virgl_rows: the frame drawn, the doorbells, without -u the readbacks, and
# the VM's death with fences pending.
virgl_rows()
{
    if [ "$(info bound)" != 1 ]; then
        fail draw "virtio-gpu did not bind over the region: $(tail -n 2 "$console" "$qemu_err" 2>&1 | tr '\n' ' ')"
        fail doorbell "no device"
        [ "$unplug" = 1 ] || fail readback "no device"
    else
        # A doorbell is counted twice, by KVM and by the transport, one
        # after the other, so each pair of counts is read with the doorbells
        # still, which would otherwise split one across the two reads.
        before=''
        ! still_counts || before=$still
        "$probe" draw "$crc" > "$out/draw.out" 2>&1
        after=''
        ! still_counts || after=$still
        polls=$(info polls)
        ivshmemd -i > "$out/info-drawn.txt" 2>&1
        if grep -q 'VGSHMPROBE OK' "$out/draw.out"; then
            pass draw "$(sed -n 's/^VGSHMPROBE DRAW: //p' "$out/draw.out")"
        else
            fail draw "$(grep VGSHMPROBE "$out/draw.out" | tr '\n' ' ')"
        fi
        if [ -z "$before" ] || [ -z "$after" ]; then
            echo "bed-shm: counts: never still for a second in 10 s: before '$before', after '$after'" \
                > "$out/counts.txt"
            fail doorbell "$(cat "$out/counts.txt")"
        else
            # shellcheck disable=SC2086 # the two samples' eight counts
            set -- $before $after
            woken=$((${5%%/*} - ${1%%/*})) rung=$((${6%%/*} - ${2%%/*}))
            io=$(($7 - $3)) irq=$(($8 - $4))
            echo "bed-shm: counts: rung $rung, woken $woken, polls $polls, ioeventfd $io, irqfd $irq" \
                > "$out/counts.txt"
            if [ "$(info doorbells)" = 1 ] && [ "$woken" -gt 0 ] && [ "$io" -ge "$woken" ] &&
                [ "$irq" -ge "$rung" ] && [ "$rung" -gt 0 ]; then
                pass doorbell "rung $rung, woken $woken, ioeventfd $io, irqfd $irq"
            else
                fail doorbell "$(cat "$out/counts.txt"), doorbells $(info doorbells)"
            fi
        fi
        [ "$unplug" = 1 ] || readback
    fi

    # The VM's death with fences pending.
    kill -STOP "$qemu" 2>/dev/null
    "$probe" pending 4 > "$out/pending.out" 2>&1 &
    waiter=$!
    until_line "$out/pending.out" 'VGSHMPROBE PENDING\|VGSHMPROBE FAIL' 30 || true
    kill_vm
    wait "$waiter" 2>/dev/null
    waiter=''
    wait "$daemon" 2>/dev/null
    daemon=''
    if grep -q 'VGSHMPROBE OK' "$out/pending.out" &&
        grep -q 'result: detached: QEMU closed' "$out/ivshmemd.out"; then
        pass death "$(sed -n 's/^VGSHMPROBE FENCE: //p' "$out/pending.out" | head -n 1)"
    else
        fail death "$(grep 'VGSHMPROBE\|result' "$out/pending.out" "$out/ivshmemd.out" | tr '\n' ' ')"
    fi
}

# The desktop session's log and its user's display.
services=/var/log/services
desktop_env='XDG_RUNTIME_DIR=/run/user/1001 WAYLAND_DISPLAY=wayland-0'

# since OFFSET: the services' log from byte OFFSET on, which is kept in
# DIR/services.txt.
since()
{
    tail -c +"$(($1 + 1))" "$services" 2>/dev/null | tr -d '\r' > "$out/services.txt"
    cat "$out/services.txt"
}

# started OFFSET DISPLAY RENDERER SILENCE [FILE PID]: wait until the
# session's last start since OFFSET is on DISPLAY, `BACKEND CARD RENDERER`
# as rach2-desktop logs it, its Weston has logged a GL renderer matching
# RENDERER and the user's socket exists, while quiet FILE SILENCE PID holds,
# FILE the services' log unless given; set `took` to the seconds waited.
# With which=any, any start since OFFSET will do, not only the last.
which=last
started()
{
    last=-1 still=0 took=0
    until since "$1" | awk -v want="on $2" -v gl="$3" -v which="$which" '
            /rach2-desktop: session: started as / { on = index($0, want) > 0; seen = 0 }
            on && /GL renderer: / && $0 ~ ("GL renderer: " gl) { seen = ever = 1 }
            END { exit !(which == "any" ? ever : on && seen) }' && [ -S /run/user/1001/wayland-0 ]; do
        quiet "${5:-$services}" "$4" "${6:-}" || return 1
        took=$((took + 1))
    done
}

# glmark ARGS...: glmark2-wayland as the desktop user on the session.
glmark()
{
    su -s /bin/sh desktop -c "export $desktop_env; exec glmark2-wayland $*"
}

# drawn FILE ARGS...: glmark ARGS into FILE, killed once FILE has stayed the
# same for 120 s, as a client of a compositor that no longer answers does.
drawn()
{
    out_file=$1
    shift
    glmark "$@" > "$out_file" 2>&1 &
    waiter=$!
    last=-1 still=0
    while kill -0 "$waiter" 2>/dev/null; do
        quiet "$out_file" 120 || pkill -9 -x glmark2-wayland || kill -9 "$waiter" 2>/dev/null
    done
    drawn_status=0
    wait "$waiter" || drawn_status=$?
    waiter=''
    return "$drawn_status"
}

# desk_rows: the desktop session on the VM's GPU, the VM killed under
# glmark2, the session back on llvmpipe and, with -g, on Rach2's driver.
desk_rows()
{
    card=''
    for c in /sys/class/drm/card[0-9]*; do
        case $(readlink -f "$c/device" 2>/dev/null) in */platform/virtio_gpu) card=${c##*/} ;; esac
    done
    if [ "$(info bound)" != 1 ] || [ -z "$card" ]; then
        for c in session load death fallback; do
            fail "$c" "virtio-gpu did not bind over the region: card '$card', $(tail -n 2 "$console" "$qemu_err" 2>&1 | tr '\n' ' ')"
        done
        return
    fi
    sup=$(cat /run/rach2-desktop.pid 2>/dev/null)
    # The session's start is in the log by the time of the bind, or never:
    # rach2-desktop moves it at once to a card that appears after a loss.
    since "$offset" | grep -q "rach2-desktop: session: started as .* on drm $card gl" ||
        kill -HUP "$sup" 2>/dev/null
    # With the GPU, the VM's virgl draws on its i915, which virgl names.
    gl=virgl
    [ -z "$bdf" ] || gl='virgl .*Intel'
    if started "$offset" "drm $card gl" "$gl" 600 "$console" "$qemu"; then
        pass session "Weston on $card through virgl after $took s: $(grep 'GL renderer: ' "$out/services.txt" | tail -n 1 | sed 's/^.*GL renderer: //')"
    else
        fail session "$(grep 'rach2-desktop: \|GL renderer\|fatal\|error' "$out/services.txt" | tail -n 4 | tr '\n' ' ')"
    fi
    for scene in ${measure:+$scenes}; do
        rung=shm-virgl zink=''
        case $scene in venus:*) rung=shm-venus scene=${scene#venus:} zink="MESA_LOADER_DRIVER_OVERRIDE=zink VK_DRIVER_FILES=$icd" ;; esac
        # shellcheck disable=SC2086 # assignments
        setsid env $desktop_env $zink sh "$measure/measure.sh" -r "$rung" -u desktop -d \
            -o "$out/measure/$rung-$scene" "$scene" > "$out/measure-$rung-$scene.out" 2>&1 &
        figpid=$!
        wait "$figpid"
        figpid=''
        sed 's/^/bed-shm: figure: /' "$out/measure-$rung-$scene.out"
    done

    # The load, in a group of its own, until the driver has rung the VM's
    # queue 32 times since it began to draw: glmark2 once it has named its
    # renderer, a scene of the harness once the harness says it has
    # started, after its latency's own round trips, for longer than the
    # step will take.
    if [ "$loadscene" = glmark2 ]; then
        loadout=$out/glmark2.out
        setsid su -s /bin/sh desktop -c "export $desktop_env; exec glmark2-wayland --run-forever -b build" \
            > "$loadout" 2>&1 &
        load=$!
        until_line "$loadout" 'GL_RENDERER' 600 "$load" || true
    else
        loadout=$out/load.out
        # shellcheck disable=SC2086 # two assignments
        setsid env $desktop_env sh "$measure/measure.sh" -r shm-virgl -u desktop -d -t 600 \
            -o "$out/load" "$loadscene" > "$loadout" 2>&1 &
        load=$!
        until_line "$out/load/scene.log" "measure: scene: $loadscene: started" 600 "$load" || true
    fi
    rung0=$(info rung | cut -d/ -f1)
    last=-1 still=0
    until [ "$(($(info rung | cut -d/ -f1) - ${rung0:-0}))" -ge 32 ]; do
        quiet "$console" 600 "$load" || break
    done
    drew=$(($(info rung | cut -d/ -f1) - ${rung0:-0}))
    if [ "$loadscene" = glmark2 ]; then
        named=$(sed -n 's/^ *GL_RENDERER: *//p' "$loadout" | head -n 1)
    else
        named="the session's virgl"
    fi
    if kill -0 "$load" 2>/dev/null && [ "$drew" -ge 32 ] &&
        { [ "$loadscene" != glmark2 ] || grep -q 'GL_RENDERER: *virgl' "$loadout"; }; then
        pass load "$loadscene on $named, $drew doorbells"
    else
        fail load "$loadscene, $drew doorbells: $(tail -n 3 "$loadout" | tr '\n' ' ')"
    fi

    # The VM's death under the load, with fences pending; the load must
    # still run once the VM is stopped and the probe has its fences pending,
    # just before the death, which with -g gpu.sh brings within a second.
    # udev's events for the cards are kept, the monitor listening before
    # the death, for a death after which the session stays on the gone
    # card; udevadm names its kernel source last, once it has subscribed to
    # both.
    dead=$(command stat -c %s "$services")
    udevadm monitor --kernel --udev --subsystem-match=drm > "$out/udev-death.txt" 2>&1 &
    umon=$!
    until_line "$out/udev-death.txt" '^KERNEL - ' 30 "$umon" || true
    monitor=listening
    { grep -q '^UDEV - ' "$out/udev-death.txt" && grep -q '^KERNEL - ' "$out/udev-death.txt" &&
        kill -0 "$umon" 2>/dev/null; } ||
        monitor="not listening: $(tail -n 2 "$out/udev-death.txt" | tr '\n' ' ')"
    at=$(command stat -c %s "$loadout")
    kill -STOP "$qemu" 2>/dev/null
    "$probe" pending 4 > "$out/pending.out" 2>&1 &
    waiter=$!
    until_line "$out/pending.out" 'VGSHMPROBE PENDING\|VGSHMPROBE FAIL' 30 || true
    alive=no
    ! grep -q 'VGSHMPROBE PENDING' "$out/pending.out" || ! kill -0 "$load" 2>/dev/null || alive=yes
    kill_vm
    wait "$waiter" 2>/dev/null
    waiter=''
    wait "$daemon" 2>/dev/null
    daemon=''
    # glmark2 ends with the session that leaves the gone card; one whose
    # submissions the kernel has refused 100000 times since the death, at
    # some 10000 a second, draws on a session still on that card.
    last=-1 still=0
    while kill -0 "$load" 2>/dev/null; do
        quiet "$loadout" 300 "$load" || break
        [ "$(refused)" -lt 100000 ] || break
    done
    # SIGTERM ends a monitor still listening with 143.
    kill "$umon" 2>/dev/null
    ustatus=0
    wait "$umon" 2>/dev/null || ustatus=$?
    [ "$ustatus" = 143 ] || [ "$monitor" != listening ] ||
        monitor="ended with status $ustatus before the row's end: $(tail -n 2 "$out/udev-death.txt" | tr '\n' ' ')"
    umon=''
    if [ "$alive" = no ]; then
        ended="ended before the death"
        wait "$load" 2>/dev/null
    elif kill -0 "$load" 2>/dev/null; then
        ended="still running"
    else
        load_status=0
        wait "$load" || load_status=$?
        ended="ended with status $load_status"
    fi
    # Counted once the load has ended, or once it went silent.
    spun=$(refused)
    spinning=''
    [ "$spun" -lt 100000 ] || spinning=", after $spun refused submissions on the gone card"
    if grep -q 'VGSHMPROBE OK' "$out/pending.out" && [ "$ended" != "still running" ] &&
        [ -z "$spinning" ] && [ "$alive" = yes ] &&
        grep -q 'result: detached: QEMU closed' "$out/ivshmemd.out"; then
        pass death "$(sed -n 's/^VGSHMPROBE FENCE: //p' "$out/pending.out" | head -n 1); $loadscene $ended"
    else
        fail death "$loadscene $ended$spinning: $(grep 'VGSHMPROBE\|result' "$out/pending.out" "$out/ivshmemd.out" | tr '\n' ' ')"
        if [ "$ended" = "still running" ] || [ -n "$spinning" ]; then
            echo "bed-shm: death: udev: $(grep -c ' remove ' "$out/udev-death.txt") removals: $(grep ' remove .*/card[0-9]* (drm)' "$out/udev-death.txt" | tr '\n' ' ')"
            echo "bed-shm: death: session: $(since "$dead" | grep 'rach2-desktop: ' | tr '\n' ' ')"
            # shellcheck disable=SC2009 # pgrep gives no state or wait channel
            ps -e -o pid,pgid,stat,wchan,args | grep -E 'weston|rach2-session|rach2-desktop|glmark2' |
                grep -v grep | sed 's/^/bed-shm: death: process: /'
        fi
    fi
    [ "$monitor" = listening ] || echo "bed-shm: death: udev monitor $monitor" >&2
    # What is left of the load's group, so that nothing of it draws on.
    end_group "$load"
    load=''

    # The session again, headless on llvmpipe, drawing a client; with -g
    # gpu.sh has given the GPU back by now, and the session may have gone
    # on to it, or started on it at once when it came back before the old
    # session had gone, which is the fallback too.
    fbshown="headless - gl" fbgl=llvmpipe
    [ -z "$bdf" ] || which=any fbshown='' fbgl='(llvmpipe|Mesa Intel)'
    if started "$dead" "$fbshown" "$fbgl" 300 &&
        grep -q "rach2-desktop: session: its display drm $card gl is gone" "$out/services.txt" &&
        { [ -n "$bdf" ] || { drawn "$out/fallback.out" -b build:duration=2 &&
            grep -q 'GL_RENDERER: *llvmpipe' "$out/fallback.out"; }; }; then
        drew=''
        [ -n "$bdf" ] || drew="; glmark2 build $(sed -n 's/^.*FPS: *\([0-9]*\).*/\1/p' "$out/fallback.out" | head -n 1) FPS"
        if grep -q 'rach2-desktop: session: started as .* on headless - gl' "$out/services.txt"; then
            pass fallback "Weston headless on llvmpipe $took s after the death$drew"
        else
            pass fallback "Weston on the GPU given back, at once, $took s after the death"
        fi
    else
        fail fallback "$(grep 'rach2-desktop: \|GL renderer\|fatal' "$out/services.txt" | tail -n 4 | tr '\n' ' ') $(tail -n 2 "$out/fallback.out" 2>/dev/null | tr '\n' ' ')"
    fi
    cp "$out/services.txt" "$out/services-fallback.txt"

    # With -g, Rach2's driver has the GPU again once gpu.sh has ended.
    [ -n "$bdf" ] || return 0
    which=last
    back=''
    for c in /sys/class/drm/card[0-9]*; do
        case $(readlink -f "$c/device" 2>/dev/null) in */"$bdf") back=${c##*/} ;; esac
    done
    # The last start is on the card given back, whether it came there from
    # the headless session (`appeared`) or at once.
    if [ -n "$back" ] && started "$dead" "drm $back gl" "Mesa Intel" 300; then
        how="at once"
        ! grep -q "rach2-desktop: session: display drm $back gl appeared" "$out/services.txt" ||
            how="from the headless session"
        dir=/sys/kernel/debug/dri/${back#card}/crtc-0/crc
        echo auto > "$dir/control" 2>/dev/null
        # Six frames, the first two of which the new pipe may still change.
        timeout 10 head -n 6 "$dir/data" > "$out/crc.txt" 2>/dev/null
        frames=$(grep -c . "$out/crc.txt")
        crcs=$(tail -n 4 "$out/crc.txt" | awk '{ print $NF }' | sort -u)
        if [ "$frames" -eq 6 ] && [ "$(echo "$crcs" | grep -c .)" = 1 ]; then
            pass return "Weston on $back $how, $(grep 'GL renderer: ' "$out/services.txt" | tail -n 1 | sed 's/^.*GL renderer: //'), pipe A crc $crcs over 4 frames, $took s after the death"
        else
            fail return "pipe A's crc: $frames of 6 frames read, the last 4 giving $(echo "$crcs" | tr '\n' ' ')"
        fi
    else
        fail return "card '$back': $(grep 'rach2-desktop: \|GL renderer\|fatal' "$out/services.txt" | tail -n 4 | tr '\n' ' ')"
    fi
}

if [ -n "$nocopy" ]; then
    nocopy_rows
elif [ "$desk" = 1 ]; then
    desk_rows
else
    virgl_rows
fi
handoff

if [ "$bad" -eq 0 ]; then
    echo "bed-shm: result: pass"
else
    # The VM's console is lost with the machine; its last lines but the
    # backend's for each pin go with the result.
    tr -d '\r' < "$console" 2>/dev/null |
        grep -v 'backend: pin: resource' | tail -n 80 |
        sed 's/^/bed-shm: vm: /'
    echo "bed-shm: result: fail: $bad check(s)"
    exit 1
fi
