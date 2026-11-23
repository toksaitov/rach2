#!/bin/sh
# bed.sh: the test bed's rungs through a VM on the Lenovo's GPU, as the
# native cases bed/* run 83-7's step in the release's boot.
#
#   bed.sh shm SUMS
#   bed.sh forward virgl|venus SUMS
#
# Run as root by etude-guest in the case's workspace, in a mount namespace of
# the case's own, on the GNU root with its KVM layer and the Intel driver
# bound. The workspace holds the runners (tests/kvm/bed-shm.sh with the
# keeper's source, bedkeep.c, which it builds, tests/assign/gpu.sh,
# bed/forward/forward.sh), vgshmprobe, the harness
# (bed/measure) with the graphics runners it uses, and 82-4b's check.sh with
# the source of 78-10d's crcmodes, which the root's cc, TinyCC, builds here
# with -static, as it requires of every program (tcc(1)). The VMs'
# disks, which only an x86-64 Linux host builds, are on the data partition,
# /dev/disk0p6, in native/bed: vm.disk, the bed's VM raw, and guest.qcow2,
# guest.tar and id_ed25519, the forward guest's disk, archive and key. SUMS,
# tests/kvm/ref/lenovo-bed.sha256, has the SHA-256 of each. The partition is
# mounted read-only, and unmounted at the exit; when it is mounted already,
# which a second mount of it would find busy, the script reads it where it
# is and leaves it mounted. QEMU opens each disk with snapshot=on and writes
# nothing to it.
#
#   shm      bed-shm.sh -w -g 0000:00:02.0: the session on the region's
#            virtio-gpu card drawing through i915 in the VM, each scene's
#            figure on the rungs shm-virgl and shm-venus, glmark2 as the
#            load, the VM killed under it, the session headless on llvmpipe
#            and back on Rach2's driver (83-7.5's shm rows, 83-7.6)
#   forward  forward.sh's glmark2 scene in the Debian guest of QEMU on
#            the render node check.sh finds for i915, through virgl or
#            Venus (83-7.5's forward rows)
#
# The shm rung's VM has the machine's MemTotal less a reserve for Rach2 and
# its session, up to a cap, which the script prints as `bed: vm: ...`; a
# piglit test that needs more of the VM's memory than that is skipped, and
# its line says so. The rung passes only when bed-shm.sh passed, every check
# that ran among it, and the game's measurement completed.
#
# Before the run check.sh reads the GPU's state and the panel's pipe CRC,
# and after it reads them again: the driver i915, the same CRC, Crocus and
# both readbacks. check.sh ends the desktop's session; SIGHUP starts it after
# the first check, since bed-shm.sh's rows draw in it.
#
# A session that is starting or ending when the script begins, some of the
# desktop user's processes running but no Weston serving its socket, is
# given 60 s to settle to `running` or `none`; one that does not, or a state
# that pgrep cannot read, is refused before anything changes. An exit
# before the first check touches no session and prints `bed: session:
# before STATE, after STATE, untouched`.
#
# From the first check on, every exit leaves the session as the script
# found it, judged once the GPU's return has settled: udev's queue empty
# and two turns of rach2-desktop's loop begun since, so that every card
# event and signal before has been handled. When none ran, SIGUSR2 is sent
# whether or not a Weston runs, since rach2-desktop may hold a session whose
# Weston has not started or has gone and start it again on a card's event;
# then no process of the desktop user may run. When one ran, SIGHUP starts
# it unless it runs. rach2-desktop's lines in the services log since the
# start are printed as `bed: desktop: LINE`, then `bed: session: before
# STATE, after STATE, restored yes|no`, STATE `running`, `none`, `partial: N
# processes` or `unknown` when pgrep failed, and a run fails that leaves a
# session where none ran, or none where one ran, or whose restore did not
# end within its waits. HUP, INT and TERM are ignored while the exit's
# cleanup runs, so that a second signal cannot cut the restore short. The
# last line is `bed: result: pass` or `fail: why`, and `pass: N skipped`
# when bed-shm.sh skipped N tests.
set -eu

here=$(pwd)
data=/dev/disk0p6
gpu=0000:00:02.0
pidfile=/run/rach2-desktop.pid
socket=/run/user/1001/wayland-0
user=desktop services=/var/log/services
mounted='' before='' touched='' result=''
# Where the services log stood at the start.
offset=$(stat -c %s "$services" 2> /dev/null || echo 0)
# The memory the shm rung keeps out of its VM, in MiB: Rach2 with no
# session uses 233 MiB of the Lenovo's 16,247, and in QEMU Rach2 used at
# most 432 MiB beside QEMU's resident memory during the game; the reserve
# holds that, QEMU's own memory beyond the VM's and the page cache the
# scenes' files need (dev-log/84-locks/bed-memory.md). The VM gets at most
# the cap, above the most any check needs.
reserve=3072 cap=16384

# fail WHY: end the run with `bed: result: fail: WHY`, which the exit trap
# prints after the session's line.
fail()
{
    result="fail: $*"
    exit 1
}

# cleanup: unmount what the script mounted, leave the session as it was
# found once the script may have changed it and print its line, and print
# the result last: the one fail or the end gave, or the status of an exit
# neither gave, by a signal or set -e. HUP, INT and TERM are ignored from
# its start, and from the start of their own traps, which end in it.
# shellcheck disable=SC2329 # the exit trap runs it
cleanup()
{
    st=$?
    trap '' HUP INT TERM
    trap - EXIT
    [ -z "$mounted" ] || umount "$mounted" 2> /dev/null || umount -l "$mounted" 2> /dev/null || :
    if [ -n "$before" ] && [ -z "$touched" ]; then
        echo "bed: session: before $before, after $(session_state), untouched"
    elif [ -n "$before" ]; then
        restored=yes
        session_restore || restored=no
        tail -c "+$((offset + 1))" "$services" 2> /dev/null | grep '^rach2-desktop: ' | sed 's/^/bed: desktop: /'
        after=$(session_state)
        echo "bed: session: before $before, after $after, restored $restored"
        # A session that ran must run again; with none before, nothing of
        # the desktop user's may be left; and the supervisor must have done
        # what it was asked within the waits.
        case $restored:$before:$after in
            yes:running:running | yes:none:none) ;;
            *)
                [ "$st" != 0 ] || result="fail: the session is not left as it was found: before $before, after $after, restored $restored"
                [ "$st" != 0 ] || st=1
                ;;
        esac
    fi
    [ -n "$result" ] || result="fail: status $st"
    echo "bed: result: $result"
    exit "$st"
}
trap cleanup EXIT
trap 'trap "" HUP INT TERM; exit 129' HUP
trap 'trap "" HUP INT TERM; exit 130' INT
trap 'trap "" HUP INT TERM; exit 143' TERM

# mount_point DEV: where the block device DEV is mounted, empty when it is
# not, found by its device number, since the mount table may name it by
# another path.
mount_point()
{
    dev=$(stat -L -c %t:%T "$1") || return 0
    while read -r src dir _; do
        if [ -b "$src" ] && [ "$(stat -L -c %t:%T "$src")" = "$dev" ]; then
            echo "$dir"
            return 0
        fi
    done < /proc/mounts
}

# procs ARGS...: pgrep ARGS's lines. Its status 1, no process matched, is
# an empty answer; any other failure is named on stderr and returns 2, since
# a pgrep that could not look says nothing of what runs.
procs()
{
    pst=0
    pgrep "$@" || pst=$?
    [ "$pst" -le 1 ] && return 0
    echo "bed: pgrep $*: status $pst" >&2
    return 2
}

# session_up: 0 when Weston serves the session's socket, 1 when it does not,
# 2 when pgrep failed.
session_up()
{
    [ -S "$socket" ] || return 1
    w=$(procs -x weston) || return 2
    [ -n "$w" ]
}

# session_state: `running` when Weston serves the session's socket, `none`
# when the desktop user runs nothing, `partial: N processes`, a session
# starting or ending, and `unknown` when pgrep failed.
session_state()
{
    up=0
    session_up || up=$?
    case $up in
        0) echo running ;;
        1)
            if ! p=$(procs -a -u "$user"); then
                echo unknown
            elif [ -z "$p" ]; then
                echo none
            else
                echo "partial: $(printf '%s\n' "$p" | wc -l) processes"
            fi
            ;;
        *) echo unknown ;;
    esac
}

# turns: wait until rach2-desktop, process $sup, has begun two turns of its
# loop since the call, each turn ending in a `sleep 1` child of its own;
# the second reads every flag a signal sent before the call set, and a
# session it stopped or started by then has stopped or started whole. 1200
# looks of 0.1 s at most.
turns()
{
    last=$(procs -P "$sup" -fx 'sleep 1') || return 1
    seen=0 tries=0
    while [ "$seen" -lt 2 ]; do
        kill -0 "$sup" 2> /dev/null || return 1
        tries=$((tries + 1))
        [ "$tries" -le 1200 ] || return 1
        sleep 0.1
        now=$(procs -P "$sup" -fx 'sleep 1') || return 1
        if [ -n "$now" ] && [ "$now" != "$last" ]; then
            seen=$((seen + 1)) last=$now
        fi
    done
}

# settle: wait until the GPU's return has reached rach2-desktop and it has
# acted on it: udev's queue empty, 60 s at most, so that the card's add has
# gone to its monitor, then two turns of its loop.
settle()
{
    [ -f "$pidfile" ] || return 0
    sup=$(cat "$pidfile")
    udevadm settle --timeout=60 2> /dev/null || :
    turns
}

# session_start: have the desktop's supervisor start the session, as
# check.sh ends it, and wait for its socket, 600 tries of 0.1 s at most.
# Fails when pgrep does.
session_start()
{
    up=0
    session_up || up=$?
    [ "$up" != 0 ] || return 0
    [ "$up" = 1 ] || return 1
    [ -f "$pidfile" ] || return 1
    kill -HUP "$(cat "$pidfile")"
    tries=0
    while :; do
        up=0
        session_up || up=$?
        [ "$up" != 0 ] || return 0
        [ "$up" = 1 ] || return 1
        tries=$((tries + 1))
        [ "$tries" -lt 600 ] || return 1
        sleep 0.1
    done
}

# session_end: have the supervisor end any session and keep none, whether
# or not a Weston runs, and wait for two turns of its loop and then until no
# process of the desktop user runs, 600 looks of 0.1 s at most.
session_end()
{
    [ -f "$pidfile" ] || return 0
    sup=$(cat "$pidfile")
    kill -USR2 "$sup" || return 1
    turns || return 1
    tries=0
    while :; do
        p=$(procs -u "$user") || return 1
        [ -n "$p" ] || return 0
        tries=$((tries + 1))
        [ "$tries" -lt 600 ] || return 1
        sleep 0.1
    done
}

# session_restore: once the GPU's return has settled, leave the session as
# the script found it.
session_restore()
{
    settle || return 1
    if [ "$before" = running ]; then
        session_start
    else
        session_end
    fi
}

case ${1:-}:$# in
    shm:2) rung=shm sums=$2 ;;
    forward:3) rung=forward profile=$2 sums=$3 ;;
    *)
        echo "usage: bed.sh shm SUMS | bed.sh forward virgl|venus SUMS" >&2
        exit 2
        ;;
esac
# The session as found, settled before anything changes: a partial one
# gets 600 looks of 0.1 s to become running or none.
before=$(session_state)
tries=0
while :; do
    case $before in
        running | none) break ;;
        unknown)
            before=''
            fail "pgrep could not read the desktop session's state; nothing was changed"
            ;;
    esac
    tries=$((tries + 1))
    if [ "$tries" -gt 600 ]; then
        found=$before before=''
        fail "the desktop session is $found and did not settle in 60 s; nothing was changed"
    fi
    sleep 0.1
    before=$(session_state)
done
echo "bed: session: found $before"
sums=$(cd "$(dirname "$sums")" && pwd)/$(basename "$sums")
[ -f "$sums" ] || fail "no $sums"

# The kit: the runners, the harness and its runners apart, crcmodes built.
rm -rf kit out
mkdir -p kit/measure/runners out
for f in bed-shm.sh bedkeep.c gpu.sh forward.sh check.sh; do
    cp "$f" kit/
done
for f in measure.sh piglit.txt latency.c webgl.html; do
    cp "$f" kit/measure/
done
for f in common.sh games.sh piglit.sh; do
    cp "$f" kit/measure/runners/
done
cc -std=c99 -static -O2 -o kit/crcmodes crcmodes.c > crcmodes.build 2>&1 ||
    fail "crcmodes did not build: $(tail -n 3 crcmodes.build)"
chmod -R a+rX kit

# The disks, checked in place on the partition: where it is mounted
# already, or mounted read-only here.
at=$(mount_point "$data")
if [ -n "$at" ]; then
    echo "bed: data: $data mounted at $at already, read there"
else
    mkdir -p data
    mount -t ext2 -o ro "$data" data || fail "$data would not mount read-only"
    mounted=$here/data
    at=$mounted
fi
bed=$at/native/bed
(cd "$bed" && grep -v '^#' "$sums" | sha256sum -c) > disks.check 2>&1 || {
    cat disks.check
    fail "a disk differs from $sums"
}

# The shm rung's VM: MemTotal less the reserve, at most the cap.
if [ "$rung" = shm ]; then
    total=$(awk '$1 == "MemTotal:" { print int($2 / 1024) }' /proc/meminfo)
    mib=$((total - reserve))
    [ "$mib" -le "$cap" ] || mib=$cap
    [ "$mib" -ge 2048 ] || fail "MemTotal $total MiB leaves the VM $mib MiB, below 2048"
    echo "bed: vm: $mib MiB: MemTotal $total MiB less $reserve kept for Rach2 and its session, at most $cap"
fi

touched=yes
sh kit/check.sh before > check-before.txt 2>&1 || :
cat check-before.txt
grep -q '^check: before: result: pass' check-before.txt || fail "check.sh before did not pass"
want=$(sed -n 's/^check: before: crcmodes: mode 1366x768 aspect crc \(0x[0-9a-f]*\).*/\1/p' check-before.txt | head -n 1)
session_start || fail "the session did not start after check.sh"

status=0
case $rung in
    shm)
        MEASURE_RUNNERS=$here/kit/measure/runners sh kit/bed-shm.sh -d "$bed/vm.disk" \
            -p ./vgshmprobe -w -x "$here/kit/measure" -S 'glmark2 piglit game venus:glmark2' \
            -g "$gpu" -m "$mib" -o "$here/out" > run.out 2>&1 || status=$?
        cat run.out
        grep -q '^bed-shm: result: pass' run.out || status=1
        grep -q '^bed-shm: scene: pass: game on shm-virgl$' run.out || {
            echo "bed: the game's measurement did not complete"
            status=1
        }
        ;;
    forward)
        cp "$bed/id_ed25519" key && chmod 600 key
        if [ "$profile" = venus ]; then
            export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/intel_hasvk_icd.json
        fi
        # i915's render node as check.sh found it, since a card that has
        # not gone yet may hold renderD128.
        node=$(sed -n 's/^check: before: driver i915, card[0-9]*, \(renderD[0-9]*\)$/\1/p' check-before.txt)
        RENDER_NODE=/dev/dri/${node:-renderD128} sh kit/forward.sh -o "$here/out" -f qcow2 \
            -d "$bed/guest.qcow2" -k key -g "$bed/guest.tar" -M glmark2 "$profile" > run.out 2>&1 ||
            status=$?
        cat run.out
        ;;
esac

sh kit/check.sh after > check-after.txt 2>&1 || :
cat check-after.txt
[ "$status" = 0 ] || fail "the $rung run failed, status $status"
grep -q '^check: after: result: pass' check-after.txt || fail "check.sh after did not pass"
grep -q "crcmodes: mode 1366x768 aspect crc ${want:-none}" check-after.txt ||
    fail "the pipe's CRC is not ${want:-none}, as before the run"
skips=$(sed -n 's/^bed-shm: result: pass: \([0-9]*\) skipped$/\1/p' run.out)
result="pass${skips:+: $skips skipped}"
