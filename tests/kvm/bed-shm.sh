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
# log (`readback`); and vgshmprobe fdefer's resources, backed from the
# VM's pages at their first use, must be refused, as the backend is told
# through the VM's second serial port, and backed from the region's heap,
# mapped, refused unusable, held past the driver's cap and freed unused as
# it says (`defer`). Then, through the VM's second serial port,
# the backend holds a render target's backing 45 s while it runs, and the
# map that waits for it must succeed with no loss of the device (`held`);
# the backend is stopped in the VM, and a new render target's map, which
# waits for its backing, must end with ENODEV once the backend's heartbeat
# has stood still for 30 s (`stopped`); stopped again and killed while the
# wait sleeps, the wait must end with ENODEV at once, by the module's word
# that the backend has gone (`killed`); after each, virtio-gpu must bind
# again over the region. Then QEMU is stopped with fenced submissions
# pending and killed: ivshmemd must detach with the VM's death, every fence
# must fail and a wait after the death must too.
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
# With -w the draw, the doorbells and the death give way to the desktop session
# on the VM's GPU: once virtio-gpu has bound over the region, rach2-desktop's
# session runs Weston on its card with GL through virgl in the VM, SIGHUP
# starting it there when no Weston runs, as on the QEMU boot or on the Lenovo
# before a step, and with -g virgl must name the Intel GPU the VM draws on
# (`session`). With -g, a session started on a machine that ran none is ended
# once the GPU is back (`end`), on HUP, INT or TERM as well.
# The load draws as the desktop user while Rach2's driver rings the VM
# (`load`): LOAD is glmark2-wayland on the session, the default, or the
# harness's scene `piglit`, surfaceless on the card's render node, `webgl`
# or `game` on the session, which needs MEASURE, each for 600 s, piglit's
# list again and again; the doorbells are counted once the scene has
# started, after the harness's latency. With MEASURE, the directory of
# bed/measure/measure.sh, each scene of SCENES (`glmark2`) runs on the
# session first, on the rung shm-virgl, or shm-venus for a scene written
# `venus:SCENE`, whose GL goes through Zink on Venus, and its lines are
# printed as figures; the scene's check (`scene`) fails when measure.sh
# fails, as it does for a piglit test that did not pass, or when its wait
# ends as the load's does. measure.sh gets the VM's MIB with -M, with
# `:llvmpipe` when no -g gives the VM a GPU, and a piglit test that needs
# more is skipped: its line `bed-shm: skipped: piglit: TEST: ...` counts
# it, and the last line says how many were. While a scene runs, a sampler
# under a keeper of its own reads Rach2's MemAvailable, the VM's pages it
# maps for its buffers and QEMU's resident memory once a second, beside
# the VM's own `bed-vm: memory:` lines, and their least and most go in
# `bed-shm: memory: RUNG-SCENE: ...`. Then QEMU is stopped, four fenced
# submissions are left pending and QEMU is killed under the load, which
# must still run:
# every fence must fail, the load must end, glmark2 before Mesa's virgl
# has said the kernel refused 100000 of its submissions since the death,
# and ivshmemd must detach (`death`); and the session must
# start again, headless with GL on llvmpipe, where glmark2 draws again
# (`fallback`). With -g as well, the session must come back to Rach2's
# driver on the GPU gpu.sh gave back, with the pipe's CRC the same over four
# frames after the first two (`return`); when gpu.sh gave the GPU back
# before the old session had gone, the session starts on it at once, which
# is both its fallback and its return.
#
# Every child the script starts in the background, ivshmemd, QEMU or
# gpu.sh, each probe, load and figure and the udev monitor, runs under a
# keeper of its own, bedkeep, beside this script, built at the start: a
# child subreaper that runs it in a session of its own, signals it as the
# script asks through the keeper's FIFO and, once it ends or the script asks
# the keeper to stop, ends and reaps all it left. The script reads each
# command's end and the keeper's from the keeper's status file, signals no
# child by its pid and waits for none but through that file, each wait with
# a bound.
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
# reason, and the last line is `bed-shm: result: pass`, with `: N skipped`
# when tests were skipped, or `fail`, after the VM's last 80 lines of
# console, `bed-shm: vm: LINE`, on a failure. A QEMU that ends before
# virtio-gpu binds is named in a line `bed-shm: vm: QEMU ended before the
# bind...` with its status, gpu.sh's verdict with -g, and its messages, and
# every check that needed the bind fails with that line; a gpu.sh still
# running with no pid file of QEMU's is sent TERM through its keeper, and
# the line says so. The console of the VM, ivshmemd's output, the counters
# and the kernel's log at the end are kept in DIR.
# With -w, so are the services log's tails at and just after the VM's death.
# Any exit, by its end or by SIGTERM, SIGINT or SIGHUP, stops every
# keeper, which kills QEMU, and has gpu.sh give the function back.
#
# With -w -g -l crysis, run the two-loop Crysis flyby once the region's
# session is ready, in native/crysis.sh's private data fixture and, through
# `crysis.sh seal`, in its read-only namespace. After the figure the
# backend is stopped through the VM's second serial port, and the
# `backend: result:` lines it prints after that word are printed and must
# hold a whole account line followed by its end with status 0 (`account`);
# then the VM stops, and the same return and handoff checks apply.
# CRYSIS_LOOPS=1 selects one loop for a profile.
# BED_VM_PROFILE_PORT forwards a loopback port to a profiling VM's SSH
# server. BED_VM_PROFILE_CRC=1 asks that VM to enable the flush readback.
# BED_VM_MESA_DEBUG gives the VM's backend Mesa's MESA_DEBUG, lowercase
# words or digits joined by commas: any but `silent`, such as 1, logs each
# GL error with its call.
# BED_PROFILE_ONLY=1 returns the GPU after the figures, before the VM's
# death-under-load check.
set -u
disk='' probe=./vgshmprobe crc=6e8e217c out=/tmp/bed-shm mib=2048 cpus=2 nocopy='' bdf=''
desk=0 unplug=0 measure='' loadscene=glmark2 scenes=glmark2
icd=/usr/share/vulkan/icd.d/venus_icd.x86_64.json
gpush=$(dirname "$0")/gpu.sh keepc=$(dirname "$0")/bedkeep.c
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
    crysis)
        case ${CRYSIS_LOOPS:-2} in
            1|2) ;;
            *) echo 'bed-shm: CRYSIS_LOOPS must be 1 or 2' >&2; exit 2 ;;
        esac
        if [ "$desk" != 1 ] || [ -z "$bdf" ] || [ ! -f ./crysis.py ] || [ ! -f ./wm.py ] ||
            [ ! -f ./crysis.sh ] ||
            [ ! ./private -ef /data ] ||
            ! awk '$2 == "/data/owner" && $4 ~ /^ro(,|$)/ { ok = 1 } END { exit !ok }' /proc/mounts; then
            echo 'bed-shm: -l crysis needs -w, -g and the private Crysis mount' >&2
            exit 2
        fi
        ;;
    *) echo "bed-shm: no load $loadscene; glmark2, piglit, webgl, game or crysis" >&2; exit 2 ;;
esac
case ${BED_VM_PROFILE_PORT:-} in
    '') ;;
    *[!0-9]*)
        echo 'bed-shm: profiling ssh port must be decimal' >&2
        exit 2
        ;;
    *)
        if [ "${#BED_VM_PROFILE_PORT}" -gt 5 ] ||
            [ "$BED_VM_PROFILE_PORT" -lt 1 ] || [ "$BED_VM_PROFILE_PORT" -gt 65535 ]; then
            echo 'bed-shm: profiling ssh port must be between 1 and 65535' >&2
            exit 2
        fi
        ;;
esac
case ${BED_VM_PROFILE_CRC:-0} in
    0|1) ;;
    *) echo 'bed-shm: profiling CRC must be 0 or 1' >&2; exit 2 ;;
esac
case ${BED_VM_MESA_DEBUG:-} in
    *[!a-z0-9,]*) echo 'bed-shm: Mesa debug flags must be lowercase words, digits and commas' >&2; exit 2 ;;
esac
profile_only=${BED_PROFILE_ONLY:-0}
if [ "$profile_only" = 1 ] && { [ "$desk" != 1 ] || [ -z "$measure" ]; }; then
    echo 'bed-shm: figure-only profiling needs -w and -x' >&2
    exit 2
fi
# The GPU's MSIs reach the VM through irqfds and would count as doorbells
# Rach2 rang, so -g runs only the nocopy or the desktop rows, which count
# no irqfd.
[ -z "$bdf" ] || [ -n "$nocopy" ] || [ "$desk" = 1 ] || { echo "bed-shm: -g needs -k or -w" >&2; exit 2; }
[ -z "$bdf" ] || [ -f "$gpush" ] || { echo "bed-shm: no $gpush for -g" >&2; exit 2; }
[ -f "$keepc" ] || { echo "bed-shm: no $keepc" >&2; exit 2; }
[ -z "$nocopy" ] || [ "$desk" = 0 ] || { echo "bed-shm: -k and -w are apart" >&2; exit 2; }
[ "$unplug" = 0 ] || { [ -z "$nocopy$bdf" ] && [ "$desk" = 0 ]; } ||
    { echo "bed-shm: -u is apart from -k, -w and -g" >&2; exit 2; }
# The numbers of the keepers (keep_new), not pids, of ivshmemd, of QEMU or
# of gpu.sh, which runs it, of a probe, of the mapping's holder, of a
# nocopy figure, of the load, of a figure and of the udev monitor; vmk is
# QEMU's or gpu.sh's. qemu is QEMU's pid, which is only read, never
# signalled, and with -g qemu_start its start time, by which gpu.sh's
# keeper knows it.
daemon='' vmk='' gpu='' waiter='' holder='' pid='' load='' fig='' umon='' bad=0
qemu='' qemu_start=''
# The keeper of the memory sampler while a scene runs, and the count of the
# tests measure.sh skipped.
sampler='' skipped=0
# The keepers not yet ended, for cleanup; the keeper, bedkeep.c built into
# DIR; the number of the last keeper made; and of the last signal asked of
# a keeper.
keepers='' keep='' keepn=0 keep_seq=0
# Whether gpu.sh was started, so that cleanup checks the GPU's driver; and
# 1 once cleanup has begun, so that it runs once.
gpu_started=0 cleaned=0
# 1 once this run, with -g on a machine that ran no session, has asked for
# one, which must end once the GPU is back, at the end or on a signal.
session_owed=0
# The compositor's settings, their copy saved before the flyby, and the
# flyby's keeper, whose preparation writes them.
crysis_ini=/etc/xdg/weston/weston.ini crysis_ini_saved='' crysis_keep=''
# NOCOPY_INTEL, for nocopy: with -g, the GPU's Venus device or a failure.
intel=''
[ -z "$bdf" ] || intel=1
# The VM's memory and renderer, as measure.sh -M takes them.
vmmem=$mib
[ -n "$bdf" ] || vmmem=$mib:llvmpipe

pass() { echo "bed-shm: $1: pass${2:+: $2}"; }
fail() { echo "bed-shm: $1: fail: $2"; bad=$((bad + 1)); }

# keep_new NAME: make the files of a new keeper, numbered keepn, of the
# command NAME names in the lines about it, add it to keepers, and set kb
# to their base, DIR/keep-N, for `"$keep" "$$" "$kb" COMMAND... <> "$kb.ctl"
# &`, where $$, the script's pid, is the keeper's parent's, so that the
# keeper sees the script's death however soon it comes. BASE.ctl is the
# keeper's control FIFO, its standard input, opened for reading and writing
# so that the keeper never sees its end of file while the script, which
# holds no end of it, runs; BASE.status is where the keeper writes `started
# KEEPER COMMAND`, `ended STATUS` once its command has ended, `signalled
# SEQ` or `unsignalled SEQ WHY` for each signal asked, `left PROCESS` for
# each process that outlived KILL, and `done STATUS` once it has reaped
# all, just before it exits; BASE.stop, once made, stops a keeper that opens
# its FIFO after the word was written; BASE.name holds NAME.
keep_new()
{
    keepn=$((keepn + 1))
    kb=$out/keep-$keepn
    rm -f "$kb.ctl" "$kb.status" "$kb.stop"
    echo "$1" > "$kb.name"
    keepers="$keepers $keepn"
    mkfifo "$kb.ctl" || echo "bed-shm: keeper $keepn: no FIFO; the keeper refuses to start" >&2
}

# keep_running N: whether the command of keeper N has not ended, from its
# status file, whatever the keeper does about what it left: no `ended`
# line, and no `done` of a keeper that refused to start.
keep_running()
{
    [ -n "$1" ] && ! grep -qE '^(ended|done) ' "$out/keep-$1.status" 2>/dev/null
}

# keep_started N: wait, 100 tries of 0.1 s at most, for keeper N's
# `started` line, or its `done` line when it refused to start.
keep_started()
{
    n=100
    until grep -qE '^(started|done) ' "$out/keep-$1.status" 2>/dev/null; do
        [ "$n" -gt 0 ] || return 1
        sleep 0.1
        n=$((n - 1))
    done
}

# keep_pid N: the pid of keeper N's command, from its `started` line, only
# to be read: its state, processor time, wait channel and what it holds.
keep_pid() { sed -n 's/^started [0-9]* \([0-9]*\)$/\1/p' "$out/keep-$1.status" 2>/dev/null | head -n 1; }

# keep_ended N: the status of keeper N's command, from its `ended` line.
keep_ended() { sed -n 's/^ended //p' "$out/keep-$1.status" 2>/dev/null | head -n 1; }

# keep_wait N TRIES: wait, TRIES tries of 0.1 s at most, until the command
# of keeper N has ended; 1 when it still runs.
keep_wait()
{
    n=$2
    while keep_running "$1"; do
        [ "$n" -gt 0 ] || return 1
        sleep 0.1
        n=$((n - 1))
    done
}

# proc_of PID: `STATE START` of process PID, its state and its start time
# in clock ticks since the boot, from its stat line, whose command name may
# hold spaces and parentheses; nothing once it has gone.
proc_of() { awk '{ sub(/^.*\) /, ""); print $1, $20 }' "/proc/$1/stat" 2>/dev/null; }

# keep_signal N SIG [PID START]: have keeper N send SIG, a name such as
# STOP, to its command, which it has not reaped, so that its pid is no
# other process's; or to PID, a child of the command whose start time is
# START, which the keeper holds still by stopping the command meanwhile.
# Waits, 100 tries of 0.1 s, for the keeper's answer; 1, with keep_why,
# when the signal was not sent, or the command has ended.
keep_signal()
{
    keep_why=''
    keep_started "$1" || { keep_why='the keeper never started'; return 1; }
    keep_running "$1" || { keep_why='the command has ended'; return 1; }
    keep_seq=$((keep_seq + 1))
    ks=$out/keep-$1.status
    printf 'signal %s %s%s\n' "$keep_seq" "$2" "${3:+ $3 ${4:-0}}" 1<> "$out/keep-$1.ctl"
    n=100
    while :; do
        ! grep -q "^signalled $keep_seq\$" "$ks" || return 0
        keep_why=$(sed -n "s/^unsignalled $keep_seq //p" "$ks")
        [ -z "$keep_why" ] || return 1
        if grep -q '^ended ' "$ks"; then
            keep_why='the command has ended'
            return 1
        fi
        if [ "$n" -le 0 ]; then
            keep_why='the keeper did not answer in 10 s'
            return 1
        fi
        sleep 0.1
        n=$((n - 1))
    done
}

# keep_stop N [WORD]: ask keeper N to end its command and all it left,
# with WORD `kill` by KILL at once: make BASE.stop, then write WORD, `stop`
# by default, on its FIFO, opened for reading and writing, which never
# waits; a keeper that has its FIFO open reads the word, and one that opens
# it later finds BASE.stop. Nothing is written once the keeper is done and
# its FIFO removed.
keep_stop()
{
    : > "$out/keep-$1.stop"
    [ ! -p "$out/keep-$1.ctl" ] || printf '%s\n' "${2:-stop}" 1<> "$out/keep-$1.ctl"
}

# keep_said N: the line about keeper N not done after its stop: its
# command's name, the two pids and what the keeper named left, with, for
# ivshmemd, the region's state as the kernel reports it, since one stuck in
# its detach keeps the region, and for QEMU or gpu.sh, KVM's machines and
# VFIO's devices and pins.
keep_said()
{
    ks_name=$(cat "$out/keep-$1.name" 2>/dev/null)
    ks_left=$(sed -n 's/^left //p' "$out/keep-$1.status" 2>/dev/null | tr '\n' ';')
    ks_who=$(sed -n 's/^started \([0-9]*\) \([0-9]*\)$/keeper \1, process \2/p' "$out/keep-$1.status" 2>/dev/null | head -n 1)
    ks_more=''
    case $ks_name in
        ivshmemd) ks_more="; $(ivshmemd -i 2>&1 | head -n 1)" ;;
        QEMU | gpu.sh)
            ks_more="; $(awk '$1 ~ /^(kvm_vms|vfio_devices|vfio_dma_pages)$/ { printf "%s %s ", $1, $2 }' /proc/stat)"
            ;;
    esac
    if [ -n "$ks_who" ]; then
        echo "$ks_name ($ks_who) not done 40 s after its stop: ${ks_left:-it named nothing left}$ks_more"
    else
        echo "$ks_name (keeper $1) never started$ks_more"
    fi
}

# keep_end [-k] N...: end keepers N..., each of which gives its command and
# every process the command left TERM, and KILL 20 s later, or with -k KILL
# at once, and reaps them all (keep_stop); then 400 tries of 0.1 s in all,
# past a keeper's own 30 s, for their `done` lines. A keeper done leaves
# keepers, and its status, the command's, is in keep_status, the last one's
# given. A keeper not done by then holds a process that outlived KILL, or
# never started: it stays in keepers for cleanup, and the return is 1, with
# keep_said's line for each in keep_left.
keep_end()
{
    keep_word='stop'
    [ "${1:-}" != -k ] || { keep_word='kill'; shift; }
    keep_left='' keep_status='' keep_undone=''
    for k in "$@"; do
        [ -z "$k" ] || keep_stop "$k" "$keep_word"
    done
    n=400
    while :; do
        keep_undone=''
        for k in "$@"; do
            [ -z "$k" ] || grep -q '^done ' "$out/keep-$k.status" 2>/dev/null || keep_undone="$keep_undone $k"
        done
        [ -n "$keep_undone" ] && [ "$n" -gt 0 ] || break
        sleep 0.1
        n=$((n - 1))
    done
    for k in "$@"; do
        [ -n "$k" ] || continue
        case " $keep_undone " in
            *" $k "*)
                keep_left="${keep_left:+$keep_left; }$(keep_said "$k")"
                ;;
            *)
                keep_status=$(sed -n 's/^done //p' "$out/keep-$k.status" | head -n 1)
                keepers=$(for j in $keepers; do [ "$j" = "$k" ] || printf '%s ' "$j"; done)
                rm -f "$out/keep-$k.ctl"
                ;;
        esac
    done
    [ -z "$keep_undone" ]
}

# await N: wait for the command of keeper N, a figure or the flyby, and
# then end the keeper (keep_end), with the command's status in
# await_status; as the load step waits (load_wait), the wait ends at once
# when the device is gone, or once the VM's doorbells have stood still for
# 600 s, with the reason in await_why. Returns 0 only when the command
# ended by itself with 0.
await()
{
    await_why='' await_status=''
    binds0=$(info binds) lost0=$(klog_count 'vgpu-shm: lost: ') last=-1 still=0
    while load_wait 600 "$1"; do :; done
    if [ -n "$load_gone" ]; then
        await_why=$load_gone
    elif keep_running "$1"; then
        await_why='no doorbell for 600 s'
    fi
    if keep_end "$1"; then
        await_status=$keep_status
    else
        await_status=running await_why="${await_why:+$await_why; }$keep_left"
    fi
    [ -z "$await_why" ] && [ "$await_status" = 0 ]
}

# holding PID [KIND]: what process PID is and what it holds, for the line
# about a child that outlived its wait: its state, wait channel and command,
# the devices it has open and, with KIND ivshmemd, the region as the kernel
# reports it, or with KIND qemu, KVM's machines and VFIO's devices and pins.
holding()
{
    # shellcheck disable=SC2009 # pgrep gives no state or wait channel
    held_ps=$(ps -e -o pid,stat,wchan,args | awk -v p="$1" '$1 == p { $1 = ""; sub(/^ +/, ""); print; exit }')
    held_dev=$(for f in /proc/"$1"/fd/*; do readlink "$f"; done 2>/dev/null | grep '^/dev/' | sort -u | tr '\n' ' ' | sed 's/ $//')
    held_more=''
    case ${2:-} in
        ivshmemd) held_more="; $(ivshmemd -i 2>&1 | head -n 1)" ;;
        qemu) held_more="; $(awk '$1 ~ /^(kvm_vms|vfio_devices|vfio_dma_pages)$/ { printf "%s %s ", $1, $2 }' /proc/stat)" ;;
    esac
    echo "${held_ps:-not in ps}; devices: ${held_dev:-none}$held_more"
}

# crysis_restore_ini: put back the compositor's settings before the GPU
# returns and the native session starts, only once the flyby's keeper is
# done, having reaped every process its command left, since the flyby's
# preparation writes the file. 1, with the reason in crysis_why and the
# saved copy kept for a later call, while the keeper is not done or the
# copy fails. A session that started on the changed file first, as when
# QEMU ended early, crysis-shm.sh starts again at its end.
crysis_restore_ini()
{
    crysis_why=''
    [ -n "$crysis_ini_saved" ] || return 0
    if [ -n "$crysis_keep" ] && ! grep -q '^done ' "$out/keep-$crysis_keep.status" 2>/dev/null; then
        crysis_why="the flyby's keeper is not done, so its preparation may still write weston.ini; left changed, saved as $crysis_ini_saved"
        return 1
    fi
    if ! cp -p "$crysis_ini_saved" "$crysis_ini"; then
        crysis_why="could not copy $crysis_ini_saved to $crysis_ini"
        return 1
    fi
    crysis_ini_saved=''
}

# session_end: end the session this run started, as crysis.sh's
# session_restore does when none ran before: USR2 to rach2-desktop, then
# wait, 600 tries of 0.1 s at most, until a turn of its loop begun after the
# signal has run whole, which the second `sleep 1` seen since shows, and
# neither Weston nor the session's script is left.
session_end()
{
    sup=$(cat /run/rach2-desktop.pid 2>/dev/null) && [ -n "$sup" ] || return 1
    kill -USR2 "$sup" 2>/dev/null || return 1
    tries=0 turn='' turns=0
    while :; do
        pause=$(pgrep -P "$sup" -fx 'sleep 1' | head -n 1)
        if [ -n "$pause" ] && [ "$pause" != "$turn" ]; then
            turn=$pause turns=$((turns + 1))
        fi
        { [ "$turns" -ge 2 ] || ! kill -0 "$sup" 2>/dev/null; } &&
            ! pgrep -x weston > /dev/null && ! pgrep -f /usr/libexec/rach2-session > /dev/null &&
            return 0
        tries=$((tries + 1))
        [ "$tries" -lt 600 ] || return 1
        sleep 0.1
    done
}

# cleanup [end]: the run's one way out, at its normal end, which passes
# `end`, and on any signal's exit: a fixed sequence, run once and never
# entered again, which leaves the GPU and the session as the run found
# them. Each step prints its outcome as a check of its own, and a step that
# did not complete fails the run. Every wait has a bound, given with its
# step.
cleanup()
{
    cleanup_status=$?
    # Step 1, signals: ignore every signal a person or a parent may send from
    # here on, and mark cleanup begun, so that no trap, retry or second call
    # runs a step again.
    trap '' HUP INT TERM QUIT USR1 USR2 PIPE ALRM
    [ "$cleaned" = 0 ] || return 0
    cleaned=1
    cleanup_bad=$bad

    # Step 2, children: every keeper but gpu.sh's ends all of its command,
    # TERM, KILL 20 s later, and reaps all, within 40 s (keep_end): first
    # those of the loads, figures, probes and the udev monitor, together,
    # then QEMU's, unless gpu.sh runs it, and ivshmemd's. A keeper not done
    # is named with what it reported left, which it goes on killing and
    # never reaps, so that no other process takes their pids. QEMU under
    # gpu.sh is gpu.sh's to kill, in step 3, before its give-back.
    survivors=''
    first=$(for k in $keepers; do
        case $k in "$gpu" | "$vmk" | "$daemon") ;; *) printf '%s ' "$k" ;; esac
    done)
    # shellcheck disable=SC2086 # a list of keepers' numbers
    keep_end $first || survivors=$keep_left
    # The flyby's preparation writes weston.ini, so the saved file goes back
    # only once the flyby's keeper is done (crysis_restore_ini).
    crysis_restore_ini || fail settings "$crysis_why"
    load='' fig='' waiter='' holder='' pid='' umon='' sampler=''
    vm=$vmk
    [ -z "$gpu" ] || vm=''
    # shellcheck disable=SC2086 # two keepers' numbers
    keep_end $vm $daemon || survivors="${survivors:+$survivors; }$keep_left"
    daemon=''
    [ -n "$gpu" ] || vmk=''
    # The control port: the script's end of the VM's second serial port
    # closed and its two pipes removed, QEMU, the other end, having gone,
    # and bed-vm-control with its VM.
    exec 7<&- 2>/dev/null
    rm -f "$out/ctl.in" "$out/ctl.out"
    if [ -z "$survivors" ]; then
        pass stop "every keeper but gpu.sh's has ended all its command left (bounds: 40 s for the keepers, 20 s to TERM and 10 s to KILL their processes)"
    else
        fail stop "still running: $survivors"
    fi

    # Step 3, GPU: with -g, one TERM to gpu.sh, from its keeper, never a
    # second, which kills QEMU and gives the function back with every signal
    # ignored; 60 s for it, since i915's bind resets the GPU, and then its
    # keeper must be done and the function on i915. A gpu.sh still running
    # is left to its keeper, which on the script's exit sends it TERM, which
    # it ignores, and KILL 20 s later.
    gpu_back=1 gpu_why=''
    if [ "$gpu_started" = 1 ]; then
        gpu_back=0
        if [ -n "$gpu" ]; then
            keep_signal "$gpu" TERM || :
            if keep_wait "$gpu" 600 && keep_end "$gpu"; then
                gpu='' vmk='' qemu=''
            elif keep_running "$gpu"; then
                gpu_why="gpu.sh $(keep_pid "$gpu") still running 60 s after its TERM: QEMU ${qemu:-unknown}${qemu:+: $(holding "$qemu" qemu)}"
            else
                gpu_why="gpu.sh ended, but its keeper is not done: $keep_left"
            fi
        fi
        drv=$(readlink "/sys/bus/pci/devices/$bdf/driver" 2>/dev/null)
        drv=${drv##*/}
        if [ -n "$gpu_why" ]; then
            fail gpu "$gpu_why"
        elif [ "$drv" = i915 ]; then
            gpu_back=1
            pass gpu "$bdf is on i915 (bound: 60 s for gpu.sh's return)"
        else
            fail gpu "$bdf is on ${drv:-no driver}, not i915, after gpu.sh's end: $(grep '^gpu: ' "$out/gpu.log" 2>/dev/null | tail -n 2 | tr '\n' ' ')"
        fi
    fi

    # Step 4, session: once the GPU is on i915 and nothing of the run is
    # left, end the session the run started on a machine that ran none,
    # and start none; else leave it alone, with the reason.
    if [ "$session_owed" = 1 ]; then
        if [ "$gpu_back" != 1 ]; then
            fail end "the session this run started is left running, since $bdf is not back on i915"
        elif [ -n "$survivors" ]; then
            fail end "the session this run started is left running, since processes of the run are still running"
        elif session_end; then
            pass end 'the session this run started has ended, as none ran before (bound: 60 s)'
        else
            fail end "the session this run started is still running 60 s after USR2: weston [$(pgrep -x weston | tr '\n' ' ')]"
        fi
        session_owed=0
    fi

    # Step 5, outcome: the lines above are the record. At the normal end
    # the result counts them; on an exit, a step that failed turns a status
    # of 0 into 1, and a signal's exit keeps its status.
    [ "${1:-}" = end ] || [ "$bad" = "$cleanup_bad" ] || [ "$cleanup_status" != 0 ] || exit 1
}
trap cleanup EXIT
trap 'trap "" HUP INT TERM; exit 129' HUP
trap 'trap "" HUP INT TERM; exit 130' INT
trap 'trap "" HUP INT TERM; exit 143' TERM
rm -rf "$out"
mkdir -p "$out"
# The keeper of each background child, built with the root's cc, as
# native/bed.sh builds crcmodes; a run that cannot keep its children runs
# none.
keep=$out/bedkeep
if ! cc -std=c99 -static -O2 -o "$keep" "$keepc" > "$out/bedkeep.build" 2>&1; then
    fail keeper "bedkeep did not build: $(tail -n 3 "$out/bedkeep.build" | tr '\n' ' ')"
    cleanup end
    echo "bed-shm: result: fail: $bad check(s)"
    exit 1
fi
# Where the desktop's log stood before anything here ran.
services=/var/log/services
offset=$(command stat -c %s "$services" 2>/dev/null || echo 0)
# Whether a desktop session ran before anything here, to leave as found.
weston_found=0
! pgrep -x weston > /dev/null || weston_found=1
console=$out/console.log qemu_err=$out/qemu.out
[ -z "$bdf" ] || console=$out/gpu/console qemu_err=$out/gpu/qemu.err

# stat NAME: a count of /proc/stat.
stat() { awk -v n="$1" '$1 == n { print $2 }' /proc/stat; }

# info FIELD: a field of ivshmemd -i's report, as `name value`.
info() { ivshmemd -i | tr ',' '\n' | sed -n "s/^.* $1 \([0-9/]*\).*$/\1/p" | head -n 1; }

# busy PID: the processor time of process PID, in ticks, which grows while
# a guest of it runs and stays while the guest waits halted.
busy() { awk '{ print $14 + $15 }' "/proc/$1/stat" 2>/dev/null || echo 0; }

# vm_alive: whether QEMU runs: the command of its keeper, or with -g,
# gpu.sh's child, by its pid and start time, which are only read.
vm_alive()
{
    if [ -n "$gpu" ]; then
        vm_proc=$(proc_of "$qemu")
        [ -n "$qemu" ] && [ "${vm_proc#* }" = "$qemu_start" ] && [ "${vm_proc%% *}" != Z ]
    else
        [ -n "$vmk" ] && keep_running "$vmk"
    fi
}

# alive N: whether the command of keeper N runs, or with N `vm`, QEMU.
alive()
{
    if [ "$1" = vm ]; then
        vm_alive
    else
        keep_running "$1"
    fi
}

# quiet FILE SILENCE [N]: one second of a wait, which `last=-1 still=0`
# starts; it fails once the command of keeper N, or with N `vm` QEMU, has
# ended, or once for SILENCE seconds FILE has not grown and that command
# has used no processor time.
quiet()
{
    [ -z "${3:-}" ] || alive "$3" || return 1
    sleep 1
    quiet_pid=''
    case ${3:-} in
        '') ;;
        vm) quiet_pid=$qemu ;;
        *) quiet_pid=$(keep_pid "$3") ;;
    esac
    size=$(command stat -c %s "$1" 2>/dev/null || echo 0):$([ -z "$quiet_pid" ] || busy "$quiet_pid")
    if [ "$size" = "$last" ]; then
        still=$((still + 1))
        [ "$still" -lt "$2" ] || return 1
    else
        last=$size still=0
    fi
}

# load_wait SILENCE N [FILE]: one second of the load step's wait, which
# `last=-1 still=0` starts, with binds0 and lost0 the transport's binds and
# the kernel's `vgpu-shm: lost:` lines at the step's start. It fails once
# the command of keeper N has ended; at once once the device is gone, a
# loss logged, the transport unbound or bound again, since no doorbell can
# come after that, with load_gone saying why; or once for SILENCE seconds
# the VM's doorbells have not been rung, whatever processor time the load
# uses, since a load on another renderer may be busy and ring nothing; with
# FILE, nor a line added to FILE.
load_wait()
{
    load_gone=''
    keep_running "$2" || return 1
    sleep 1
    if [ "$(klog_count 'vgpu-shm: lost: ')" -gt "$lost0" ]; then
        load_gone="the device is gone: $(dmesg | grep 'vgpu-shm: lost: ' | tail -n 1)"
        return 1
    fi
    load_bound=$(info bound) load_binds=$(info binds)
    if [ "$load_bound" != 1 ] || [ "$load_binds" != "$binds0" ]; then
        load_gone="the device is gone: bound $load_bound, binds $binds0 then $load_binds"
        return 1
    fi
    size=$(info rung | cut -d/ -f1)
    [ -z "${3:-}" ] || size=$size:$(wc -l < "$3" 2>/dev/null || echo 0)
    if [ "$size" = "$last" ]; then
        still=$((still + 1))
        [ "$still" -lt "$1" ] || return 1
    else
        last=$size still=0
    fi
}

# until_line FILE PATTERN SILENCE [N]: wait until FILE has a line
# matching PATTERN, while quiet FILE SILENCE N holds.
until_line()
{
    last=-1 still=0
    until grep -q "$2" "$1" 2>/dev/null; do
        quiet "$1" "$3" "${4:-}" || return 1
    done
}

# fences_pending: wait for all four submissions while the VM is stopped.
# The probe must finish submitting even if it keeps logging or using CPU.
fences_pending()
{
    pending_left=30
    until grep -qx 'VGSHMPROBE PENDING 4' "$out/pending.out"; do
        [ "$pending_left" -gt 0 ] && keep_running "$waiter" &&
            ! grep -q '^VGSHMPROBE FAIL' "$out/pending.out" || return 1
        sleep 1
        pending_left=$((pending_left - 1))
    done
}

# probe_ready N: wait until `vgshmprobe pending`, the command of keeper N,
# has said its render target is made and has stopped itself, as quiet
# pending.out 30 N allows; the target's backing is the VM's, which a
# stopped VM cannot give.
probe_ready()
{
    until_line "$out/pending.out" 'VGSHMPROBE READY\|VGSHMPROBE FAIL' 30 "$1" || return 0
    last=-1 still=0
    ready_pid=$(keep_pid "$1")
    until [ "$(sed -n 's/^State:[[:space:]]*\(.\).*/\1/p' "/proc/$ready_pid/status" 2>/dev/null)" = T ]; do
        quiet "$out/pending.out" 30 "$1" || return 0
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
keep_new ivshmemd
"$keep" "$$" "$kb" ivshmemd -s 32M "$out/ivs.sock" <> "$kb.ctl" > "$out/ivshmemd.out" 2>&1 &
daemon=$keepn
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
# with the function, and $qemu is QEMU's own process, from its pid file,
# with its start time; QEMU's messages are then in gpu.sh's DIR/gpu/qemu.err.
set --
if [ -n "${BED_VM_PROFILE_PORT:-}" ]; then
    set -- -netdev "user,id=bedprof,restrict=on,hostfwd=tcp:127.0.0.1:$BED_VM_PROFILE_PORT-:22" \
        -device virtio-net-pci,netdev=bedprof \
        -fw_cfg "name=opt/rach2/vgpu-profile,string=${BED_VM_PROFILE_CRC:-0}"
fi
# QEMU's option parser reads a doubled comma as one comma.
[ -z "${BED_VM_MESA_DEBUG:-}" ] ||
    set -- "$@" -fw_cfg \
        "name=opt/rach2/mesa-debug,string=$(printf '%s' "$BED_VM_MESA_DEBUG" | sed 's/,/,,/g')"
# The VM's second serial port, ttyS1, a pipe whose input this script
# keeps open, read and written, so that a word sent never waits, even with
# QEMU gone: bed-vm-control in the VM does each word to the backend
# (vm_ctl). With -g too, where the flyby's end stops the backend through it.
mkfifo "$out/ctl.in" "$out/ctl.out"
exec 7<> "$out/ctl.in"
set -- "$@" -chardev pipe,path="$out/ctl",id=ctl -device isa-serial,chardev=ctl,index=1
if [ -z "$bdf" ]; then
    keep_new QEMU
    TMPDIR=$out "$keep" "$$" "$kb" qemu-system-x86_64 -accel kvm -machine q35,kernel-irqchip=split -cpu host -smp "$cpus" \
        -m "${mib}M" -nodefaults -no-reboot -display none -monitor none \
        -drive file="$disk",if=none,id=vm,format=raw,snapshot=on \
        -device virtio-blk-pci,drive=vm,ioeventfd=off \
        -chardev socket,path="$out/ivs.sock",id=ivs -device ivshmem-doorbell,chardev=ivs,vectors=2 \
        -serial file:"$console" "$@" <> "$kb.ctl" > "$out/qemu.out" 2>&1 &
    vmk=$keepn
    keep_started "$vmk" || :
    qemu=$(keep_pid "$vmk")
else
    keep_new gpu.sh
    TMPDIR=$out "$keep" "$$" "$kb" bash "$gpush" -d "$bdf" -m "$mib" -s "$cpus" -w 0 -o "$out/gpu" -D "$disk" -- \
        -pidfile "$out/qemu.pid" \
        -chardev socket,path="$out/ivs.sock",id=ivs -device ivshmem-doorbell,chardev=ivs,vectors=2 \
        "$@" <> "$kb.ctl" > "$out/gpu.log" 2>&1 &
    gpu=$keepn vmk=$keepn gpu_started=1
    last=-1 still=0
    until [ -s "$out/qemu.pid" ]; do
        quiet "$out/gpu.log" 120 "$gpu" || break
    done
    qemu=$(cat "$out/qemu.pid" 2>/dev/null)
    case $qemu in '' | *[!0-9]*) qemu='' ;; esac
    [ -z "$qemu" ] || qemu_start=$(proc_of "$qemu" | cut -d' ' -f2)
fi

# vm_signal SIG: have QEMU's keeper send SIG to QEMU or, with -g, gpu.sh's
# keeper send it to gpu.sh's QEMU, the process of the pid file that started
# at qemu_start (keep_signal).
vm_signal()
{
    if [ -n "$gpu" ]; then
        [ -n "$qemu" ] || { keep_why='no pid of QEMU'; return 1; }
        keep_signal "$gpu" "$1" "$qemu" "$qemu_start"
    else
        [ -n "$vmk" ] || { keep_why='no QEMU'; return 1; }
        keep_signal "$vmk" "$1"
    fi
}

# stop_vm: kill QEMU, through gpu.sh when it owns the VM, with SIGUSR1.
stop_vm()
{
    if [ -n "$gpu" ]; then
        keep_signal "$gpu" USR1
    else
        vm_signal KILL
    fi
}

# wait_vm: wait for QEMU's end or gpu.sh's, which also gives the function
# back, and end its keeper. gpu.sh waits for its QEMU first, so the wait for
# either is bounded at 600 tries of 0.1 s, and one still running then is
# left, with QEMU, to cleanup, which names what QEMU holds. gpu.sh's status
# is in gpu_status.
wait_vm()
{
    if [ -n "$gpu" ]; then
        if keep_wait "$gpu" 600; then
            gpu_status=$(keep_ended "$gpu")
            # A QEMU that outlived gpu.sh is its keeper's, and cleanup's.
            keep_end "$gpu" || echo "bed-shm: vm: left after gpu.sh's end: $keep_left" >&2
            gpu=''
        else
            gpu_status="still running 60 s after QEMU's kill"
            return 0
        fi
    elif [ -n "$vmk" ]; then
        # A QEMU stuck in the kernel after KILL is left to cleanup.
        keep_wait "$vmk" 600 || return 0
        keep_end "$vmk" || echo "bed-shm: vm: left after QEMU's end: $keep_left" >&2
    fi
    qemu='' vmk=''
}

# kill_vm: kill and reap the VM, and wait for any GPU handoff.
kill_vm()
{
    stop_vm
    wait_vm
}

# death_children: give the fence probe and ivshmemd 60 tries to exit after
# QEMU is killed, and end their keepers once they have. A task stuck in the
# kernel may still be there after KILL, so the failure must not wait for
# its exit: the keeper of one still running is asked to KILL it, and is
# left to cleanup, which waits for it.
death_children()
{
    left=60 death_error=''
    while keep_running "$waiter" || keep_running "$daemon"; do
        [ "$left" -gt 0 ] || break
        sleep 1
        left=$((left - 1))
    done
    if keep_running "$waiter"; then
        death_error="fence probe still running: $(holding "$(keep_pid "$waiter")")"
        keep_stop "$waiter" kill
    else
        st=$(keep_ended "$waiter")
        [ "$st" = 0 ] || death_error="fence probe status ${st:-unknown}"
        keep_end "$waiter" || death_error="${death_error:+$death_error, }$keep_left"
    fi
    waiter=''
    if keep_running "$daemon"; then
        death_error="${death_error:+$death_error, }ivshmemd still running: $(holding "$(keep_pid "$daemon")" ivshmemd)"
        keep_stop "$daemon" kill
    else
        st=$(keep_ended "$daemon")
        [ "$st" = 0 ] || death_error="${death_error:+$death_error, }ivshmemd status ${st:-unknown}"
        keep_end "$daemon" || death_error="${death_error:+$death_error, }$keep_left"
        daemon=''
    fi
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
if until_line "$out/ivshmemd.out" 'QEMU connected' 60 vm; then
    last=-1 still=0 registered=0
    until [ "$(info bound)" = 1 ]; do
        quiet "$console" 600 vm || break
        [ "$unplug" = 1 ] || continue
        dmesg | tail -n "+$((klog_from + 1))" | grep -q 'drm: virtio_gpu: card' || continue
        registered=$((registered + 1))
        [ "$registered" -lt 60 ] || break
    done
fi
ivshmemd -i > "$out/info-bound.txt" 2>&1
# vm_end: how QEMU ended when it ended before the bind, which every row then
# fails with: with -g, gpu.sh's verdict, which gives QEMU's status, once
# gpu.sh has given the function back; otherwise QEMU's own status. QEMU's
# messages and gpu.sh's memory lines follow it. A gpu.sh that still runs
# with no pid file of QEMU's, whose wait gave up, failed to start the VM,
# and since it waits on a QEMU it started for ever (-w 0), its keeper sends
# it TERM, on which it kills QEMU and gives the function back.
vm_end=''
if [ "$(info bound)" != 1 ] && ! vm_alive; then
    if [ -n "$gpu" ]; then
        how='QEMU ended before the bind'
        if [ -z "$qemu" ] && keep_running "$gpu"; then
            how='QEMU wrote no pid file and gpu.sh was stopped'
            keep_signal "$gpu" TERM || how="$how, though its keeper sent no TERM: $keep_why"
        fi
        wait_vm
        vm_end="$how: gpu.sh status $gpu_status: $(grep '^gpu: result:\|^gpu: stopped:' "$out/gpu.log" | tail -n 2 | tr '\n' ' ')"
    elif [ -n "$vmk" ]; then
        keep_wait "$vmk" 600 || :
        vm_status=$(keep_ended "$vmk")
        wait_vm
        vm_end="QEMU ended before the bind with status ${vm_status:-unknown}"
    fi
    if [ -n "$vm_end" ]; then
        echo "bed-shm: vm: $vm_end"
        tail -n 20 "$qemu_err" 2>/dev/null | sed 's/^/bed-shm: vm: qemu: /'
        [ -z "$bdf" ] || grep '^gpu: memory:' "$out/gpu.log" | sed 's/^/bed-shm: vm: /'
    fi
fi
# With -u, the console's place on the region's card, inside the bind.
if [ "$unplug" = 1 ]; then
    dmesg | tail -n "+$((klog_from + 1))" > "$out/dmesg-bind.txt"
    card=$(sed -n 's/^.*drm: virtio_gpu: \(card[0-9]*\)$/\1/p' "$out/dmesg-bind.txt" | tail -n 1)
    unbound=''
    if [ "$(info bound)" != 1 ] && [ -n "$card" ]; then
        # The bind has not returned: where the transport's thread sleeps,
        # then whether an open of the card, which reaps the device's
        # queues, lets it go on.
        t=$(pgrep -x vgpu-shm | head -n 1)
        unbound="vgpu-shm sleeps in $(cat "/proc/$t/wchan" 2>/dev/null || echo '?')"
        : <> "/dev/dri/$card"
        n=60
        until [ "$(info bound)" = 1 ] || [ "$n" -eq 0 ]; do
            sleep 1
            n=$((n - 1))
        done
        if [ "$(info bound)" = 1 ]; then
            unbound="$unbound, and the bind returned once $card was opened, whose open reaps the queues"
        else
            unbound="$unbound, and an open of $card did not end the bind"
        fi
        dmesg | tail -n "+$((klog_from + 1))" > "$out/dmesg-bind.txt"
    fi
    if [ -z "$unbound" ] && [ "$(info bound)" = 1 ] && [ -n "$card" ] &&
        grep -q "console: $card: a native display" "$out/dmesg-bind.txt" &&
        ! grep -q "console: $card: screen not set" "$out/dmesg-bind.txt"; then
        pass console "virtio-gpu bound over the region as $card with the console placed on it"
    else
        fail console "${unbound:+$unbound; }bound $(info bound), card '$card': $(grep 'console: \|virtio_gpu: command' "$out/dmesg-bind.txt" | tail -n 3 | tr '\n' ' ')"
    fi
fi

# why_unbound: why virtio-gpu has not bound: how QEMU ended, when it ended
# before the bind, or else the last lines of the VM's console and of QEMU's
# messages.
why_unbound()
{
    if [ -n "$vm_end" ]; then
        echo "$vm_end"
    else
        tail -n 2 "$console" "$qemu_err" 2>&1 | tr '\n' ' '
    fi
}

# figure COMMAND...: one measurement, its lines added to DIR/measure.txt
# and printed, each as `bed-shm: figure: LINE`, after `bed-shm: measuring:
# COMMAND`; Venus is selected for nocopy; ok=0 when it fails, with
# ivshmemd's counts. It is waited for as the load step waits (load_wait),
# with its output as progress too: the wait ends at once when the device is
# gone, or once for 600 s the VM's doorbells have not been rung and the
# figure has printed no line, and its keeper is then ended. Every 30 s
# while it runs, ivshmemd's counts and the VM's last lines show how far it
# has got.
figure()
{
    echo "bed-shm: measuring: $*"
    keep_new figure
    VK_DRIVER_FILES=$icd NOCOPY_INTEL=$intel "$keep" "$$" "$kb" "$@" <> "$kb.ctl" > "$out/figure.out" 2>&1 &
    pid=$keepn s=0 figure_why=''
    binds0=$(info binds) lost0=$(klog_count 'vgpu-shm: lost: ') last=-1 still=0
    while load_wait 600 "$pid" "$out/figure.out"; do
        s=$((s + 1))
        [ $((s % 30)) -ne 0 ] && continue
        echo "bed-shm: running: $s s: $(ivshmemd -i 2>&1 | tr '\n' ' ')"
        tr -d '\r' < "$console" | tail -n 3 | sed 's/^/bed-shm: running: vm: /'
    done
    if [ -n "$load_gone" ]; then
        figure_why=$load_gone
    elif keep_running "$pid"; then
        figure_why='no doorbell and no line of output for 600 s'
    fi
    if keep_end "$pid"; then
        figure_status=$keep_status
    else
        figure_status="ended, its keeper not done: $keep_left"
    fi
    pid=''
    if [ "$figure_status" != 0 ] || [ -n "$figure_why" ]; then
        ok=0
        echo "bed-shm: failed: status $figure_status${figure_why:+: $figure_why}: $(ivshmemd -i 2>&1 | tr '\n' ' ')"
    fi
    cat "$out/figure.out" >> "$out/measure.txt"
    [ -z "$figure_why" ] || echo "FIGURE FAIL: $*: $figure_why" >> "$out/measure.txt"
    sed 's/^/bed-shm: figure: /' "$out/figure.out"
}

# nocopy_rows: the buffers mapped from the VM's pages, then the VM killed
# with one mapping live.
nocopy_rows()
{
    if [ "$(info bound)" != 1 ]; then
        for c in build pages coherent release refuse measure revoke leak; do
            fail "$c" "virtio-gpu did not bind over the region: $(why_unbound)"
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
    until_line "$console" 'backend: map: resource .*: refused: not exported' 60 vm ||
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
    keep_new "the mapping's holder"
    VK_DRIVER_FILES=$icd NOCOPY_INTEL=$intel "$keep" "$$" "$kb" "$out/nocopy" hold 1048576 \
        <> "$kb.ctl" > "$out/hold.out" 2>&1 &
    holder=$keepn
    until_line "$out/hold.out" 'NOCOPY HELD\|NOCOPY FAIL' 120 "$holder" || true
    ivshmemd -i > "$out/info-held.txt" 2>&1
    held=$(stat kvm_gpa_map_pages)
    kill_vm
    until_line "$out/hold.out" 'NOCOPY SIGBUS\|NOCOPY FAIL' 60 "$holder" || true
    # A holder left after KILL is cleanup's.
    ! keep_end -k "$holder" || holder=''
    # ivshmemd detaches once QEMU has gone; one left is cleanup's.
    ! keep_wait "$daemon" 600 || ! keep_end "$daemon" || daemon=''
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

# defer: `vgshmprobe fdefer` on the region's card: resources whose backing
# from the VM's pages is asked for at their create and taken at their first
# use. First one whose backing the backend, told through the VM's second
# serial port, refuses, which the region's heap must back: its bytes must
# go to the device and come back through its map, and the backend's one
# refusal as asked must name its resource. Then one of the VM's pages the
# same way, which the probe fails unless it added one map of the VM's pages
# to the transport's count, one too large for either, and more held
# untaken than the driver's cap on pending asks.
defer()
{
    asked0=$(tr -d '\r' < "$console" | grep -c 'backend: back: resource [0-9]*: refused: [0-9]* bytes: as asked')
    derr=''
    vm_ctl refuse || derr="the VM did not have the backend refuse"
    "$probe" fdefer > "$out/defer.out" 2>&1
    id=$(sed -n 's/^VGSHMPROBE DEFER: refused: resource //p' "$out/defer.out")
    asked=$(($(tr -d '\r' < "$console" | grep -c 'backend: back: resource [0-9]*: refused: [0-9]* bytes: as asked') - asked0))
    named=$(tr -d '\r' < "$console" | grep -c "backend: back: resource ${id:-none}: refused: [0-9]* bytes: as asked")
    if [ -z "$derr" ] && { [ "$asked" != 1 ] || [ "$named" != 1 ]; }; then
        derr="the backend refused $asked backing(s) as asked, $named of them resource ${id:-none}'s"
    fi
    if [ -z "$derr" ] && grep -q 'VGSHMPROBE OK' "$out/defer.out"; then
        pass defer "$(sed -n 's/^VGSHMPROBE DEFER: //p' "$out/defer.out" | tr '\n' ';' |
            sed 's/;$//; s/;/; /g')"
    else
        fail defer "${derr:+$derr: }$(grep VGSHMPROBE "$out/defer.out" | tr '\n' ' ')"
    fi
}

# readback: readback.txt's piglit tests on the region's card, one at a time
# on EGL's surfaceless platform as the desktop user, as the bed's piglit
# scene runs them (bed/measure/measure.sh), and the refusals they met: of a
# blob's export, of a page list, of a resource's backing from the VM's
# memory, and of the region's heap. The buffers must have been backed from
# the VM's pages, each a map of the transport's.
readback()
{
    kit=$(cd "$(dirname "$0")" && pwd)
    rb=$out/readback
    mkdir -p "$rb"
    chown desktop "$rb"
    exports0=$(refusals) klog0=$(klog_count 'refused: its page list')
    backs0=$(klog_count "refused: the VM's backing") maps0=$(info maps) heap0=$(info refused)
    su -s /bin/sh desktop -c "exec sh $kit/piglit.sh -g -o $rb -l $kit/readback.txt" \
        > "$out/readback.out" 2>&1
    piglit_status=$?
    # The VM's console comes behind its work, at the serial port's pace:
    # wait until it has been silent for 5 s, 120 s at most.
    last=-1 still=0 n=120
    while [ "$n" -gt 0 ] && quiet "$console" 5 vm; do
        n=$((n - 1))
    done
    exports=$(($(refusals) - exports0)) klog=$(($(klog_count 'refused: its page list') - klog0))
    backs=$(($(klog_count "refused: the VM's backing") - backs0))
    maps=$(($(info maps) - maps0)) heap=$(($(info refused) - heap0))
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
    what="$results; $exports refused exports, $klog refused page lists, $backs refused backings"
    what="$what, $heap refused heap pages, $maps maps of the VM's pages; renderer ${renderer:-unknown}"
    if [ "$piglit_status" = 0 ] && [ "$passed" = "$tests" ] && [ "$exports" = 0 ] && [ "$klog" = 0 ] &&
        [ "$backs" = 0 ] && [ "$heap" = 0 ] && [ "$maps" -gt 0 ] &&
        echo "$renderer" | grep -q '^virgl'; then
        pass readback "$what"
    else
        fail readback "piglit.sh status $piglit_status: $what"
    fi
}

# vm_ctl WORD [SECONDS]: have bed-vm-control in the VM do WORD to the
# backend, stop, cont, kill, start, hold, refuse or term, and wait SECONDS,
# 120 by default, at most for its word on the console that it has.
vm_ctl()
{
    had=$(tr -d '\r' < "$console" | grep -c "bed-vm: control: $1: done")
    echo "$1" >&7
    n=${2:-120}
    while [ "$(tr -d '\r' < "$console" | grep -c "bed-vm: control: $1: done")" -le "$had" ]; do
        n=$((n - 1))
        [ "$n" -gt 0 ] || return 1
        sleep 1
    done
}

# rebound BINDS: wait until virtio-gpu is bound over the region again, more
# than BINDS times in all, while quiet console 600 qemu holds.
rebound()
{
    last=-1 still=0
    until [ "$(info bound)" = 1 ] && [ "$(info binds)" -gt "$1" ]; do
        quiet "$console" 600 vm || return 1
    done
}

# loss NAME WORD MIN MAX REASON: row NAME. With the backend stopped,
# `vgshmprobe lost` makes a render target and maps it, and the map waits
# for the backing's answer; WORD, if any, is done to the backend once the
# probe sleeps in that wait. The wait must end with ENODEV in MIN to MAX
# seconds, the kernel must have logged the loss once as `lost: epoch N: the
# device: REASON`, and the backend, sent SIGCONT, or started again after a kill,
# must be bound again for the rows after.
loss()
{
    lname=$1 word=$2 min=$3 max=$4 reason=$5
    binds=$(info binds) lost0=$(klog_count "lost: epoch [0-9]*: the device: $reason")
    lerr=''
    vm_ctl stop || lerr="the VM did not stop the backend"
    keep_new "vgshmprobe lost"
    "$keep" "$$" "$kb" "$probe" lost <> "$kb.ctl" > "$out/$lname.out" 2>&1 &
    waiter=$keepn
    keep_started "$waiter" || :
    wpid=$(keep_pid "$waiter")
    if [ -z "$lerr" ] && [ -n "$word" ]; then
        n=60
        until [ "$(cat "/proc/$wpid/wchan" 2>/dev/null)" = virtio_gpu_wait_answer ]; do
            n=$((n - 1))
            if [ "$n" -le 0 ] || ! keep_running "$waiter"; then
                lerr="the probe never slept in virtio_gpu_wait_answer"
                break
            fi
            sleep 0.5
        done
        [ -n "$lerr" ] || vm_ctl "$word" || lerr="the VM did not $word the backend"
    fi
    n=$((max + 30))
    while keep_running "$waiter" && [ "$n" -gt 0 ]; do
        sleep 1
        n=$((n - 1))
    done
    if keep_running "$waiter"; then
        lerr="${lerr:+$lerr, }the probe still waits in $(cat "/proc/$wpid/wchan" 2>/dev/null) after $((max + 30)) s"
    fi
    # KILL to a probe still waiting, and 40 s for its keeper; one left is
    # cleanup's.
    keep_end -k "$waiter" || lerr="${lerr:+$lerr, }$keep_left"
    waiter=''
    ms=$(sed -n 's/^VGSHMPROBE LOST: ms //p' "$out/$lname.out")
    logged=$(($(klog_count "lost: epoch [0-9]*: the device: $reason") - lost0))
    if [ "$word" = kill ]; then
        vm_ctl start || lerr="${lerr:+$lerr, }the VM did not start the backend again"
    else
        vm_ctl cont || lerr="${lerr:+$lerr, }the VM did not continue the backend"
    fi
    rebound "$binds" || lerr="${lerr:+$lerr, }virtio-gpu was not bound again"
    what="$(sed -n 's/^VGSHMPROBE LOST: error /error /p' "$out/$lname.out") after ${ms:-?} ms, $logged loss(es) logged"
    if [ -z "$lerr" ] && grep -q 'VGSHMPROBE OK' "$out/$lname.out" && [ "$logged" = 1 ] &&
        [ "${ms:-0}" -ge $((min * 1000)) ] && [ "${ms:-0}" -le $((max * 1000)) ]; then
        pass "$lname" "$what, bound again"
    else
        fail "$lname" "${lerr:+$lerr: }$what: $(grep VGSHMPROBE "$out/$lname.out" | tr '\n' ' ')"
    fi
}

# held: row `held`. The backend, told through the VM's second serial
# port, holds the next new backing 45 s, past the heartbeat's limit of
# 30 s, while its process runs, and `vgshmprobe held` makes a render target
# and maps it, and the map waits for that backing. The map must succeed
# more than 35 s on,
# the VM's console must say the backend held one backing, the kernel must
# log no loss and virtio-gpu must not have bound again: the heartbeat moves
# from a thread apart from the serving one, so a long command is waited
# for, as Linux's guest waits out its host's.
held()
{
    binds=$(info binds) lost0=$(klog_count 'lost: epoch [0-9]*: the device: ')
    holds0=$(tr -d '\r' < "$console" | grep -c 'backend: back: resource [0-9]*: holding ')
    herr=''
    vm_ctl hold || herr="the VM did not have the backend hold"
    keep_new "vgshmprobe held"
    "$keep" "$$" "$kb" "$probe" held <> "$kb.ctl" > "$out/held.out" 2>&1 &
    waiter=$keepn
    n=120
    while keep_running "$waiter" && [ "$n" -gt 0 ]; do
        sleep 1
        n=$((n - 1))
    done
    if keep_running "$waiter"; then
        herr="${herr:+$herr, }the probe still waits in $(cat "/proc/$(keep_pid "$waiter")/wchan" 2>/dev/null) after 120 s"
    fi
    # KILL to a probe still waiting, and 40 s for its keeper; one left is
    # cleanup's.
    keep_end -k "$waiter" || herr="${herr:+$herr, }$keep_left"
    waiter=''
    ms=$(sed -n 's/^VGSHMPROBE HELD: ms //p' "$out/held.out")
    logged=$(($(klog_count 'lost: epoch [0-9]*: the device: ') - lost0))
    holds=$(($(tr -d '\r' < "$console" | grep -c 'backend: back: resource [0-9]*: holding ') - holds0))
    what="$(sed -n 's/^VGSHMPROBE HELD: error /error /p' "$out/held.out") after ${ms:-?} ms"
    what="$what, $holds held, $logged loss(es) logged, binds $binds then $(info binds)"
    if [ -z "$herr" ] && grep -q 'VGSHMPROBE OK' "$out/held.out" && [ "$holds" = 1 ] &&
        [ "$logged" = 0 ] && [ "$(info bound)" = 1 ] && [ "$(info binds)" = "$binds" ] &&
        [ "${ms:-0}" -ge 35000 ]; then
        pass held "$what"
    else
        fail held "${herr:+$herr: }$what: $(grep VGSHMPROBE "$out/held.out" | tr '\n' ' ')"
    fi
}

# virgl_rows: the frame drawn, the doorbells, without -u the readbacks, the
# backings taken at first use, a backing held past the heartbeat's limit,
# the backend's loss, stopped and then killed, and the VM's death with
# fences pending.
virgl_rows()
{
    if [ "$(info bound)" != 1 ]; then
        fail draw "virtio-gpu did not bind over the region: $(why_unbound)"
        fail doorbell "no device"
        if [ "$unplug" = 0 ]; then
            fail readback "no device"
            fail defer "no device"
            fail held "no device"
            fail stopped "no device"
            fail killed "no device"
        fi
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
        if [ "$unplug" = 0 ]; then
            readback
            defer
            held
            # The heartbeat stands still from the stop, so the wait ends
            # 30 s on, less what the stop's word and the probe's start
            # took; the kill ends it at once, by the module's word.
            loss stopped '' 10 60 'its heartbeat stood still'
            loss killed kill 0 20 "the device's process ended"
        fi
    fi

    # The VM's death with fences pending, the probe's target made first.
    keep_new 'the fence probe'
    "$keep" "$$" "$kb" "$probe" pending 4 <> "$kb.ctl" > "$out/pending.out" 2>&1 &
    waiter=$keepn
    probe_ready "$waiter"
    pending=yes
    vm_signal STOP || { pending=no; echo "bed-shm: death: QEMU not stopped: $keep_why" >&2; }
    keep_signal "$waiter" CONT || echo "bed-shm: death: the probe not continued: $keep_why" >&2
    fences_pending || pending=no
    kill_vm
    death_children
    [ "$pending" = yes ] || death_error="${death_error:+$death_error, }four fences were not pending before the death"
    if [ -z "$death_error" ] && grep -q 'VGSHMPROBE OK' "$out/pending.out" &&
        grep -q 'result: detached: QEMU closed' "$out/ivshmemd.out"; then
        pass death "$(sed -n 's/^VGSHMPROBE FENCE: //p' "$out/pending.out" | head -n 1)"
    else
        fail death "${death_error:+$death_error: }$(grep 'VGSHMPROBE\|result' "$out/pending.out" "$out/ivshmemd.out" | tr '\n' ' ')"
    fi
}

# The desktop session's log and its user's display.
desktop_env='XDG_RUNTIME_DIR=/run/user/1001 WAYLAND_DISPLAY=wayland-0'

# since OFFSET: the services' log from byte OFFSET on, or from the start
# if init has shortened it below OFFSET. Keep the copy in DIR/services.txt.
since()
{
    log_size=$(command stat -c %s "$services" 2>/dev/null || echo 0)
    log_from=$1
    [ "$log_size" -ge "$log_from" ] || log_from=0
    tail -c +"$((log_from + 1))" "$services" 2>/dev/null |
        head -c "$((log_size - log_from))" | tr -d '\r' > "$out/services.txt"
    cat "$out/services.txt"
}

# started OFFSET DISPLAY RENDERER TRIES [N]: wait until the
# session's last start since OFFSET is on DISPLAY, `BACKEND CARD RENDERER`
# as rach2-desktop logs it, its Weston has logged a GL renderer matching
# RENDERER and the user's socket exists. Give up after TRIES sleeps, or
# once the command of keeper N, or with N `vm` QEMU, has ended, when given.
# Set `took` to the number of seconds slept.
# With which=any, any start since OFFSET will do, not only the last.
which=last
started()
{
    took=0
    until since "$1" | awk -v want="on $2" -v gl="$3" -v which="$which" '
            /rach2-desktop: session: started as / { on = index($0, want) > 0; seen = 0 }
            on && /GL renderer: / && $0 ~ ("GL renderer: " gl) { seen = ever = 1 }
            END { exit !(which == "any" ? ever : on && seen) }' && [ -S /run/user/1001/wayland-0 ]; do
        [ "$took" -lt "$4" ] || return 1
        [ -z "${5:-}" ] || alive "$5" || return 1
        sleep 1
        took=$((took + 1))
    done
}

# drawn FILE ARGS...: glmark2-wayland ARGS as the desktop user on the
# session, into FILE, under a keeper, which kills it and all it left once
# FILE has stayed the same for 120 s, as a client of a compositor that no
# longer answers does.
drawn()
{
    out_file=$1
    shift
    keep_new glmark2
    "$keep" "$$" "$kb" su -s /bin/sh desktop -c "export $desktop_env; exec glmark2-wayland $*" \
        <> "$kb.ctl" > "$out_file" 2>&1 &
    waiter=$keepn
    last=-1 still=0
    while keep_running "$waiter"; do
        quiet "$out_file" 120 || break
    done
    if keep_end -k "$waiter"; then
        drawn_status=$keep_status
    else
        drawn_status=1
        echo "bed-shm: fallback: glmark2 left: $keep_left" >&2
    fi
    waiter=''
    return "$drawn_status"
}

# desk_return: check the native session and its pipe CRC after the handoff.
desk_return()
{
    # With -g, Rach2's driver has the GPU again once gpu.sh has ended.
    [ -n "$bdf" ] || return 0
    which=last
    back=''
    for c in /sys/class/drm/card[0-9]*; do
        case $(readlink -f "$c/device" 2>/dev/null) in */"$bdf") back=${c##*/} ;; esac
    done
    # The last start is on the card given back, whether it came there from
    # the headless session (`appeared`) or at once.
    if [ -n "$back" ] && started "$dead" "drm $back gl" "Mesa Intel" 60; then
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

# DIR/sample-memory.sh QEMU: once a second until it is stopped, a line of
# Rach2's MemAvailable in kB, the VM's pages that Rach2 maps for its
# buffers, the backings among them (kvm_gpa_map_pages), and the resident kB
# of QEMU, process QEMU, `-` for one not read. One awk reads the three
# files, since the shell's read takes a byte at a time and /proc/stat is
# long, so that the figures pay one awk and one sleep a second. It runs
# under a keeper of its own while a scene runs.
cat > "$out/sample-memory.sh" <<'SAMPLER'
while :; do
    st=/proc/$1/status
    [ -n "$1" ] && [ -r "$st" ] || st=/dev/null
    awk '$1 == "MemAvailable:" { avail = $2 } $1 == "kvm_gpa_map_pages" { pages = $2 }
         $1 == "VmRSS:" { rss = $2 }
         END { print (avail == "" ? "-" : avail), (pages == "" ? "-" : pages), (rss == "" ? "-" : rss) }' \
        /proc/meminfo /proc/stat "$st"
    sleep 1
done
SAMPLER

# memory_record NAME FROM TO: keep the VM's `bed-vm: memory:` lines of the
# scene, those of its console that begin at or after byte FROM and end by
# byte TO, with the last one before it, in DIR/memory-NAME.vm, and print
# the figures of them and of DIR/memory-NAME.rach2, sample_memory's lines:
# the VM's MemAvailable and i915's memory, and Rach2's MemAvailable, its use
# apart from QEMU's resident memory, which bed.sh's reserve must hold, and
# the VM's pages it maps, `-` for a figure no sample gave.
#
# The VM writes a line only when a figure has moved 64 MiB since its last
# one, with the extremes of the seconds since that line. The figures read
# in the scene are the values of its lines and the extremes of each of them
# after the first; the first one's may be of seconds before the scene. A
# second that no line read is within 64 MiB of the line before it, so from
# the scene's start to the reading of its last line no second is below the
# least of those lines and the one before the scene less 64 MiB, or above
# their most plus 64 MiB. After that reading nothing bounds a second: a line
# the VM wrote in the scene may reach the console after its end, so the
# bound ends there, and a scene with no line or none before it has none.
memory_record()
{
    mem_total=$(awk '$1 == "MemTotal:" { print $2 }' /proc/meminfo)
    # The scene begins after the end of the line under way at FROM.
    cut=0
    if [ "$2" -gt 0 ]; then
        cut=$(head -c "$3" "$console" 2>/dev/null | tail -c "+$2" | head -n 1 | wc -c)
        cut=$(($2 - 1 + cut))
    fi
    # Whole lines only, so that a line FROM or TO cut is left out.
    line='bed-vm: memory: available [0-9]+ kB, least [0-9]+ kB; i915 (none|[0-9]+ objects, [0-9]+ kB, most [0-9]+ kB)$'
    {
        head -c "$cut" "$console" 2>/dev/null | tr -d '\r' | grep -aE "$line" | tail -n 1 |
            sed 's/^.*bed-vm: memory: /before: /'
        head -c "$3" "$console" 2>/dev/null | tail -c "+$((cut + 1))" | tr -d '\r' |
            grep -aE "$line" | sed 's/^.*bed-vm: memory: //'
    } > "$out/memory-$1.vm"
    # A line: available A kB, least L kB; i915 N objects, K kB, most M kB,
    # or i915 none; `before: ` before the last one ahead of the scene.
    vm_mem=$(awk '
        function mib(v) { return int(v / 1024) }
        {
            pre = $1 == "before:"
            if (pre) { $1 = ""; $0 = $0 }
            gem = $8 == "none" ? "" : $10 + 0
            if (pre) { before = $2 + 0; bgem = gem; next }
            n++
            if (least == "" || $2 < least) least = $2 + 0
            if (most == "" || $2 > most) most = $2 + 0
            if (n > 1 && $5 < least) least = $5 + 0
            if (gem == "") next
            if (gmost == "" || gem > gmost) gmost = gem
            if (n > 1 && $13 > gmost) gmost = $13 + 0
        }
        END {
            if (before == "" && !n) { printf "the VM wrote no sample"; exit }
            low = least; high = most; ghigh = gmost
            if (before != "") {
                if (low == "" || before < low) low = before
                if (high == "" || before > high) high = before
                if (bgem != "" && (ghigh == "" || bgem > ghigh)) ghigh = bgem
            }
            bound = before != "" && n
            upto = " until the last line\047s reading, and no bound after it"
            printf "the VM\047s MemAvailable "
            if (before != "") printf "%d MiB before the scene, ", mib(before)
            if (n) printf "least %d MiB and most %d read in %d line%s in it", mib(least), mib(most), n, (n > 1 ? "s" : "")
            else printf "no line in it"
            if (bound) printf ", no second below %d MiB or above %d%s", mib(low - 65536), mib(high + 65536 + 1023), upto
            else printf ", and no bound in it"
            if (gmost != "") {
                printf "; i915\047s objects at most %d MiB read", mib(gmost)
                if (bound) printf ", no second above %d%s", mib(ghigh + 65536 + 1023), upto
                else printf ", and no bound in it"
            } else if (bgem != "") printf "; i915\047s objects %d MiB before the scene, and no bound in it", mib(bgem)
            else printf "; no i915"
        }' "$out/memory-$1.vm")
    rach2_mem=$(awk -v total="$mem_total" '
        function mib(v) { return v == "" ? "-" : int(v / 1024) }
        $1 ~ /^[0-9]+$/ {
            n++
            if (least == "" || $1 < least) least = $1
            if ($3 ~ /^[0-9]+$/ && (own == "" || total - $1 - $3 > own)) own = total - $1 - $3
        }
        $2 ~ /^[0-9]+$/ && (pages == "" || $2 > pages) { pages = $2 }
        END {
            if (!n) { printf "Rach2 gave no sample"; exit }
            printf "Rach2\047s MemAvailable least %s MiB of %s, its use apart from QEMU at most %s MiB, the VM\047s pages it maps at most %s MiB, in %d samples",
                mib(least), mib(total), mib(own), pages == "" ? "-" : mib(pages * 4), n
        }' "$out/memory-$1.rach2")
    echo "bed-shm: memory: $1: $vm_mem; $rach2_mem"
}

# crysis_counts: the same counters as the bed's shm-virgl figures.
crysis_counts()
{
    printf 'copied_bytes %s\nexits %s\nround_trips %s\n' \
        "$(stat virtio_gpu_copied_bytes)" "$(stat kvm_exits)" "$(info rung | cut -d/ -f1)"
}

# crysis_figure: measure the umu flyby with no minimum frame rate. prepare
# writes the session's files; the flyby runs in crysis.sh's read-only
# namespace, with the output directory writable.
crysis_figure()
{
    loops=${CRYSIS_LOOPS:-2}
    if ! cp -p "$crysis_ini" "$out/weston.ini"; then
        fail load 'could not save weston.ini'
        return
    fi
    crysis_ini_saved=$out/weston.ini
    crysis_counts > "$out/crysis-counts.before"
    keep_new 'the flyby'
    crysis_keep=$keepn
    # shellcheck disable=SC2016 # the child shell reads its output directory
    "$keep" "$$" "$kb" sh -c '
        export CRYSIS_STATE=$1/crysis-state CRYSIS_GATE=0
        timeout --foreground 60 python3 ./crysis.py prepare "$1/crysis" --shm &&
            exec sh ./crysis.sh seal "$1" python3 ./crysis.py flyby umu flyby-umu "$2"
    ' sh "$out" "$loops" <> "$kb.ctl" &
    fig=$keepn
    if await "$fig"; then
        pass load "crysis completed $loops umu flyby loop(s)"
    else
        fail load "crysis flyby status $await_status${await_why:+: $await_why}"
    fi
    fig=''
    crysis_counts > "$out/crysis-counts.after"
    if counts=$(awk '
        NR == FNR { if ($2 ~ /^[0-9]+$/) base[$1] = $2; next }
        $1 in base && $2 ~ /^[0-9]+$/ && $2 >= base[$1] {
            printf " %s %.0f", $1, $2 - base[$1]
            valid++
        }
        END { exit valid != 3 }' "$out/crysis-counts.before" "$out/crysis-counts.after"); then
        echo "bed-shm: figure: crysis shm-virgl$counts"
    else
        fail figure 'missing or reset crysis counters'
    fi
}

# crysis_fresh: print, without carriage returns, the console's whole lines
# that began after byte $mark. The read starts one byte early, or at an
# added newline when $mark is 0, so its first record is the rest of the
# line the mark cut, or nothing, and is dropped; the sentinel after the file
# ends the last record, so a line still being written is dropped too.
crysis_fresh()
{
    {
        if [ "$mark" -gt 0 ]; then
            tail -c "+$mark" "$console"
        else
            echo
            cat "$console"
        fi
        printf '%s\n' '@crysis-fresh-end@'
    } | tr -d '\r' | awk 'NR > 1 && !/@crysis-fresh-end@$/'
}

# crysis_account: stop the VM's backend with SIGTERM through the control
# port, so that it prints its `backend: result:` lines before the VM is
# killed, and wait while the VM lives, 120 s at most in all, for the
# service's `backend: ended:` line on the console. Print the result and end
# lines that came after the word, and pass `account` only when they hold a
# whole account line, `backend: result: account judged N, least room R
# bytes, ledger most L bytes`, followed by `backend: ended: status 0`. A
# stall's snapshot before the word does not count.
crysis_account()
{
    if ! vm_alive; then
        fail account 'the VM ended before its backend was stopped'
        return
    fi
    mark=$(($(wc -c < "$console")))
    start=$(date +%s)
    aerr=''
    vm_ctl term 120 || aerr="no word from the VM that the backend was sent TERM"
    n=$((120 - ($(date +%s) - start)))
    until crysis_fresh | grep -aqE 'bed-vm: backend: ended: status [0-9]+$'; do
        n=$((n - 1))
        { [ "$n" -gt 0 ] && vm_alive; } || break
        sleep 1
    done
    crysis_fresh | grep -aE 'bed-vm: backend: (result|ended): ' |
        sed 's/^.*bed-vm: /bed-shm: vm: /'
    line=$(crysis_fresh | awk '
        /bed-vm: backend: result: account / {
            sub(/^.*bed-vm: backend: result: account /, "")
            acc = $0 ~ /^judged [0-9]+, least room [0-9]+ bytes, ledger most [0-9]+ bytes$/ ? $0 : ""
            next
        }
        /bed-vm: backend: ended: status/ {
            if (acc != "" && $0 ~ /bed-vm: backend: ended: status 0$/) found = acc
            exit
        }
        END { if (found != "") print found }')
    if [ -n "$line" ]; then
        pass account "$line"
    else
        fail account "${aerr:+$aerr; }no whole account line followed by the backend's end with status 0 since TERM: $(tr -d '\r' < "$console" | tail -n 2 | tr '\n' ' ')"
    fi
}

# crysis_return: stop the backend and then the VM after the flyby, and
# check the restored session.
crysis_return()
{
    crysis_restore_ini || fail return "$crysis_why"
    dead=$(command stat -c %s "$services" 2>/dev/null || echo 0)
    tail -c 16384 "$services" > "$out/services-death.txt"
    crysis_account
    kill_vm
    desk_return
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
            fail "$c" "virtio-gpu did not bind over the region: card '$card', $(why_unbound)"
        done
        [ "$loadscene" != crysis ] || crysis_return
        return
    fi
    # After a GPU handoff the supervisor moves a running session itself.
    # With none, as on the QEMU boot or on the Lenovo before a step, have it
    # start one, which chooses the card now bound.
    [ -z "$bdf" ] || [ "$weston_found" = 1 ] || session_owed=1
    if [ -z "$bdf" ] || ! pgrep -x weston > /dev/null; then
        sup=$(cat /run/rach2-desktop.pid 2>/dev/null)
        [ -z "$sup" ] || kill -HUP "$sup" 2>/dev/null || :
    fi
    # With the GPU, the VM's virgl draws on its i915, which virgl names.
    gl=virgl
    [ -z "$bdf" ] || gl='virgl .*Intel'
    if started "$offset" "drm $card gl" "$gl" 120 vm; then
        pass session "Weston on $card through virgl after $took s: $(grep 'GL renderer: ' "$out/services.txt" | tail -n 1 | sed 's/^.*GL renderer: //')"
    else
        fail session "$(grep 'rach2-desktop: \|GL renderer\|fatal\|error' "$out/services.txt" | tail -n 4 | tr '\n' ' ')"
        for c in load death fallback; do
            fail "$c" 'the session did not start on the VM card'
        done
        if [ "$loadscene" = crysis ]; then
            crysis_return
        elif [ -n "$bdf" ]; then
            fail return 'the session did not start on the VM card'
        fi
        return
    fi
    if [ "$loadscene" = crysis ]; then
        crysis_figure
        crysis_return
        return
    fi
    for scene in ${measure:+$scenes}; do
        rung=shm-virgl zink=''
        case $scene in venus:*) rung=shm-venus scene=${scene#venus:} zink="MESA_LOADER_DRIVER_OVERRIDE=zink VK_DRIVER_FILES=$icd" ;; esac
        sname=$rung-$scene
        figout=$out/measure-$sname.out
        sfrom=$(command stat -c %s "$console" 2>/dev/null || echo 0)
        keep_new "the memory sampler of $sname"
        "$keep" "$$" "$kb" sh "$out/sample-memory.sh" "$qemu" <> "$kb.ctl" > "$out/memory-$sname.rach2" 2>&1 &
        sampler=$keepn
        keep_new "the figure $scene"
        # shellcheck disable=SC2086 # assignments
        "$keep" "$$" "$kb" env $desktop_env $zink sh "$measure/measure.sh" -r "$rung" \
            -u desktop -d -M "$vmmem" -o "$out/measure/$sname" "$scene" <> "$kb.ctl" > "$figout" 2>&1 &
        fig=$keepn
        await "$fig" ||
            echo "bed-shm: figure: $scene: status $await_status${await_why:+: $await_why}" >&2
        fig=''
        sto=$(command stat -c %s "$console" 2>/dev/null || echo 0)
        keep_end "$sampler" || echo "bed-shm: memory: $sname: the sampler's keeper is not done: $keep_left" >&2
        sampler=''
        sed 's/^/bed-shm: figure: /' "$figout"
        grep "^measure: $scene: skipped: " "$figout" |
            sed "s/^measure: $scene: skipped: /bed-shm: skipped: $scene: /"
        skipped=$((skipped + $(grep -c "^measure: $scene: skipped: " "$figout")))
        memory_record "$sname" "$sfrom" "$sto"
        if [ -z "$await_why" ] && [ "$await_status" = 0 ] && grep -q "^measure: result: $scene $rung " "$figout"; then
            pass scene "$scene on $rung"
        else
            fail scene "$scene on $rung: measure.sh status $await_status${await_why:+: $await_why}: $(grep 'failed\|runner: ' "$figout" | tail -n 2 | tr '\n' ' ')"
        fi
    done
    if [ "$profile_only" = 1 ]; then
        crysis_return
        return
    fi

    # The load, in a group of its own, until the driver has rung the VM's
    # queue 32 times since it began to draw: glmark2 once it has named its
    # renderer, a scene of the harness once the harness says it has
    # started, after its latency's own round trips, for longer than the
    # step will take. The step ends after 600 s without a doorbell, and at
    # once when the device is gone (load_wait).
    binds0=$(info binds) lost0=$(klog_count 'vgpu-shm: lost: ')
    last=-1 still=0 load_gone=''
    if [ "$loadscene" = glmark2 ]; then
        loadout=$out/glmark2.out loadline=$loadout loadpat='GL_RENDERER'
        keep_new 'the load'
        "$keep" "$$" "$kb" su -s /bin/sh desktop \
            -c "export $desktop_env; exec glmark2-wayland --run-forever -b build" <> "$kb.ctl" > "$loadout" 2>&1 &
        load=$keepn
    else
        loadout=$out/load.out loadline=$out/load/scene.log loadpat="measure: scene: $loadscene: started"
        keep_new 'the load'
        # shellcheck disable=SC2086 # two assignments
        "$keep" "$$" "$kb" env $desktop_env sh "$measure/measure.sh" -r shm-virgl -u desktop -d -t 600 -M "$vmmem" \
            -o "$out/load" "$loadscene" <> "$kb.ctl" > "$loadout" 2>&1 &
        load=$keepn
    fi
    until grep -q "$loadpat" "$loadline" 2>/dev/null; do
        load_wait 600 "$load" || break
    done
    rung0=$(info rung | cut -d/ -f1)
    until [ -n "$load_gone" ] || [ "$(($(info rung | cut -d/ -f1) - ${rung0:-0}))" -ge 32 ]; do
        load_wait 600 "$load" || break
    done
    drew=$(($(info rung | cut -d/ -f1) - ${rung0:-0}))
    if [ "$loadscene" = glmark2 ]; then
        named=$(sed -n 's/^ *GL_RENDERER: *//p' "$loadout" | head -n 1)
    else
        named="the session's virgl"
    fi
    if [ -z "$load_gone" ] && keep_running "$load" && [ "$drew" -ge 32 ] &&
        { [ "$loadscene" != glmark2 ] || grep -q 'GL_RENDERER: *virgl' "$loadout"; }; then
        pass load "$loadscene on $named, $drew doorbells"
    else
        fail load "$loadscene, $drew doorbells${load_gone:+, $load_gone}: $(tail -n 3 "$loadout" | tr '\n' ' ')"
    fi

    # The VM's death under the load, with fences pending; the load must
    # still run once the VM is stopped and the probe has its fences pending,
    # just before the death, which with -g gpu.sh brings within a second.
    # udev's events for the cards are kept, the monitor listening before
    # the death, for a death after which the session stays on the gone
    # card; udevadm names its kernel source last, once it has subscribed to
    # both.
    keep_new 'the udev monitor'
    "$keep" "$$" "$kb" udevadm monitor --kernel --udev --subsystem-match=drm \
        <> "$kb.ctl" > "$out/udev-death.txt" 2>&1 &
    umon=$keepn
    until_line "$out/udev-death.txt" '^KERNEL - ' 30 "$umon" || true
    monitor=listening
    { grep -q '^UDEV - ' "$out/udev-death.txt" && grep -q '^KERNEL - ' "$out/udev-death.txt" &&
        keep_running "$umon"; } ||
        monitor="not listening: $(tail -n 2 "$out/udev-death.txt" | tr '\n' ' ')"
    keep_new 'the fence probe'
    "$keep" "$$" "$kb" "$probe" pending 4 <> "$kb.ctl" > "$out/pending.out" 2>&1 &
    waiter=$keepn
    probe_ready "$waiter"
    at=$(command stat -c %s "$loadout")
    pending=yes
    vm_signal STOP || { pending=no; echo "bed-shm: death: QEMU not stopped: $keep_why" >&2; }
    keep_signal "$waiter" CONT || echo "bed-shm: death: the probe not continued: $keep_why" >&2
    fences_pending || pending=no
    alive=no
    ! keep_running "$load" || alive=yes
    dead=$(command stat -c %s "$services" 2>/dev/null || echo 0)
    tail -c 16384 "$services" > "$out/services-death.txt"
    stop_vm
    # Take a second copy at QEMU's exit, before gpu.sh finishes the handoff
    # or the surviving compositor fills the log and init truncates it.
    left=10
    while [ "$left" -gt 0 ] && vm_alive; do
        sleep 1
        left=$((left - 1))
    done
    tail -c 16384 "$services" > "$out/services-after-death.txt"
    death_children
    [ "$pending" = yes ] || death_error="${death_error:+$death_error, }four fences were not pending before the death"
    wait_vm
    # glmark2 ends with the session that leaves the gone card; one whose
    # submissions the kernel has refused 100000 times since the death, at
    # some 10000 a second, draws on a session still on that card.
    left=60
    while [ "$left" -gt 0 ] && keep_running "$load"; do
        [ "$(refused)" -lt 100000 ] || break
        sleep 1
        left=$((left - 1))
    done
    # udevadm ends on SIGTERM, which its keeper sends, with 0, or 143 when
    # the signal comes first. A keeper not done 40 s after its stop is left
    # to cleanup.
    stopped=no
    ! keep_running "$umon" || stopped=yes
    if keep_end "$umon"; then
        ustatus=$keep_status
        umon=''
    else
        ustatus=running
        case $monitor in
            listening) monitor="still running: $keep_left" ;;
            *) monitor="$monitor; still running: $keep_left" ;;
        esac
    fi
    [ "$monitor" != listening ] ||
        { [ "$stopped" = yes ] && { [ "$ustatus" = 0 ] || [ "$ustatus" = 143 ]; }; } ||
        monitor="ended with status $ustatus before the row's end: $(tail -n 2 "$out/udev-death.txt" | tr '\n' ' ')"
    [ "$monitor:$ustatus" != listening:143 ] ||
        echo 'bed-shm: death: udev monitor stopped by sigterm' >&2
    if keep_running "$load"; then
        ended="still running"
    elif [ "$alive" = no ]; then
        ended="ended before the death"
    else
        ended="ended with status $(keep_ended "$load")"
    fi
    # Counted once the load has ended, or once it went silent.
    spun=$(refused)
    spinning=''
    [ "$spun" -lt 100000 ] || spinning=", after $spun refused submissions on the gone card"
    if [ -z "$death_error" ] && grep -q 'VGSHMPROBE OK' "$out/pending.out" && [ "$ended" != "still running" ] &&
        [ -z "$spinning" ] && [ "$alive" = yes ] &&
        grep -q 'result: detached: QEMU closed' "$out/ivshmemd.out"; then
        pass death "$(sed -n 's/^VGSHMPROBE FENCE: //p' "$out/pending.out" | head -n 1); $loadscene $ended"
    else
        fail death "$loadscene $ended$spinning: ${death_error:+$death_error: }$(grep 'VGSHMPROBE\|result' "$out/pending.out" "$out/ivshmemd.out" | tr '\n' ' ')"
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
    keep_end "$load" || echo "bed-shm: load: left after the death: $keep_left" >&2
    load=''

    # The session again, headless on llvmpipe, drawing a client; with -g
    # gpu.sh has given the GPU back by now, and the session may have gone
    # on to it, or started on it at once when it came back before the old
    # session had gone, which is the fallback too.
    fbshown="headless - gl" fbgl=llvmpipe
    [ -z "$bdf" ] || which=any fbshown='' fbgl='(llvmpipe|Mesa Intel)'
    if started "$dead" "$fbshown" "$fbgl" 60 &&
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

    desk_return
}

if [ -n "$nocopy" ]; then
    nocopy_rows
elif [ "$desk" = 1 ]; then
    desk_rows
else
    virgl_rows
fi
handoff
dmesg > "$out/dmesg.txt" 2>&1
# The way out, here before the result, so that its steps count; the exit's
# trap then finds it done.
cleanup end

if [ "$bad" -eq 0 ]; then
    if [ "$skipped" -gt 0 ]; then
        echo "bed-shm: result: pass: $skipped skipped"
    else
        echo "bed-shm: result: pass"
    fi
else
    for log in services-death services-after-death; do
        [ -f "$out/$log.txt" ] || continue
        tail -n 40 "$out/$log.txt" | sed "s/^/bed-shm: $log: /"
    done
    # The VM's console is lost with the machine; its last lines but the
    # backend's for each pin go with the result.
    tr -d '\r' < "$console" 2>/dev/null |
        grep -v 'backend: pin: resource' | tail -n 80 |
        sed 's/^/bed-shm: vm: /'
    echo "bed-shm: result: fail: $bad check(s)"
    exit 1
fi
