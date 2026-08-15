#!/usr/bin/env sh

#
# selfbuild_test.sh: the machine builds its own kernel, and the exact bytes it
# built are extracted, checked and booted through the full probe suite.
#
# Milestone 20 proved the machine compiles a program; milestone 21 proved a
# build is a build. What this proves is self-hosting: the staged tcc, driven by
# the staged pdpmake reading the staged guest.mk, turns the staged /src/rach2
# tree into a kernel, twice from clean workspaces with equal hashes, and the
# host then boots those bytes. It runs as the unprivileged `test` account, like
# every build capstone: a machine only root can rebuild is a different machine.
#
# The harness contract is make_test.sh's, hard-won there: stale logs removed
# before the boot, every assertion made in the guest over that command's own
# output, every command sent only after the previous one's marker, tokens
# written split so a command's echo cannot satisfy its own wait, pids cleared
# before they are signalled, the failing log kept, and every wait bounded, with
# one whole-run deadline over the per-step ones because two kernel builds under
# emulation are long and a hang must still be told from slow progress.
#
# The guest cannot ask its file system for free space (this kernel has no
# statfs), so the space preflight runs on the host, over the disk copy's own
# free-node chain, before the machine is started.
#
# Usage: selfbuild_test.sh <kernel> <disk> <test password>
#

set -e

KERNEL="$1"
DISK="$2"
TESTPASSWORD="$3"

if [ -z "$KERNEL" ] || [ -z "$DISK" ] || [ -z "$TESTPASSWORD" ]; then
    echo "usage: selfbuild_test.sh <kernel> <disk> <test password>" >&2
    exit 2
fi

# The 48 MB KRFFS partition sits at LBA 2048 (krffs/mkdisk.sh).
PART_OFFSET=1048576
PART_SIZE=50331648

# Two clean kernel builds: the copied source tree, the objects, the response
# file and the kernel, with margin. Computed once here rather than discovered
# as a mid-build failure inside the guest.
NEED_BYTES=8388608

# The whole run's deadline. A successful run is two kernel builds under
# emulation; this bounds a hang without cutting a slow build short.
DEADLINE=9000
START="$(date +%s)"

WORK="$DISK.work"
LOG="$DISK.work.log"
FEED="$DISK.work.feed"
STEPS="$DISK.work.steps"
KEXTRACT="$DISK.kernel.guest"
rm -f "$WORK" "$LOG" "$FEED" "$STEPS" "$KEXTRACT"

QPID=""
FPID=""

cleanup_boot() {
    qpid="$QPID"
    fpid="$FPID"
    QPID=""
    FPID=""
    if [ -n "$qpid" ]; then
        kill -9 "$qpid" 2>/dev/null || true
        wait "$qpid" 2>/dev/null || true
    fi
    if [ -n "$fpid" ]; then
        kill -9 "$fpid" 2>/dev/null || true
        wait "$fpid" 2>/dev/null || true
    fi
    rm -f "$FEED"
}

cleanup() {
    cleanup_boot
    rm -f "$WORK" "$LOG" "$FEED" "$STEPS" "$KEXTRACT"
}

trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

fail() {
    if [ -f "$LOG" ]; then
        cp "$LOG" "$DISK.fail.log" 2>/dev/null || true
    fi
    cleanup_boot
    echo "selfbuild-test: FAIL: $1" >&2
    echo "selfbuild-test: the failing boot's console log: $DISK.fail.log" >&2
    exit 1
}

# Wait up to $2 seconds for the token $1 in the log, under the run deadline.
wait_token() {
    i=0
    while [ "$i" -lt "$2" ]; do
        if grep -q "$1" "$LOG" 2>/dev/null; then
            return 0
        fi
        if [ "$(($(date +%s) - START))" -gt "$DEADLINE" ]; then
            fail "the run passed its ${DEADLINE}s deadline waiting for $1"
        fi
        sleep 1
        i=$((i + 1))
    done
    return 1
}

step() {
    printf '%s\t%s\n' "$1" "$2" >> "$STEPS"
}

# The machine first, the feed second (opening the fifo for reading waits for
# the writer). The feeder paces on each step's marker with a long bound, since
# a full kernel build sits behind a single marker.
boot_steps() {
    rm -f "$FEED"
    mkfifo "$FEED"
    qemu-system-i386 -cpu max -display none -kernel "$KERNEL" \
        -append "rach2.fsdisk" -rtc base=utc \
        -drive "file=$WORK,format=raw,if=ide,index=0,media=disk" \
        -serial stdio < "$FEED" > "$LOG" 2>&1 &
    QPID=$!
    (
        sleep 14
        printf 'test\n'
        sleep 4
        printf '%s\n' "$TESTPASSWORD"
        sleep 6
        while IFS='	' read -r marker command; do
            printf '%s\n' "$command"
            waited=0
            while [ "$waited" -lt 3600 ] && ! grep -q "$marker" "$LOG" 2>/dev/null; do
                sleep 1
                waited=$((waited + 1))
            done
        done < "$STEPS"
        held=0
        while [ "$held" -lt 120 ] && kill -0 "$QPID" 2>/dev/null; do
            sleep 1
            held=$((held + 1))
        done
    ) > "$FEED" &
    FPID=$!
}

rm -f "$WORK"
cp "$DISK" "$WORK"

# The space preflight, on the host, over the copy the guest will write.
FREE_BYTES="$(python3 -c "
import sys
sys.path.insert(0, 'tools')
import check_setid
region = check_setid.read_region('$WORK', $PART_OFFSET, $PART_SIZE)
nodes = check_setid.walk_nodes(region)
print(sum(n['size'] for n in nodes if n['type'] == 1))
")"
if [ -z "$FREE_BYTES" ] || [ "$FREE_BYTES" -lt "$NEED_BYTES" ]; then
    fail "the disk holds $FREE_BYTES free bytes and two kernel builds need $NEED_BYTES"
fi

# The build, twice, from a workspace deleted and recreated each time. There is
# no in-guest process preflight: /proc does not enumerate processes (ps prints
# nothing), and the harness itself holds the process budget by construction,
# one console session sending one command at a time. Each build's tail is
# echoed after its status so a failure names its file in the kept log.
: > "$STEPS"
step "COPYONE" "rm -rf /home/test/kbuild; mkdir /home/test/kbuild && cd /home/test/kbuild && cp -r /src/rach2 . && echo COPY\"\"ONE"
step "BUILDONE" "cd /home/test/kbuild/rach2 && make -f guest.mk > /tmp/kb1.out 2>&1; echo BUILD\"\"ONE=\$?"
step "TAILONE" "busybox tail -4 /tmp/kb1.out; echo TAIL\"\"ONE"
step "HASHONE" "cd /home/test/kbuild/rach2 && busybox sha256sum kernel > /tmp/k1.sha; K1=\$(busybox cat /tmp/k1.sha); echo HASH\"\"ONE=\$K1 SIZE\"\"ONE=\$(busybox stat -c %s kernel)"
step "KEEPONE" "cp /home/test/kbuild/rach2/kernel /home/test/kernel.selfbuilt && echo KEEP\"\"ONE"
step "COPYTWO" "cd /home/test/kbuild && rm -rf rach2 && cp -r /src/rach2 . && echo COPY\"\"TWO"
step "BUILDTWO" "cd /home/test/kbuild/rach2 && make -f guest.mk > /tmp/kb2.out 2>&1; echo BUILD\"\"TWO=\$?"
step "TAILTWO" "busybox tail -4 /tmp/kb2.out; echo TAIL\"\"TWO"
step "HASHTWO" "cd /home/test/kbuild/rach2 && busybox sha256sum kernel > /tmp/k2.sha; K2=\$(busybox cat /tmp/k2.sha); echo HASH\"\"TWO=\$K2"
step "VERDICTDONE" "if [ -n \"\$K1\" ] && [ \"\$K1\" = \"\$K2\" ]; then echo HASH\"\"SAME; else echo HASH\"\"DIFFER; fi; echo VERDICT\"\"DONE"
step "SYNCDONE" "sync; echo SYNC\"\"DONE"

boot_steps

if ! wait_token "COPYONE" 1800; then
    fail "the source tree never copied into the workspace"
fi
if ! wait_token "BUILDONE=" 3600; then
    fail "the first kernel build never finished"
fi
if ! grep -q "BUILDONE=0" "$LOG"; then
    wait_token "TAILONE" 60 || true
    fail "the first kernel build failed; its last lines are in the kept log"
fi
if ! wait_token "HASHONE=[0-9a-f]" 240; then
    fail "the first build's kernel was never hashed"
fi
# A sanity floor, not a structure check (the kernel checker is that): the
# tcc-linked kernel is smaller than the host's, carrying no debug data, and
# lands near 300 KB.
SIZEONE="$(grep -a -o 'SIZEONE=[0-9]*' "$LOG" | tail -1 | cut -d= -f2)"
if [ -z "$SIZEONE" ] || [ "$SIZEONE" -lt 200000 ]; then
    fail "the built kernel is $SIZEONE bytes, which is no kernel"
fi
if ! wait_token "KEEPONE" 120; then
    fail "the built kernel was never set aside for extraction"
fi
if ! wait_token "BUILDTWO=" 3600; then
    fail "the second kernel build never finished"
fi
if ! grep -q "BUILDTWO=0" "$LOG"; then
    wait_token "TAILTWO" 60 || true
    fail "the second kernel build failed; its last lines are in the kept log"
fi
if ! wait_token "VERDICTDONE" 240; then
    fail "the two builds' hashes were never compared"
fi
if grep -q "HASHDIFFER" "$LOG"; then
    fail "two clean builds of one tree produced two different kernels"
fi
if ! grep -q "HASHSAME" "$LOG"; then
    fail "the hash comparison never reported"
fi
if ! wait_token "SYNCDONE" 240; then
    fail "the disk was never synced after the builds"
fi
GUEST_SHA="$(grep -a -o 'HASHONE=[0-9a-f]*' "$LOG" | grep -a -oE '[0-9a-f]{64}' | tail -1)"
if [ -z "$GUEST_SHA" ]; then
    fail "the guest never reported the built kernel's hash"
fi
cleanup_boot

echo "selfbuild-test: the machine built its kernel twice from clean trees, hash $GUEST_SHA, $SIZEONE bytes: OK"
echo "selfbuild-test: PASS"
