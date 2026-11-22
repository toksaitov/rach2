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
# is both its fallback and its return. Each load and each figure runs
# under a keeper of its own, bedkeep, beside this script, built at the
# start: a child subreaper that runs it in a session of its own and, once
# it ends or the script writes a word on the keeper's FIFO, ends and reaps
# all it left. The script reads each load's end and the keeper's from the
# keeper's status file, and signals no keeper by its pid.
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
# With -w, so are the services log's tails at and just after the VM's death.
# Any exit, by its end or by SIGTERM, SIGINT or SIGHUP, stops the load's
# and the figure's keepers, kills QEMU and has gpu.sh give the function
# back.
#
# With -w -g -l crysis, run the two-loop Crysis flyby once the region's
# session is ready, in native/crysis.sh's private data fixture and, through
# `crysis.sh seal`, in its read-only namespace. The VM stops after the
# figure, and the same return and handoff checks apply.
# CRYSIS_LOOPS=1 selects one loop for a profile.
# BED_VM_PROFILE_PORT forwards a loopback port to a profiling VM's SSH
# server. BED_VM_PROFILE_CRC=1 asks that VM to enable the flush readback.
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
[ "$desk" = 0 ] || [ -f "$keepc" ] || { echo "bed-shm: no $keepc for -w" >&2; exit 2; }
[ -z "$nocopy" ] || [ "$desk" = 0 ] || { echo "bed-shm: -k and -w are apart" >&2; exit 2; }
[ "$unplug" = 0 ] || { [ -z "$nocopy$bdf" ] && [ "$desk" = 0 ]; } ||
    { echo "bed-shm: -u is apart from -k, -w and -g" >&2; exit 2; }
# load and fig are the numbers of the load's and the figure's keepers
# (keep_new), not pids.
daemon='' qemu='' gpu='' waiter='' holder='' pid='' load='' fig='' umon='' bad=0
# Children that outlived a row's bounded wait for them after KILL, and the
# numbers of the keepers that outlived keep_end's, for cleanup to end
# again; the keeper, bedkeep.c built into DIR with -w; and the number of
# the last keeper made.
stuck='' keepers='' keep='' keepn=0
# Whether gpu.sh was started, so that cleanup checks the GPU's driver; and
# 1 once cleanup has begun, so that it runs once.
gpu_started=0 cleaned=0
# 1 once this run, with -g on a machine that ran no session, has asked for
# one, which must end once the GPU is back, at the end or on a signal.
session_owed=0
crysis_ini=/etc/xdg/weston/weston.ini crysis_ini_saved=''
# NOCOPY_INTEL, for nocopy: with -g, the GPU's Venus device or a failure.
intel=''
[ -z "$bdf" ] || intel=1

pass() { echo "bed-shm: $1: pass${2:+: $2}"; }
fail() { echo "bed-shm: $1: fail: $2"; bad=$((bad + 1)); }

# pids WORD...: the words that are process IDs above 1 and not this
# script's, so that a kill never names 0, -1 or the script's own group.
pids()
{
    for pids_w in "$@"; do
        case $pids_w in '' | *[!0-9]*) continue ;; esac
        [ "$pids_w" -gt 1 ] && [ "$pids_w" != "$$" ] && printf '%s ' "$pids_w"
    done
}

# keep_new: make the files of a new keeper, numbered keepn, and set kb
# to their base, DIR/keep-N, for `"$keep" "$kb" COMMAND... <> "$kb.ctl" &`.
# BASE.ctl is the keeper's control FIFO, its standard input, opened for
# reading and writing so that the keeper never sees its end of file while
# the script, which holds no end of it, runs; BASE.status is where the
# keeper writes `started KEEPER COMMAND`, `ended STATUS` once its command
# has ended, `left PROCESS` for each process that outlived KILL, and `done
# STATUS` once it has reaped all, just before it exits; BASE.stop, once
# made, stops a keeper that opens its FIFO after the word was written.
keep_new()
{
    keepn=$((keepn + 1))
    kb=$out/keep-$keepn
    rm -f "$kb.ctl" "$kb.status" "$kb.stop"
    mkfifo "$kb.ctl" || echo "bed-shm: keeper $keepn: no FIFO; the keeper refuses to start" >&2
}

# keep_running N: whether the command of keeper N has not ended, from its
# status file, whatever the keeper does about what it left: no `ended`
# line, and no `done` of a keeper that refused to start.
keep_running()
{
    ! grep -qE '^(ended|done) ' "$out/keep-$1.status" 2>/dev/null
}

# keep_stop N: ask keeper N to end its command and all it left: make
# BASE.stop, then write a word on its FIFO, opened for reading and writing,
# which never waits; a keeper that has its FIFO open reads the word, and
# one that opens it later finds BASE.stop. Nothing is written once the
# keeper is done and its FIFO removed.
keep_stop()
{
    : > "$out/keep-$1.stop"
    [ ! -p "$out/keep-$1.ctl" ] || printf 'stop\n' 1<> "$out/keep-$1.ctl"
}

# keep_end N: end keeper N of a load or a figure, which gives its command
# and every process the command left TERM, and KILL 20 s later, and reaps
# them all (keep_stop); then 400 tries of 0.1 s, past the keeper's own
# 30 s, for its `done` line, and its status, the command's, in keep_status.
# A keeper not done by then holds a process that outlived KILL, or never
# started: it stays in keepers, and the return is 1, with what it reported
# in keep_left.
keep_end()
{
    keep_left='' keep_status=''
    [ -n "$1" ] || return 0
    keep_st=$out/keep-$1.status
    keep_stop "$1"
    n=400
    until grep -q '^done ' "$keep_st" 2>/dev/null; do
        if [ "$n" -le 0 ]; then
            case " $keepers " in *" $1 "*) ;; *) keepers="$keepers $1" ;; esac
            keep_said=$(sed -n 's/^left //p' "$keep_st" 2>/dev/null | tr '\n' ';')
            keep_who=$(sed -n 's/^started \([0-9]*\) .*/process \1/p' "$keep_st" 2>/dev/null | head -n 1)
            if [ -n "$keep_who" ]; then
                keep_left="keeper $1 ($keep_who) not done 40 s after its stop: ${keep_said:-it named nothing left}"
            else
                keep_left="keeper $1 never started"
            fi
            return 1
        fi
        sleep 0.1
        n=$((n - 1))
    done
    keep_status=$(sed -n 's/^done //p' "$keep_st" | head -n 1)
    keepers=$(for k in $keepers; do [ "$k" = "$1" ] || printf '%s ' "$k"; done)
    rm -f "$out/keep-$1.ctl"
    return 0
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

# gone TRIES PID...: wait, TRIES tries of 0.1 s at most, until none of the
# children PID... runs, without reaping them, so that a later wait still
# gives each one's status. Returns 1, with the pids still running in
# reap_live, when any is left.
gone()
{
    reap_left=$1
    shift
    while :; do
        reap_live=''
        for reap_pid in "$@"; do
            ! kill -0 "$reap_pid" 2>/dev/null || reap_live="$reap_live $reap_pid"
        done
        [ -n "$reap_live" ] && [ "$reap_left" -gt 0 ] || break
        sleep 0.1
        reap_left=$((reap_left - 1))
    done
    [ -z "$reap_live" ]
}

# reap TRIES PID...: give the children PID... TRIES tries of 0.1 s to exit,
# as gone does, and reap those that did. A task stuck in the kernel may
# outlive KILL, so the wait is bounded. Returns 1, with the pids still
# running in reap_live, when any is left.
reap()
{
    gone "$@"
    reap_status=$?
    shift
    for reap_pid in "$@"; do
        kill -0 "$reap_pid" 2>/dev/null || wait "$reap_pid" 2>/dev/null
    done
    return "$reap_status"
}

# holding PID: what process PID is and what it holds, for the line about a
# child that outlived its wait: its state, wait channel and command, the
# devices it has open and, for ivshmemd, the region as the kernel reports
# it, or for QEMU, KVM's machines and VFIO's devices and pins.
holding()
{
    # shellcheck disable=SC2009 # pgrep gives no state or wait channel
    held_ps=$(ps -e -o pid,stat,wchan,args | awk -v p="$1" '$1 == p { $1 = ""; sub(/^ +/, ""); print; exit }')
    held_dev=$(for f in /proc/"$1"/fd/*; do readlink "$f"; done 2>/dev/null | grep '^/dev/' | sort -u | tr '\n' ' ' | sed 's/ $//')
    held_more=''
    [ "$1" != "$daemon" ] || held_more="; $(ivshmemd -i 2>&1 | head -n 1)"
    [ "$1" != "$qemu" ] ||
        held_more="; $(awk '$1 ~ /^(kvm_vms|vfio_devices|vfio_dma_pages)$/ { printf "%s %s ", $1, $2 }' /proc/stat)"
    echo "${held_ps:-not in ps}; devices: ${held_dev:-none}$held_more"
}

# child PID: the name of the child PID, for a line about it.
child()
{
    case $1 in
        "$qemu") echo "QEMU $1" ;;
        "$daemon") echo "ivshmemd $1" ;;
        "$waiter") echo "the fence probe $1" ;;
        "$holder") echo "the mapping's holder $1" ;;
        "$pid") echo "the probe $1" ;;
        "$umon") echo "the udev monitor $1" ;;
        *) echo "process $1" ;;
    esac
}

# crysis_restore_ini: put back the compositor's settings before the GPU
# returns and the native session starts. A session that started on the
# changed file first, as when QEMU ended early, crysis-shm.sh starts again
# at its end.
crysis_restore_ini()
{
    [ -z "$crysis_ini_saved" ] || cp -p "$crysis_ini_saved" "$crysis_ini" || return 1
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
    crysis_restore_ini || fail settings 'failed to restore weston.ini'

    # Step 2, load: each load's and figure's keeper, and each an earlier
    # end left, ends all of the load or the figure, once each: TERM, KILL
    # 20 s later, and reaps all, within 40 s (keep_end); then every child
    # the run started but gpu.sh, QEMU among them unless gpu.sh runs it: TERM,
    # 10 s, KILL, 10 s more. The survivors are named with what they hold,
    # left unreaped, so that no other process takes their pids. QEMU under
    # gpu.sh is gpu.sh's to kill, in step 3, before its give-back.
    survivors=''
    # shellcheck disable=SC2086 # a list of keepers' numbers
    for k in $(printf '%s\n' $load $fig $keepers | sort -un); do
        keep_end "$k" || survivors="${survivors:+$survivors; }$keep_left"
    done
    load='' fig=''
    vm=$qemu
    [ -z "$(pids "$gpu")" ] || vm=''
    # shellcheck disable=SC2086 # stuck is a list of pids
    kids=$(pids $vm $waiter $holder $pid $daemon $umon $stuck)
    if [ -n "$kids" ]; then
        # shellcheck disable=SC2086 # a list of pids
        kill -TERM $kids 2>/dev/null
        # shellcheck disable=SC2086 # a list of pids
        if ! reap 100 $kids; then
            kids=$reap_live
            # shellcheck disable=SC2086 # a list of pids
            kill -KILL $kids 2>/dev/null
            # shellcheck disable=SC2086 # a list of pids
            reap 100 $kids ||
                for l in $reap_live; do
                    survivors="${survivors:+$survivors; }$(child "$l") after KILL: $(holding "$l")"
                done
        fi
    fi
    # The control port: the script's end of the VM's second serial port
    # closed and its two pipes removed, QEMU, the other end, having gone,
    # and bed-vm-control with its VM.
    exec 7<&- 2>/dev/null
    rm -f "$out/ctl.in" "$out/ctl.out"
    if [ -z "$survivors" ]; then
        pass stop "the load's keepers and every child have ended (bounds: 40 s for a keeper, 20 s to TERM and 10 s to KILL its processes, 10 s each to TERM and to KILL a child)"
    else
        fail stop "still running: $survivors"
    fi

    # Step 3, GPU: with -g, one TERM to gpu.sh, never a second, which kills
    # QEMU and gives the function back with every signal ignored; 60 s for
    # it, since i915's bind resets the GPU, and then the function must be
    # on i915.
    gpu_back=1 gpu_why=''
    if [ "$gpu_started" = 1 ]; then
        gpu_back=0
        if [ -n "$(pids "$gpu")" ]; then
            kill -TERM "$gpu" 2>/dev/null
            if reap 600 "$gpu"; then
                gpu='' qemu=''
            else
                gpu_why="gpu.sh $gpu still running 60 s after its TERM: QEMU ${qemu:-unknown}${qemu:+: $(holding "$qemu")}"
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
# The keeper of each load and figure, built with the root's cc, as
# native/bed.sh builds crcmodes; a run that cannot keep its loads runs
# none.
if [ "$desk" = 1 ]; then
    keep=$out/bedkeep
    if ! cc -std=c99 -static -O2 -o "$keep" "$keepc" > "$out/bedkeep.build" 2>&1; then
        fail keeper "bedkeep did not build: $(tail -n 3 "$out/bedkeep.build" | tr '\n' ' ')"
        cleanup end
        echo "bed-shm: result: fail: $bad check(s)"
        exit 1
    fi
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

# load_wait SILENCE N: one second of the load step's wait, which
# `last=-1 still=0` starts, with binds0 and lost0 the transport's binds and
# the kernel's `vgpu-shm: lost:` lines at the step's start. It fails once
# the command of keeper N has ended; at once once the device is gone, a
# loss logged, the transport unbound or bound again, since no doorbell can
# come after that, with load_gone saying why; or once for SILENCE seconds
# the VM's doorbells have not been rung, whatever processor time the load
# uses, since a load on another renderer may be busy and ring nothing.
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
    if [ "$size" = "$last" ]; then
        still=$((still + 1))
        [ "$still" -lt "$1" ] || return 1
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

# fences_pending: wait for all four submissions while the VM is stopped.
# The probe must finish submitting even if it keeps logging or using CPU.
fences_pending()
{
    pending_left=30
    until grep -qx 'VGSHMPROBE PENDING 4' "$out/pending.out"; do
        [ "$pending_left" -gt 0 ] && kill -0 "$waiter" 2>/dev/null &&
            ! grep -q '^VGSHMPROBE FAIL' "$out/pending.out" || return 1
        sleep 1
        pending_left=$((pending_left - 1))
    done
}

# probe_ready PID: wait until `vgshmprobe pending`, process PID, has said
# its render target is made and has stopped itself, as quiet pending.out 30
# PID allows; the target's backing is the VM's, which a stopped VM cannot
# give.
probe_ready()
{
    until_line "$out/pending.out" 'VGSHMPROBE READY\|VGSHMPROBE FAIL' 30 "$1" || return 0
    last=-1 still=0
    until [ "$(sed -n 's/^State:[[:space:]]*\(.\).*/\1/p' "/proc/$1/status" 2>/dev/null)" = T ]; do
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
set --
if [ -n "${BED_VM_PROFILE_PORT:-}" ]; then
    set -- -netdev "user,id=bedprof,restrict=on,hostfwd=tcp:127.0.0.1:$BED_VM_PROFILE_PORT-:22" \
        -device virtio-net-pci,netdev=bedprof \
        -fw_cfg "name=opt/rach2/vgpu-profile,string=${BED_VM_PROFILE_CRC:-0}"
fi
# The VM's second serial port, ttyS1, a pipe whose input this script
# keeps open, read and written, so that a word sent never waits, even with
# QEMU gone: bed-vm-control in the VM does each word to the backend
# (vm_ctl).
mkfifo "$out/ctl.in" "$out/ctl.out"
exec 7<> "$out/ctl.in"
if [ -z "$bdf" ]; then
    set -- "$@" -chardev pipe,path="$out/ctl",id=ctl -device isa-serial,chardev=ctl,index=1
    TMPDIR=$out qemu-system-x86_64 -accel kvm -machine q35,kernel-irqchip=split -cpu host -smp "$cpus" \
        -m "${mib}M" -nodefaults -no-reboot -display none -monitor none \
        -drive file="$disk",if=none,id=vm,format=raw,snapshot=on \
        -device virtio-blk-pci,drive=vm,ioeventfd=off \
        -chardev socket,path="$out/ivs.sock",id=ivs -device ivshmem-doorbell,chardev=ivs,vectors=2 \
        -serial file:"$console" "$@" > "$out/qemu.out" 2>&1 &
    qemu=$!
else
    TMPDIR=$out bash "$gpush" -d "$bdf" -m "$mib" -s "$cpus" -w 0 -o "$out/gpu" -D "$disk" -- \
        -pidfile "$out/qemu.pid" \
        -chardev socket,path="$out/ivs.sock",id=ivs -device ivshmem-doorbell,chardev=ivs,vectors=2 \
        "$@" > "$out/gpu.log" 2>&1 &
    gpu=$! gpu_started=1
    last=-1 still=0
    until [ -s "$out/qemu.pid" ]; do
        quiet "$out/gpu.log" 120 "$gpu" || break
    done
    qemu=$(cat "$out/qemu.pid" 2>/dev/null)
fi

# stop_vm: kill QEMU, through gpu.sh when it owns the VM.
stop_vm()
{
    if [ -n "$gpu" ]; then
        kill -USR1 "$gpu" 2>/dev/null
    else
        kill -9 "$qemu" 2>/dev/null
    fi
}

# wait_vm: reap QEMU or gpu.sh, which also gives the function back. gpu.sh
# waits for its QEMU first, so the wait for either is bounded at 600 tries
# of 0.1 s, and one still running then is left, with QEMU, to cleanup,
# which names what QEMU holds.
wait_vm()
{
    if [ -n "$gpu" ]; then
        if gone 600 "$gpu"; then
            gpu_status=0
            wait "$gpu" 2>/dev/null || gpu_status=$?
            gpu=''
        else
            gpu_status="still running 60 s after QEMU's kill"
            return 0
        fi
    elif [ -n "$(pids "$qemu")" ]; then
        # A QEMU stuck in the kernel after KILL is left to cleanup.
        gone 600 "$qemu" || return 0
        wait "$qemu" 2>/dev/null
    fi
    qemu=''
}

# kill_vm: kill and reap the VM, and wait for any GPU handoff.
kill_vm()
{
    stop_vm
    wait_vm
}

# death_children: give the fence probe and ivshmemd 60 tries to exit after
# QEMU is killed. Reap only children that exited. A task stuck in the kernel
# may still be there after KILL, so the failure must not wait for its exit;
# ivshmemd left stays in daemon, and the probe left goes to stuck, since a
# later draw takes waiter, for cleanup.
death_children()
{
    left=60 death_error=''
    while kill -0 "$waiter" 2>/dev/null || kill -0 "$daemon" 2>/dev/null; do
        [ "$left" -gt 0 ] || break
        sleep 1
        left=$((left - 1))
    done
    if kill -0 "$waiter" 2>/dev/null; then
        death_error="fence probe still running: $(holding "$waiter")"
        kill -KILL "$waiter" 2>/dev/null
        stuck="$stuck $waiter"
    else
        wait "$waiter" 2>/dev/null || death_error="fence probe status $?"
    fi
    waiter=''
    if kill -0 "$daemon" 2>/dev/null; then
        death_error="${death_error:+$death_error, }ivshmemd still running: $(holding "$daemon")"
        kill -KILL "$daemon" 2>/dev/null
    else
        wait "$daemon" 2>/dev/null || death_error="${death_error:+$death_error, }ivshmemd status $?"
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
    ! reap 100 "$holder" || holder=''
    # ivshmemd detaches once QEMU has gone; one left is cleanup's.
    ! reap 600 "$daemon" || daemon=''
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
# same way, one too large for either, and more held untaken than the
# driver's cap on pending asks.
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
    while [ "$n" -gt 0 ] && quiet "$console" 5 "$qemu"; do
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

# vm_ctl WORD: have bed-vm-control in the VM do WORD to the backend, stop,
# cont, kill or start, and wait, 120 s at most, for its word on the console
# that it has.
vm_ctl()
{
    had=$(tr -d '\r' < "$console" | grep -c "bed-vm: control: $1: done")
    echo "$1" >&7
    n=120
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
        quiet "$console" 600 "$qemu" || return 1
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
    "$probe" lost > "$out/$lname.out" 2>&1 &
    waiter=$!
    if [ -z "$lerr" ] && [ -n "$word" ]; then
        n=60
        until [ "$(cat "/proc/$waiter/wchan" 2>/dev/null)" = virtio_gpu_wait_answer ]; do
            n=$((n - 1))
            if [ "$n" -le 0 ] || ! kill -0 "$waiter" 2>/dev/null; then
                lerr="the probe never slept in virtio_gpu_wait_answer"
                break
            fi
            sleep 0.5
        done
        [ -n "$lerr" ] || vm_ctl "$word" || lerr="the VM did not $word the backend"
    fi
    n=$((max + 30))
    while kill -0 "$waiter" 2>/dev/null && [ "$n" -gt 0 ]; do
        sleep 1
        n=$((n - 1))
    done
    if kill -0 "$waiter" 2>/dev/null; then
        lerr="${lerr:+$lerr, }the probe still waits in $(cat "/proc/$waiter/wchan" 2>/dev/null) after $((max + 30)) s"
        kill -9 "$waiter" 2>/dev/null
    fi
    wait "$waiter" 2>/dev/null
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
    "$probe" held > "$out/held.out" 2>&1 &
    waiter=$!
    n=120
    while kill -0 "$waiter" 2>/dev/null && [ "$n" -gt 0 ]; do
        sleep 1
        n=$((n - 1))
    done
    if kill -0 "$waiter" 2>/dev/null; then
        herr="${herr:+$herr, }the probe still waits in $(cat "/proc/$waiter/wchan" 2>/dev/null) after 120 s"
        kill -9 "$waiter" 2>/dev/null
    fi
    wait "$waiter" 2>/dev/null
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
        fail draw "virtio-gpu did not bind over the region: $(tail -n 2 "$console" "$qemu_err" 2>&1 | tr '\n' ' ')"
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
    "$probe" pending 4 > "$out/pending.out" 2>&1 &
    waiter=$!
    probe_ready "$waiter"
    kill -STOP "$qemu" 2>/dev/null
    kill -CONT "$waiter" 2>/dev/null
    pending=yes
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

# started OFFSET DISPLAY RENDERER TRIES [PID]: wait until the
# session's last start since OFFSET is on DISPLAY, `BACKEND CARD RENDERER`
# as rach2-desktop logs it, its Weston has logged a GL renderer matching
# RENDERER and the user's socket exists. Give up after TRIES sleeps, or if
# PID has exited when given. Set `took` to the number of seconds slept.
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
        [ -z "${5:-}" ] || kill -0 "$5" 2>/dev/null || return 1
        sleep 1
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
    keep_new
    # shellcheck disable=SC2016 # the child shell reads its output directory
    "$keep" "$kb" sh -c '
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

# crysis_return: stop the VM after the flyby and check the restored session.
crysis_return()
{
    crysis_restore_ini || fail return 'could not restore weston.ini'
    dead=$(command stat -c %s "$services" 2>/dev/null || echo 0)
    tail -c 16384 "$services" > "$out/services-death.txt"
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
            fail "$c" "virtio-gpu did not bind over the region: card '$card', $(tail -n 2 "$console" "$qemu_err" 2>&1 | tr '\n' ' ')"
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
    if started "$offset" "drm $card gl" "$gl" 120 "$qemu"; then
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
        figout=$out/measure-$rung-$scene.out
        keep_new
        # shellcheck disable=SC2086 # assignments
        "$keep" "$kb" env $desktop_env $zink sh "$measure/measure.sh" -r "$rung" \
            -u desktop -d -o "$out/measure/$rung-$scene" "$scene" <> "$kb.ctl" > "$figout" 2>&1 &
        fig=$keepn
        await "$fig" ||
            echo "bed-shm: figure: $scene: status $await_status${await_why:+: $await_why}" >&2
        fig=''
        sed 's/^/bed-shm: figure: /' "$figout"
        if [ "$profile_only" = 1 ] && { [ -n "$await_why" ] || [ "$await_status" != 0 ]; }; then
            fail figure "$scene exited with status $await_status${await_why:+: $await_why}"
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
        keep_new
        "$keep" "$kb" su -s /bin/sh desktop \
            -c "export $desktop_env; exec glmark2-wayland --run-forever -b build" <> "$kb.ctl" > "$loadout" 2>&1 &
        load=$keepn
    else
        loadout=$out/load.out loadline=$out/load/scene.log loadpat="measure: scene: $loadscene: started"
        keep_new
        # shellcheck disable=SC2086 # two assignments
        "$keep" "$kb" env $desktop_env sh "$measure/measure.sh" -r shm-virgl -u desktop -d -t 600 \
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
    udevadm monitor --kernel --udev --subsystem-match=drm > "$out/udev-death.txt" 2>&1 &
    umon=$!
    until_line "$out/udev-death.txt" '^KERNEL - ' 30 "$umon" || true
    monitor=listening
    { grep -q '^UDEV - ' "$out/udev-death.txt" && grep -q '^KERNEL - ' "$out/udev-death.txt" &&
        kill -0 "$umon" 2>/dev/null; } ||
        monitor="not listening: $(tail -n 2 "$out/udev-death.txt" | tr '\n' ' ')"
    "$probe" pending 4 > "$out/pending.out" 2>&1 &
    waiter=$!
    probe_ready "$waiter"
    at=$(command stat -c %s "$loadout")
    kill -STOP "$qemu" 2>/dev/null
    kill -CONT "$waiter" 2>/dev/null
    pending=yes
    fences_pending || pending=no
    alive=no
    ! keep_running "$load" || alive=yes
    dead=$(command stat -c %s "$services" 2>/dev/null || echo 0)
    tail -c 16384 "$services" > "$out/services-death.txt"
    stop_vm
    # Take a second copy at QEMU's exit, before gpu.sh finishes the handoff
    # or the surviving compositor fills the log and init truncates it.
    left=10
    while [ "$left" -gt 0 ] && kill -0 "$qemu" 2>/dev/null; do
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
    # udevadm ends on SIGTERM with 0, or 143 when the signal comes first.
    # A monitor still running 100 tries after TERM is left to cleanup,
    # which kills it.
    stopped=no
    ! kill "$umon" 2>/dev/null || stopped=yes
    ustatus=0
    if gone 100 "$umon"; then
        wait "$umon" 2>/dev/null || ustatus=$?
        umon=''
    else
        ustatus=running
        case $monitor in
            listening) monitor="still running 10 s after TERM: $(holding "$umon")" ;;
            *) monitor="$monitor; still running 10 s after TERM: $(holding "$umon")" ;;
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
        ended="ended with status $(sed -n 's/^ended //p' "$out/keep-$load.status" | head -n 1)"
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
# The way out, here before the result, so that its steps count; the exit's
# trap then finds it done.
cleanup end

if [ "$bad" -eq 0 ]; then
    echo "bed-shm: result: pass"
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
