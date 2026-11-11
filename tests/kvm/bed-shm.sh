#!/bin/sh
# bed-shm.sh: Rach2's virtio-gpu over the region it shares with the test
# bed's VM, run under Rach2's KVM (tests/cases/machine.bed-shm.json).
#
#   sh bed-shm.sh -d DISK [-p PROBE] [-c CRC] [-k NOCOPY | -w [-l LOAD]]
#       [-x MEASURE [-S SCENES]] [-g BDF] [-o DIR] [-m MIB] [-n CPUS]
#
# ivshmemd attaches a region of 32 MiB to /dev/vgpu-shm, and two more
# attaches are refused; QEMU boots DISK, the VM that `bed/vm/image/vm.sh
# rach2` makes, under -accel kvm with the region on an ivshmem-doorbell of
# two vectors; once virtio-gpu has bound over the region, PROBE draws the
# display frame with virgl in the VM and reads it back, and its CRC must be
# CRC, llvmpipe's. The doorbells must have carried the round trips with no
# exit to QEMU: every doorbell the VM rang reached Rach2 through a KVM
# ioeventfd and every one Rach2 rang through an irqfd. Then QEMU is stopped
# with fenced submissions pending and killed: ivshmemd must detach with the
# VM's death, every fence must fail and a wait after the death must too.
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
# the VM, SIGHUP starting it there unless the session moved there itself
# (`session`), and the load draws under it as the desktop user while
# Rach2's driver rings the VM (`load`): LOAD is glmark2-wayland, the
# default, or the harness's scene `piglit`, `webgl` or `game`, which needs
# MEASURE. With MEASURE, the directory of bed/measure/measure.sh, each scene
# of SCENES (`glmark2`) runs on the session first, on the rung shm-virgl,
# or shm-venus for a scene written `venus:SCENE`, whose GL goes through
# Zink on Venus, and its lines are printed as figures. Then QEMU is
# stopped, four fenced submissions are left pending and QEMU is killed
# under the load: every fence must fail, the load must end and ivshmemd
# must detach (`death`); and the session must start again, headless with
# GL on llvmpipe, where glmark2 draws again (`fallback`). With -g as well, the
# session must come back to Rach2's driver on the GPU gpu.sh gave back,
# with the pipe's CRC stable (`return`).
#
# With -g the VM also has the host's PCI function BDF, the Lenovo's GPU:
# gpu.sh, beside this script, takes it from Rach2's Intel driver, starts
# QEMU with it on vfio-pci and the disk on AHCI, kills QEMU when this
# script sends it SIGUSR1, and gives the function back to the driver,
# which must pass gpu.sh's own checks once the VM has gone (`handoff`).
#
# Each check prints one line, `bed-shm: CHECK: pass` or `fail` with the
# reason, and the last line is `bed-shm: result: pass` or `fail`, after the
# VM's last 80 lines of console, `bed-shm: vm: LINE`, on a failure. The
# console of the VM, ivshmemd's output and the counters are kept in DIR.
set -u
disk='' probe=./vgshmprobe crc=6e8e217c out=/tmp/bed-shm mib=2048 cpus=2 nocopy='' bdf=''
desk=0 measure='' loadscene=glmark2 scenes=glmark2
icd=/usr/share/vulkan/icd.d/venus_icd.x86_64.json
gpush=$(dirname "$0")/gpu.sh
while getopts d:p:c:k:wl:x:S:g:o:m:n: o; do
    case $o in
        w) desk=1 ;;
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
    echo "usage: bed-shm.sh -d DISK [-p PROBE] [-c CRC] [-k NOCOPY | -w [-l LOAD]] [-x MEASURE [-S SCENES]] [-g BDF] [-o DIR] [-m MIB] [-n CPUS]" >&2
    exit 2
}
case $loadscene in
    glmark2) ;;
    piglit | webgl | game) [ -n "$measure" ] || { echo "bed-shm: -l $loadscene needs -x" >&2; exit 2; } ;;
    *) echo "bed-shm: no load $loadscene; glmark2, piglit, webgl or game" >&2; exit 2 ;;
esac
[ -z "$bdf" ] || [ -f "$gpush" ] || { echo "bed-shm: no $gpush for -g" >&2; exit 2; }
[ -z "$nocopy" ] || [ "$desk" = 0 ] || { echo "bed-shm: -k and -w are apart" >&2; exit 2; }
daemon='' qemu='' gpu='' waiter='' holder='' pid='' load='' bad=0
# gpu.sh, sent SIGTERM, kills its QEMU and gives the function back.
trap 'kill -9 $qemu $waiter $holder $pid $load 2>/dev/null; kill $daemon $gpu 2>/dev/null; wait 2>/dev/null' EXIT
rm -rf "$out"
mkdir -p "$out"
# Where the desktop's log stood before anything here ran.
offset=$(wc -c < /var/log/services 2>/dev/null || echo 0)
console=$out/console.log
[ -z "$bdf" ] || console=$out/gpu/console

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
    size=$({ wc -c < "$1"; } 2>/dev/null || echo 0):$([ -z "${3:-}" ] || busy "$3")
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
# with the function, and $qemu is QEMU's own process, from its pid file.
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
    : > "$out/qemu.out"
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
if until_line "$out/ivshmemd.out" 'QEMU connected' 60 "$qemu"; then
    last=-1 still=0
    until [ "$(info bound)" = 1 ]; do
        quiet "$console" 600 "$qemu" || break
    done
fi
ivshmemd -i > "$out/info-bound.txt" 2>&1

# figure COMMAND...: one measurement, its lines added to DIR/measure.txt
# and printed, each as `bed-shm: figure: LINE`, after `bed-shm: measuring:
# COMMAND`; Venus is selected for nocopy; ok=0 when it fails, with
# ivshmemd's counts. Every 30 s while
# it runs, ivshmemd's counts and the VM's last lines show how far it has
# got.
figure()
{
    echo "bed-shm: measuring: $*"
    VK_DRIVER_FILES=$icd "$@" > "$out/figure.out" 2>&1 &
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
            fail "$c" "virtio-gpu did not bind over the region: $(tail -n 2 "$console" "$out/qemu.out" 2>&1 | tr '\n' ' ')"
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
    VK_DRIVER_FILES=$icd "$out/nocopy" rounds 16 "$round_bytes" > "$out/rounds.out" 2>&1
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
    if grep -q '^NOCOPY OK' "$out/rounds.out"; then
        pass coherent "$(sed -n 's/^NOCOPY ROUNDS: //p' "$out/rounds.out"); $(sed -n 's/^NOCOPY DEVICE: //p' "$out/rounds.out")"
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
    VK_DRIVER_FILES=$icd "$out/nocopy" hold 1048576 > "$out/hold.out" 2>&1 &
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

# virgl_rows: the frame drawn, the doorbells, and the VM's death with
# fences pending.
virgl_rows()
{
    if [ "$(info bound)" != 1 ]; then
        fail draw "virtio-gpu did not bind over the region: $(tail -n 2 "$console" "$out/qemu.out" 2>&1 | tr '\n' ' ')"
        fail doorbell "no device"
    else
        io0=$(stat kvm_ioeventfd_signals) irq0=$(stat kvm_irqfd_raises)
        woken0=$(info woken | cut -d/ -f1) rung0=$(info rung | cut -d/ -f1)
        "$probe" draw "$crc" > "$out/draw.out" 2>&1
        io1=$(stat kvm_ioeventfd_signals) irq1=$(stat kvm_irqfd_raises)
        woken1=$(info woken | cut -d/ -f1) rung1=$(info rung | cut -d/ -f1)
        polls=$(info polls)
        ivshmemd -i > "$out/info-drawn.txt" 2>&1
        if grep -q 'VGSHMPROBE OK' "$out/draw.out"; then
            pass draw "$(sed -n 's/^VGSHMPROBE DRAW: //p' "$out/draw.out")"
        else
            fail draw "$(grep VGSHMPROBE "$out/draw.out" | tr '\n' ' ')"
        fi
        woken=$((woken1 - woken0)) rung=$((rung1 - rung0))
        io=$((io1 - io0)) irq=$((irq1 - irq0))
        echo "bed-shm: counts: rung $rung, woken $woken, polls $polls, ioeventfd $io, irqfd $irq" \
            > "$out/counts.txt"
        if [ "$(info doorbells)" = 1 ] && [ "$woken" -gt 0 ] && [ "$io" -ge "$woken" ] &&
            [ "$irq" -ge "$rung" ] && [ "$rung" -gt 0 ]; then
            pass doorbell "rung $rung, woken $woken, ioeventfd $io, irqfd $irq"
        else
            fail doorbell "$(cat "$out/counts.txt"), doorbells $(info doorbells)"
        fi
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
            fail "$c" "virtio-gpu did not bind over the region: card '$card', $(tail -n 2 "$console" "$out/qemu.out" 2>&1 | tr '\n' ' ')"
        done
        return
    fi
    sup=$(cat /run/rach2-desktop.pid 2>/dev/null)
    # The session's start is in the log by the time of the bind, or never:
    # rach2-desktop moves it at once to a card that appears after a loss.
    since "$offset" | grep -q "rach2-desktop: session: started as .* on drm $card gl" ||
        kill -HUP "$sup" 2>/dev/null
    if started "$offset" "drm $card gl" virgl 600 "$console" "$qemu"; then
        pass session "Weston on $card through virgl after $took s: $(grep 'GL renderer: ' "$out/services.txt" | tail -n 1 | sed 's/^.*GL renderer: //')"
    else
        fail session "$(grep 'rach2-desktop: \|GL renderer\|fatal\|error' "$out/services.txt" | tail -n 4 | tr '\n' ' ')"
    fi
    for scene in ${measure:+$scenes}; do
        rung=shm-virgl zink=''
        case $scene in venus:*) rung=shm-venus scene=${scene#venus:} zink="MESA_LOADER_DRIVER_OVERRIDE=zink VK_DRIVER_FILES=$icd" ;; esac
        # shellcheck disable=SC2086 # assignments
        env $desktop_env $zink sh "$measure/measure.sh" -r "$rung" -u desktop -d \
            -o "$out/measure/$rung-$scene" "$scene" > "$out/measure-$rung-$scene.out" 2>&1
        sed 's/^/bed-shm: figure: /' "$out/measure-$rung-$scene.out"
    done

    # The load, until the driver has rung the VM's queue 32 times since it
    # began to draw: glmark2 once it has named its renderer, a scene of the
    # harness from its start, for longer than the step will take.
    if [ "$loadscene" = glmark2 ]; then
        loadout=$out/glmark2.out
        glmark --run-forever -b build > "$loadout" 2>&1 &
        load=$!
        until_line "$loadout" 'GL_RENDERER' 600 "$load" || true
    else
        loadout=$out/load.out
        # shellcheck disable=SC2086 # two assignments
        env $desktop_env sh "$measure/measure.sh" -r shm-virgl -u desktop -d -t 600 \
            -o "$out/load" "$loadscene" > "$loadout" 2>&1 &
        load=$!
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

    # The VM's death under the load, with fences pending.
    dead=$(wc -c < "$services")
    kill -STOP "$qemu" 2>/dev/null
    "$probe" pending 4 > "$out/pending.out" 2>&1 &
    waiter=$!
    until_line "$out/pending.out" 'VGSHMPROBE PENDING\|VGSHMPROBE FAIL' 30 || true
    kill_vm
    wait "$waiter" 2>/dev/null
    waiter=''
    wait "$daemon" 2>/dev/null
    daemon=''
    last=-1 still=0
    while kill -0 "$load" 2>/dev/null; do
        quiet "$loadout" 300 "$load" || break
    done
    if kill -0 "$load" 2>/dev/null; then
        ended="still running"
    else
        load_status=0
        wait "$load" || load_status=$?
        ended="ended with status $load_status"
        load=''
    fi
    if grep -q 'VGSHMPROBE OK' "$out/pending.out" && [ -z "$load" ] &&
        grep -q 'result: detached: QEMU closed' "$out/ivshmemd.out"; then
        pass death "$(sed -n 's/^VGSHMPROBE FENCE: //p' "$out/pending.out" | head -n 1); $loadscene $ended"
    else
        fail death "$loadscene $ended: $(grep 'VGSHMPROBE\|result' "$out/pending.out" "$out/ivshmemd.out" | tr '\n' ' ')"
    fi

    # The session again, headless on llvmpipe, drawing a client; with -g
    # gpu.sh has given the GPU back by now, and the session may have gone
    # on to it.
    [ -z "$bdf" ] || which=any
    if started "$dead" "headless - gl" llvmpipe 300 &&
        grep -q "rach2-desktop: session: its display drm $card gl is gone" "$out/services.txt" &&
        { [ -n "$bdf" ] || { glmark -b build:duration=2 > "$out/fallback.out" 2>&1 &&
            grep -q 'GL_RENDERER: *llvmpipe' "$out/fallback.out"; }; }; then
        drew=''
        [ -n "$bdf" ] || drew="; glmark2 build $(sed -n 's/^.*FPS: *\([0-9]*\).*/\1/p' "$out/fallback.out" | head -n 1) FPS"
        pass fallback "Weston headless on llvmpipe $took s after the death$drew"
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
    if [ -n "$back" ] && started "$dead" "drm $back gl" Intel 300 &&
        grep -q "rach2-desktop: session: display drm $back gl appeared" "$out/services.txt"; then
        dir=/sys/kernel/debug/dri/${back#card}/crtc-0/crc
        echo auto > "$dir/control" 2>/dev/null
        crcs=$(timeout 10 head -n 6 "$dir/data" 2>/dev/null | tail -n 4 | awk '{ print $NF }' | sort -u)
        if [ "$(echo "$crcs" | grep -c .)" = 1 ]; then
            pass return "Weston on $back, $(grep 'GL renderer: ' "$out/services.txt" | tail -n 1 | sed 's/^.*GL renderer: //'), pipe A crc $crcs over 4 frames, $took s after the death"
        else
            fail return "pipe A's crc over 4 frames: $(echo "$crcs" | tr '\n' ' ')"
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
