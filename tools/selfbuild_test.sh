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
# The run ends with a second, short boot as root that rebuilds the staged
# /src/ish in place, no copy and no touch: that is the demonstration flow,
# and it is what a revert to zero staged timestamps breaks (pdpmake reads a
# zero modification time as a file that does not exist).
#
# Usage: selfbuild_test.sh <kernel> <disk> <test password> <root password>
#

set -e

KERNEL="$1"
DISK="$2"
TESTPASSWORD="$3"
ROOTPASSWORD="$4"

if [ -z "$KERNEL" ] || [ -z "$DISK" ] || [ -z "$TESTPASSWORD" ] ||
   [ -z "$ROOTPASSWORD" ]; then
    echo "usage: selfbuild_test.sh <kernel> <disk> <test password> <root password>" >&2
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
    login_name="$1"
    login_password="$2"
    rm -f "$FEED"
    mkfifo "$FEED"
    qemu-system-i386 -cpu max -display none -kernel "$KERNEL" \
        -append "rach2.fsdisk" -rtc base=utc \
        -drive "file=$WORK,format=raw,if=ide,index=0,media=disk" \
        -serial stdio < "$FEED" > "$LOG" 2>&1 &
    QPID=$!
    (
        sleep 14
        printf '%s\n' "$login_name"
        sleep 4
        printf '%s\n' "$login_password"
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
    fail "the disk has $FREE_BYTES free bytes and two kernel builds need $NEED_BYTES"
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
step "SMOKEPING" "./ping 127.0.0.1 > /dev/null 2>&1 && echo SMOKE\"\"PING"
# Port 8080 and not 80: these steps run as an ordinary user, and a port below
# 1024 is root's, so the self-built server is asked for one this user may have.
step "SMOKEWEB" "./httpd 8080 > /dev/null 2>&1 & sleep 2; printf 'GET / HTTP/1.0\r\n\r\n' | ./nc 127.0.0.1 8080 | busybox grep -q HTTP/ && echo SMOKE\"\"WEB; kill %% 2>/dev/null"
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
if ! wait_token "SYNCDONE" 240; then
    fail "the disk was never synced after the builds"
fi
GUEST_SHA="$(grep -a -o 'HASHONE=[0-9a-f]*' "$LOG" | grep -a -oE '[0-9a-f]{64}' | tail -1)"
if [ -z "$GUEST_SHA" ]; then
    fail "the guest never reported the built kernel's hash"
fi
cleanup_boot

# The proof of the bytes. The machine is stopped; the exact kernel it built is
# extracted from the synced disk copy, its hash must equal the one the guest
# recorded, the structural checker must accept the tcc layout, and then those
# bytes boot the full probe suite. Nothing here trusts the build; everything
# is read back out and judged.
sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

if ! build/krffs/readfile.krffs "$WORK" /home/test/kernel.selfbuilt \
        "$KEXTRACT" --offset "$PART_OFFSET" --size "$PART_SIZE"; then
    fail "the built kernel could not be extracted from the disk copy"
fi
HOST_SHA="$(sha256_of "$KEXTRACT")"
if [ "$HOST_SHA" != "$GUEST_SHA" ]; then
    fail "the extracted kernel hashes $HOST_SHA and the guest recorded $GUEST_SHA"
fi
if ! python3 tools/check_kernel.py --tcc "$KEXTRACT"; then
    fail "the built kernel failed the structural checker"
fi
if ! python3 tools/runtests.py --kernel "$KEXTRACT" \
        --initrd build/images/test.krffs --timeout 300 --steptmo 60; then
    fail "the probe suite did not pass on the machine-built kernel"
fi

# The GRUB half: the same bytes through a real loader. It needs Docker to
# assemble the disk, and where Docker is absent the skip is named rather than
# silent; the QEMU proof above ran either way. Every Docker operation runs
# under a bound, because a stalled daemon or package mirror would otherwise
# hang a run whose every other wait has one.
bounded() {
    limit="$1"
    shift
    "$@" &
    bpid=$!
    bwaited=0
    while [ "$bwaited" -lt "$limit" ] && kill -0 "$bpid" 2>/dev/null; do
        sleep 1
        bwaited=$((bwaited + 1))
    done
    if kill -0 "$bpid" 2>/dev/null; then
        kill -9 "$bpid" 2>/dev/null || true
        wait "$bpid" 2>/dev/null || true
        return 124
    fi
    wait "$bpid"
}

docker_ready=1
docker_skip="the Docker command is not installed"
if command -v docker >/dev/null 2>&1; then
    if bounded 30 docker info >/dev/null 2>&1; then
        docker_ready=0
    else
        docker_skip="the Docker daemon did not answer"
    fi
fi
if [ "$docker_ready" -eq 0 ]; then
    GRUB_DISK="$DISK.grub"
    if ! bounded 900 sh tools/mkdisk_grub.sh "$KEXTRACT" "$GRUB_DISK"; then
        rm -f "$GRUB_DISK"
        fail "the GRUB disk around the built kernel did not assemble inside its bound"
    fi
    GLOG="$DISK.grub.log"
    rm -f "$GLOG"
    qemu-system-i386 -cpu max -display none -rtc base=utc \
        -drive "file=$GRUB_DISK,format=raw,if=ide,index=0,media=disk" \
        -serial stdio < /dev/null > "$GLOG" 2>&1 &
    QPID=$!
    grub_ok=1
    waited=0
    while [ "$waited" -lt 120 ]; do
        if grep -aq "login:" "$GLOG" 2>/dev/null; then
            grub_ok=0
            break
        fi
        sleep 1
        waited=$((waited + 1))
    done
    cleanup_boot
    if [ "$grub_ok" -ne 0 ]; then
        cp "$GLOG" "$DISK.fail.log" 2>/dev/null || true
        rm -f "$GRUB_DISK" "$GLOG"
        fail "the machine-built kernel did not reach a login prompt through GRUB"
    fi
    rm -f "$GRUB_DISK" "$GLOG"
    GRUB_NOTE="and booted through GRUB"
else
    echo "selfbuild-test: the GRUB boot needs Docker to assemble the disk; skipped: $docker_skip"
    GRUB_NOTE="(GRUB skipped: $docker_skip)"
fi

# The in-place regression: root, in a second short boot of the same disk,
# runs make directly in the staged /src/ish, no copy and no touch. This is
# the demonstration flow, and it is what a revert to zero staged timestamps
# breaks (pdpmake then reads every staged source as a file that does not
# exist).
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

echo "selfbuild-test: the machine built its kernel twice from clean trees (hash $GUEST_SHA, $SIZEONE bytes), its utilities and its shell, and root rebuilt the staged /src/ish in place; the exact kernel bytes passed the checker and the probe suite $GRUB_NOTE: OK"
echo "selfbuild-test: PASS"
