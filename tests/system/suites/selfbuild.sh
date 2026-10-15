#!/usr/bin/env sh

#
# selfbuild.sh: the machine builds its own kernel, and the exact bytes it
# built are extracted, checked and booted through the full probe suite.
#
# The staged tcc, driven by the staged pdpmake reading the staged guest.mk,
# turns the staged /src/rach2 tree into a kernel, twice from clean
# workspaces with equal hashes, and the host then boots those bytes. It runs
# as the unprivileged `test` account, as the other build suites do, since a
# build that only root can run is not the one a user has.
#
# The console is driven as make.sh drives it: stale logs removed before the
# boot, every check made in the guest over that command's own output, every
# command sent only after the previous one's marker, tokens written split so a
# command's echo cannot satisfy its own wait, pids cleared before they are
# signalled, the failing log kept, and every wait bounded, with one deadline
# for the whole run over the per-step ones, because two kernel builds under
# emulation are long and a hang must still be told from slow progress.
#
# The free space is checked on the host, over the disk copy's free-node chain
# or the ext2 volume's statistics, before the machine is started.
#
# The run ends with a second, short boot as root that rebuilds the staged
# /src/ish in place, with no copy and no touch, as a person would; staged
# files with zero timestamps fail it, since pdpmake reads a zero modification
# time as a file that does not exist.
#
# Usage: selfbuild.sh <kernel> <disk> <test password> <root password>
#
# With SELFBUILD_ROOT=ext2 in the environment the disk's partition is an ext2
# volume, booted with rach2.root=ext2, its free space read by debugfs and the
# built kernel extracted by debugfs; the guest builds the kernel once, and
# the second build, the hash comparison, the userland builds and their smoke
# checks are left to the KRFFS run, since a program's bytes are the same
# whichever root it was built on. The ext2 run is one clean build of the
# kernel and the boot of what it built.
#

# The architecture, its build tree and its emulator, as
# tests/system/suites.mk names them. The machine builds its own
# architecture's kernel, with its own TinyCC.
RACH2_ARCH=${RACH2_ARCH:-i686}
ARCH_BUILD=${ARCH_BUILD:-build}
QEMU=${RACH2_QEMU:-qemu-system-i386}

# The runner helper, whose `until` and `gone` are the suite's waits.
RUNUTIL="$(cd "$(dirname "$0")/../../.." && pwd)/build/tests/system/runutil"
set -e

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"

KERNEL="$1"
DISK="$2"
TESTPASSWORD="$3"
ROOTPASSWORD="$4"

if [ -z "$KERNEL" ] || [ -z "$DISK" ] || [ -z "$TESTPASSWORD" ] ||
   [ -z "$ROOTPASSWORD" ]; then
    echo "usage: selfbuild_test.sh <kernel> <disk> <test password> <root password>" >&2
    exit 2
fi

# The 48 MB partition sits at LBA 2048 (krffs/mkdisk.sh, or the ext2 disk
# the suite's rule assembles the same way).
PART_OFFSET=1048576
PART_SIZE=50331648
ROOT_TYPE="${SELFBUILD_ROOT:-krffs}"
case "$ROOT_TYPE" in
    krffs) ROOT_TOKEN="rach2.fsdisk" ;;
    ext2) ROOT_TOKEN="rach2.root=ext2"; . "$REPO/tools/setup/e2fsprogs_path.sh" ;;
    *) echo "selfbuild_test.sh: SELFBUILD_ROOT is krffs or ext2" >&2; exit 2 ;;
esac

# The space two clean kernel builds need: the copied source tree, the
# objects, the response file and the kernel, with margin, checked here so a
# full disk does not fail a build inside the guest. The x86-64 objects and
# kernel are larger: its first build and the kernel set aside take about
# 13 MiB.
case "$RACH2_ARCH" in
    x86_64) NEED_BYTES=16777216 ;;
    *) NEED_BYTES=8388608 ;;
esac

# The whole run's deadline. A successful run is two kernel builds under
# emulation; this bounds a hang without cutting a slow build short.
# shellcheck source=tests/system/deadline.sh
. "$REPO/tests/system/deadline.sh"
DEADLINE=$(scaled 9000)
START="$(date +%s)"

WORK="$DISK.work"
LOG="$DISK.work.log"
FEED="$DISK.work.feed"
STEPS="$DISK.work.steps"
KEXTRACT="$DISK.kernel.guest"
rm -f "$WORK" "$LOG" "$FEED" "$STEPS" "$KEXTRACT"

QPID=""
FPID=""

# cleanup_boot: stop the machine and its feeder and remove the FIFO; safe to
# call twice, since each pid is cleared before it is signalled.
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

# cleanup: stop the boot and remove the working files. When the run did not
# pass, which includes a stop by the runner, the console log is kept as
# $DISK.fail.log, the only record of what the machine did, and the disk the
# machine wrote as $DISK.fail.work, so the state a failure left in the file
# system can be read on the host.
PASSED=""
cleanup() {
    cleanup_boot
    if [ -z "$PASSED" ] && [ -f "$LOG" ]; then
        cp "$LOG" "$DISK.fail.log" 2>/dev/null || true
        mv "$WORK" "$DISK.fail.work" 2>/dev/null || true
    fi
    rm -f "$WORK" "$WORK.p1" "$LOG" "$FEED" "$STEPS" "$KEXTRACT"
}

trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

# fail REASON: keep the console log as $DISK.fail.log, stop the boot, print
# REASON and exit 1.
fail() {
    if [ -f "$LOG" ]; then
        cp "$LOG" "$DISK.fail.log" 2>/dev/null || true
    fi
    cleanup_boot
    echo "selfbuild-test: FAIL: $1" >&2
    echo "selfbuild-test: the failing boot's console log: $DISK.fail.log" >&2
    exit 1
}

# wait_token TOKEN SECONDS: wait up to SECONDS for TOKEN in the log; return 1
# if it does not come, and fail once the run's deadline has passed.
wait_token() {
    "$RUNUTIL" until "$LOG" "$1" "$2" && return 0
    if [ "$(($(date +%s) - START))" -gt "$DEADLINE" ]; then
        fail "the run passed its ${DEADLINE}s deadline waiting for $1"
    fi
    return 1
}

# step MARKER COMMAND: add COMMAND to the steps, typed after the step before
# it has printed its marker; MARKER is what COMMAND prints when done.
step() {
    printf '%s\t%s\n' "$1" "$2" >> "$STEPS"
}

# The console is driven through tests/system/drive.sh: a line is typed when
# the output it answers has arrived, under a bound.
# shellcheck source=tests/system/drive.sh
. "$REPO/tests/system/drive.sh"
# boot_steps NAME PASSWORD: boot the working disk, log in as NAME, and type
# each step once the one before it has printed its marker. The machine starts
# first and the feed second, since opening the FIFO for reading waits for the
# writer. A full build is behind a single marker, so a step's bound is what
# is left of the run's deadline, and a hung build fails when the deadline
# passes rather than an hour after it.
boot_steps() {
    login_name="$1"
    login_password="$2"
    rm -f "$FEED"
    mkfifo "$FEED"
    "$QEMU" -cpu max -display none -kernel "$KERNEL" \
        -append "$ROOT_TOKEN" -rtc base=utc \
        -drive "file=$WORK,format=raw,if=ide,index=0,media=disk" \
        -serial stdio < "$FEED" > "$LOG" 2>&1 &
    QPID=$!
    DRIVE_LOG=$LOG
    DRIVE_FIFO=$FEED
    drive_feed "$QPID"
    FPID=$DRIVE_FPID
    drive_login "$login_name" "$login_password" "$(scaled 90)" || fail "the machine did not log in at its console"
    while IFS='	' read -r marker command; do
        remaining=$(( START + DEADLINE - $(date +%s) ))
        [ "$remaining" -gt 0 ] || fail "the run's budget was spent before $marker"
        drive_type "$command"
        drive_expect_raw "$marker" "$remaining" || fail "the step for $marker never printed it"
    done < "$STEPS"
}

rm -f "$WORK"
cp "$DISK" "$WORK"

# The free space, read on the host from the copy the guest will write.
if [ "$ROOT_TYPE" = ext2 ]; then
    dd if="$WORK" bs=1048576 skip=1 count=48 2>/dev/null > "$WORK.p1"
    FREE_BYTES="$(debugfs -R stats "$WORK.p1" 2>/dev/null | awk '/^Free blocks:/ { print $3 * 4096 }')"
else
    FREE_BYTES="$(build/tools/inspect/setidcheck --free --image "$WORK" \
        --offset "$PART_OFFSET" --size "$PART_SIZE")"
fi
if [ -z "$FREE_BYTES" ] || [ "$FREE_BYTES" -lt "$NEED_BYTES" ]; then
    fail "the disk has $FREE_BYTES free bytes and two kernel builds need $NEED_BYTES"
fi

# The build, twice, from a workspace deleted and recreated each time, by one
# console session sending one command at a time. Each build's tail is echoed
# after its status so a failure names its file in the kept log.
: > "$STEPS"
step "COPYONE" "rm -rf /home/test/kbuild; mkdir /home/test/kbuild && cd /home/test/kbuild && cp -r /src/rach2 . && echo COPY\"\"ONE"
step "BUILDONE" "cd /home/test/kbuild/rach2 && make -f guest.mk > /tmp/kb1.out 2>&1; echo BUILD\"\"ONE=\$?"
step "TAILONE" "busybox tail -4 /tmp/kb1.out; echo TAIL\"\"ONE"
step "HASHONE" "cd /home/test/kbuild/rach2 && busybox sha256sum kernel > /tmp/k1.sha; K1=\$(busybox cat /tmp/k1.sha); echo HASH\"\"ONE=\$K1 SIZE\"\"ONE=\$(busybox stat -c %s kernel) SIZE\"\"END"
step "KEEPONE" "cp /home/test/kbuild/rach2/kernel /home/test/kernel.selfbuilt && echo KEEP\"\"ONE"
step "DUONE" "busybox du -sk /home/test/kbuild /tmp /home/test/kernel.selfbuilt; busybox df -k /home/test; echo DU\"\"ONE"
# The second build, the hash comparison, the userland builds and their
# smoke checks run on the KRFFS root only, as the header says.
if [ "$ROOT_TYPE" = krffs ]; then
step "COPYTWO" "cd /home/test/kbuild && rm -rf rach2 && cp -r /src/rach2 . && echo COPY\"\"TWO"
step "DUTWO" "busybox du -sk /home/test/kbuild /tmp; busybox df -k /home/test; echo DU\"\"TWO"
step "BUILDTWO" "cd /home/test/kbuild/rach2 && make -f guest.mk > /tmp/kb2.out 2>&1; echo BUILD\"\"TWO=\$?"
step "TAILTWO" "busybox tail -4 /tmp/kb2.out; echo TAIL\"\"TWO"
step "HASHTWO" "cd /home/test/kbuild/rach2 && busybox sha256sum kernel > /tmp/k2.sha; K2=\$(busybox cat /tmp/k2.sha); echo HASH\"\"TWO=\$K2"
step "VERDICTDONE" "if [ -n \"\$K1\" ] && [ \"\$K1\" = \"\$K2\" ]; then echo HASH\"\"SAME; else echo HASH\"\"DIFFER; fi; echo VERDICT\"\"DONE"
step "UCOPYDONE" "cd /home/test/kbuild && rm -rf utils ish && cp -r /src/utils . && cp -r /src/ish . && echo UCOPY\"\"DONE"
step "UBUILDRC" "cd /home/test/kbuild/utils && make -f guest.mk > /tmp/ub.out 2>&1; echo UBUILD\"\"RC=\$?"
step "UBTAIL" "busybox tail -4 /tmp/ub.out; echo UB\"\"TAIL"
step "IBUILDRC" "cd /home/test/kbuild/ish && make -f guest.mk > /tmp/ib.out 2>&1; echo IBUILD\"\"RC=\$?"
step "IBTAIL" "busybox tail -4 /tmp/ib.out; echo IB\"\"TAIL"
step "SMOKECAT" "cd /home/test/kbuild/utils && busybox echo hi > /tmp/sf && [ \"\$(./cat /tmp/sf)\" = hi ] && echo SMOKE\"\"CAT"
step "SMOKEECHO" "[ \"\$(./echo alpha)\" = alpha ] && echo SMOKE\"\"ECHO"
step "SMOKETF" "./true && ! ./false && echo SMOKE\"\"TF"
step "SMOKEPWD" "[ \"\$(cd /tmp && /home/test/kbuild/utils/pwd)\" = /tmp ] && echo SMOKE\"\"PWD"
step "SMOKELS" "./ls /tmp | busybox grep -q sf && echo SMOKE\"\"LS"
step "SMOKETM" "./touch /tmp/st && [ -f /tmp/st ] && ./mkdir /tmp/sd && [ -d /tmp/sd ] && echo SMOKE\"\"TM"
step "SMOKEMCR" "./cp /tmp/sf /tmp/sc && busybox cmp -s /tmp/sf /tmp/sc && ./mv /tmp/sc /tmp/sm && [ -f /tmp/sm ] && ./rm /tmp/sm /tmp/st && [ ! -f /tmp/sm ] && echo SMOKE\"\"MCR"
step "SMOKEYES" "./yes | busybox head -n 1 | busybox grep -qx y && echo SMOKE\"\"YES"
step "SMOKEPING" "./ping -c 1 127.0.0.1 > /dev/null 2>&1 && echo SMOKE\"\"PING"
# Port 8080 and not 80: these steps run as an ordinary user, and a port below
# 1024 is root's, so the self-built server is asked for one this user may have.
# Two steps and not one, since the console keeps 255 bytes of a line typed
# while the shell has not yet taken the terminal back from the command before.
# interval: the self-built server is asked every second until it answers
step "WEBUP" "./httpd 8080 > /dev/null 2>&1 & i=0; until printf 'GET / HTTP/1.0\r\n\r\n' | ./nc 127.0.0.1 8080 2>/dev/null | busybox grep -q 200 || [ \$i -ge 30 ]; do i=\$((i + 1)); sleep 1; done; echo WEB\"\"UP"
step "SMOKEWEB" "printf 'GET / HTTP/1.0\r\n\r\n' | ./nc 127.0.0.1 8080 | busybox grep -q HTTP/ && echo SMOKE\"\"WEB; kill %% 2>/dev/null"
# The refusal statuses above are exact (1 for each credential refusal, and
# sudo run against its permission check, not only its usage line), so an exec
# failure (126, 127) or a crash (128 plus a signal) cannot pass as a refusal.
step "SMOKEBUILT" "[ -x halt ] && [ -x init-shell ] && [ -x init-login ] && [ -x adduser-crash ] && echo SMOKE\"\"BUILT"
step "SMOKELOGIN" "./login < /dev/null > /dev/null 2>&1; [ \$? -eq 1 ] && echo SMOKE\"\"LOGIN"
step "SMOKESU" "./su -c true < /dev/null > /dev/null 2>&1; [ \$? -eq 1 ] && echo SMOKE\"\"SU"
step "SMOKESUDO" "./sudo true > /dev/null 2>&1; [ \$? -eq 1 ] && echo SMOKE\"\"SUDO"
step "SMOKEPASSWD" "./passwd < /dev/null > /dev/null 2>&1; [ \$? -eq 1 ] && echo SMOKE\"\"PASSWD"
step "SMOKEADD" "./adduser > /dev/null 2>&1; [ \$? -eq 1 ] && echo SMOKE\"\"ADD"
step "SMOKEINSTK" "./bootctl kernel ./cat > /dev/null 2>&1; [ \$? -eq 1 ] && echo SMOKE\"\"INSTK"
step "SMOKEISH" "cd /home/test/kbuild/ish && [ \"\$(./ish -c '/home/test/kbuild/utils/echo selfbuilt')\" = selfbuilt ] && echo SMOKE\"\"ISH"
fi
step "SYNCDONE" "sync; echo SYNC\"\"DONE"

boot_steps test "$TESTPASSWORD"

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
# The size follows the hash on one line, so reading it as soon as `HASHONE=`
# appears can read a number the machine is still typing, a prefix of the real
# size. The wait is for the marker that follows the value.
if ! wait_token "SIZEEND" 240; then
    fail "the first build's kernel was never hashed"
fi
# A sanity floor, not a structure check (the kernel checker is that): the
# tcc-linked kernel is about 1.7 MB on i686 and 2.0 MB on x86-64, a little
# larger than the host's, and a size under 200000 bytes means the build left
# a truncated or empty file.
SIZEONE="$(grep -a -o 'SIZEONE=[0-9]*' "$LOG" | tail -1 | cut -d= -f2)"
if [ -z "$SIZEONE" ] || [ "$SIZEONE" -lt 200000 ]; then
    fail "the built kernel is $SIZEONE bytes, which is no kernel"
fi
if ! wait_token "KEEPONE" 120; then
    fail "the built kernel was never set aside for extraction"
fi
if [ "$ROOT_TYPE" = krffs ]; then
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
if ! wait_token "UBUILDRC=" 1200; then
    fail "the utils build never finished"
fi
if ! grep -q "UBUILDRC=0" "$LOG"; then
    wait_token "UBTAIL" 60 || true
    fail "the utils build failed; its last lines are in the kept log"
fi
if ! wait_token "IBUILDRC=" 600; then
    fail "the ish build never finished"
fi
if ! grep -q "IBUILDRC=0" "$LOG"; then
    wait_token "IBTAIL" 60 || true
    fail "the ish build failed; its last lines are in the kept log"
fi
for smoke in SMOKECAT SMOKEECHO SMOKETF SMOKEPWD SMOKELS SMOKETM SMOKEMCR \
             SMOKEYES SMOKEPING SMOKEWEB SMOKEBUILT SMOKELOGIN SMOKESU \
             SMOKESUDO SMOKEPASSWD SMOKEADD SMOKEINSTK SMOKEISH; do
    if ! wait_token "$smoke" 240; then
        fail "the userland smoke check $smoke did not pass"
    fi
done
fi
if ! wait_token "SYNCDONE" 240; then
    fail "the disk was never synced after the builds"
fi
GUEST_SHA="$(grep -a -o 'HASHONE=[0-9a-f]*' "$LOG" | grep -a -oE '[0-9a-f]{64}' | tail -1)"
if [ -z "$GUEST_SHA" ]; then
    fail "the guest never reported the built kernel's hash"
fi
cleanup_boot

# The kernel's bytes, checked on the host. With the machine stopped, the
# exact kernel it built is extracted from the synced disk copy, its hash must
# equal the one the guest recorded, the structural checker must accept the
# tcc layout, and then those bytes boot the full probe suite.
#
# sha256_of FILE: print the SHA-256 of FILE, building the tree's own sha256
# tool first if it is missing.
sha256_of() {
    if [ ! -x "$REPO/build/tools/verify/sha256" ]; then
        mkdir -p "$REPO/build/tools/verify"
        cc -O2 -o "$REPO/build/tools/verify/sha256" \
            "$REPO/tools/verify/sha256.c" || return 1
    fi
    "$REPO/build/tools/verify/sha256" "$1" | awk '{print $1}'
}

if [ "$ROOT_TYPE" = ext2 ]; then
    # The machine was synced and then stopped, so the volume is dirty and
    # must be consistent: e2fsck's forced check finds nothing to repair.
    dd if="$WORK" bs=1048576 skip=1 count=48 2>/dev/null > "$WORK.p1"
    e2fsck -fn "$WORK.p1" > "$DISK.fsck.log" 2>&1 || \
        fail "the volume the machine built on is not consistent after the synced stop"
    if ! debugfs -R "dump /home/test/kernel.selfbuilt $KEXTRACT" "$WORK.p1" > /dev/null 2>&1 ||
       [ ! -s "$KEXTRACT" ]; then
        fail "the built kernel could not be extracted from the disk copy"
    fi
elif ! build/krffs/readfile.krffs "$WORK" /home/test/kernel.selfbuilt \
        "$KEXTRACT" --offset "$PART_OFFSET" --size "$PART_SIZE"; then
    fail "the built kernel could not be extracted from the disk copy"
fi
HOST_SHA="$(sha256_of "$KEXTRACT")"
if [ "$HOST_SHA" != "$GUEST_SHA" ]; then
    fail "the extracted kernel hashes $HOST_SHA and the guest recorded $GUEST_SHA"
fi
if ! build/tools/inspect/elfcheck --kernel --tcc "$KEXTRACT"; then
    fail "the built kernel failed the structural checker"
fi
if ! build/tests/system/runtests --kernel "$KEXTRACT" \
        --initrd "$ARCH_BUILD"/images/test.krffs --timeout 300 --steptmo 60 \
        --log "$DISK.probes.log"; then
    fail "the probe suite did not pass on the machine-built kernel"
fi
if [ "$ROOT_TYPE" = ext2 ]; then
    sh "$REPO/tests/system/probe_roots.sh" "$KEXTRACT" "$DISK.ext2-probes" ext2 \
        || fail "the probes did not pass on the rebuilt kernel's ext2 root"
fi

# The GRUB half: the same bytes through a real loader.
#
# bounded SECONDS COMMAND...: run COMMAND, and kill it and return 124 if it has
# not ended within SECONDS; otherwise return its status. The disk assembly
# runs under it, since every other wait of the run has a limit.
bounded() {
    limit="$1"
    shift
    "$@" &
    bpid=$!
    if ! "$RUNUTIL" gone "$bpid" "$limit"; then
        kill -9 "$bpid" 2>/dev/null || true
        wait "$bpid" 2>/dev/null || true
        return 124
    fi
    wait "$bpid"
}

# The GRUB boot of the guest-built kernel always runs: the disk is assembled
# on the host from the pinned GRUB images.
GRUB_DISK="$DISK.grub"
# The disk's root is the small os-simple ext2 image, since the row tests the
# guest-built kernel booting through GRUB and not what it mounts.
if ! bounded 900 env \
        RACH2_DISK_TABLE=mbr \
        RACH2_DISK_RECOVERY_MB=8 \
        RACH2_DISK_EXT2_MB=16 \
        RACH2_DISK_EXT2_IMAGE="$REPO/$ARCH_BUILD/images/os-simple.ext2" \
        sh tools/image/mkdisk_grub.sh "$KEXTRACT" "$GRUB_DISK"; then
    rm -f "$GRUB_DISK"
    fail "the GRUB disk around the built kernel did not assemble inside its bound"
fi
GLOG="$DISK.grub.log"
rm -f "$GLOG"
"$QEMU" -cpu max -display none -rtc base=utc \
    -drive "file=$GRUB_DISK,format=raw,if=ide,index=0,media=disk" \
    -serial stdio < /dev/null > "$GLOG" 2>&1 &
QPID=$!
grub_ok=1
if "$RUNUTIL" until "$GLOG" "login:" 120; then
    grub_ok=0
fi
cleanup_boot
if [ "$grub_ok" -ne 0 ]; then
    cp "$GLOG" "$DISK.fail.log" 2>/dev/null || true
    rm -f "$GRUB_DISK" "$GLOG"
    fail "the machine-built kernel did not reach a login prompt through GRUB"
fi
rm -f "$GRUB_DISK" "$GLOG"
GRUB_NOTE="and booted through GRUB"

# The build in place: root, in a second short boot of the same disk, runs
# make directly in the staged /src/ish, with no copy and no touch. Staged
# files with zero timestamps fail it, since pdpmake then reads every staged
# source as a file that does not exist.
: > "$STEPS"
step "INPLACERC" "cd /src/ish && make -f guest.mk > /tmp/ip.out 2>&1; echo INPLACE\"\"RC=\$?"
step "IPTAIL" "busybox tail -3 /tmp/ip.out; echo IP\"\"TAIL"
rm -f "$LOG"
boot_steps root "$ROOTPASSWORD"
if ! wait_token "INPLACERC=" 600; then
    fail "the in-place root build never reported"
fi
if ! grep -q "INPLACERC=0" "$LOG"; then
    wait_token "IPTAIL" 60 || true
    fail "make -f guest.mk in the staged /src/ish failed; its last lines are in the kept log"
fi
cleanup_boot

if [ "$ROOT_TYPE" = krffs ]; then
    echo "selfbuild-test: the machine built its kernel twice from clean trees (hash $GUEST_SHA, $SIZEONE bytes), its utilities and its shell, and root rebuilt the staged /src/ish in place; the exact kernel bytes passed the checker and the probe suite $GRUB_NOTE: OK"
else
    echo "selfbuild-test: the machine built its kernel once from a clean tree on the ext2 root (hash $GUEST_SHA, $SIZEONE bytes); the exact kernel bytes passed the checker and the probe suite $GRUB_NOTE: OK"
fi
PASSED=1
echo "selfbuild-test: PASS"
