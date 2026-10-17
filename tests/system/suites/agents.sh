#!/usr/bin/env sh

#
# agents.sh: the agents layer on the GNU system (`tests/run agents`).
#
# Boots the x86-64 kernel with `rach2.root=ext2` on a disk whose one
# partition is a copy of os-real-gnu-agents.ext2, the GNU root with every
# package of the agents layer (vendor/gnu/agents/Readme.md), and runs the
# layer's rows over SSH as the test account, an ordinary user. The machine
# has no route out: QEMU's user network forwards the SSH port to it, and
# 10.0.2.100:443 to the fake API server, tests/system/fakeapi/fakeapi.py, on
# the host, through `fakeapi.py relay`, and carries nothing else it sends.
#
# A row is a file of tests/system/agents/, NAME.sh, one for each package and
# each later test of the layer, so that a new row is a new file. Its header
# has a line `# bound: N`, the seconds the row may take under TCG on the
# testing host, and it defines the function `row`, which
# tests/system/agents_rows.sh runs on the machine in a directory of the
# row's own. A row named after a package is given the version its recipe
# pins. A failed row does not stop the next, so a run reports every row the
# machine cannot do. RACH2_AGENTS_ROWS, a list of row names separated by
# spaces, runs those rows alone, as a mutation proof of one row needs.
#
# The files git tracks under the tree's tests and tools, as this tree has
# them, go to the machine as /tmp/agents/tree.tar, for the rows that run the
# tests of the project's loops (tools/flow/) there.
#
# A row that talks to a vendor's service has a script for the fake server,
# NAME.json beside it, or a header `# script: PATH` relative to
# tests/system/, and a header line `# hosts: NAME...` with the host
# names it reaches. The suite makes a certificate authority, which the
# machine has as /tmp/agents/ca.pem, and a server certificate for every
# row's names, and root gives each name the server's address in /etc/hosts.
# The server plays the row's script while the row runs, and the row fails
# unless the server's verdict passes: every step played and no request the
# script does not expect. A row may write /tmp/agents/results/NAME.tar and
# supply NAME_verify.py beside its script. The host fetches that archive and
# runs the verifier with the archive, server record and pinned version. It
# starts FILE --watch RECORD TIMINGS before the row and sends it SIGTERM
# afterwards; TIMINGS records elapsed host seconds as requests arrive.
# `# package: NAME` selects another package's version; `# verifier: FILE`
# selects another row's verifier. `# script-filter: FILE` runs FILE --script
# SOURCE DESTINATION to prepare a variant of the shared server script.
#
# Usage: agents.sh KERNEL IMAGE DISK PORT APIPORT
#

QEMU=${RACH2_QEMU:-qemu-system-x86_64}

# shellcheck source=tests/system/deadline.sh
. "$(cd "$(dirname "$0")/../../.." && pwd)/tests/system/deadline.sh"
set -u

KERNEL=$1
IMAGE=$2
DISK=$3
PORT=$4
APIPORT=$5
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
RUNUTIL="$REPO/build/tests/system/runutil"
DEADLINE="$REPO/build/tests/system/deadline"
MKMBR="$REPO/build/tools/image/mkmbr"
ROOT_KEY="$REPO/krffs/rach2_demo_ed25519"
TEST_KEY="$REPO/krffs/rach2_test_ed25519"
FAKEAPI="$REPO/tests/system/fakeapi/fakeapi.py"
ROWS_DIR="$REPO/tests/system/agents"
PACKAGES="$REPO/vendor/gnu/agents/packages"
LOG="$DISK.log"
KNOWN="$DISK.known"
QPID=""
SPID=""
TPID=""
SERVER_STATUS=""
work=""

. "$REPO/tools/setup/e2fsprogs_path.sh"

# Stop the machine; safe to call twice, since the pid is cleared before it is
# signalled.
stop_machine() {
    qpid=$QPID
    QPID=""
    if [ -n "$qpid" ]; then
        kill -9 "$qpid" 2>/dev/null || true
        wait "$qpid" 2>/dev/null || true
    fi
}
# Stop the server, which writes its verdict as it ends, and keep its exit
# status in SERVER_STATUS; one that has not ended in 30 seconds is killed.
stop_server() {
    spid=$SPID
    SPID=""
    if [ -n "$spid" ]; then
        kill -TERM "$spid" 2>/dev/null || true
        "$RUNUTIL" gone "$spid" 30 || kill -9 "$spid" 2>/dev/null || true
        wait "$spid" 2>/dev/null
        SERVER_STATUS=$?
    fi
}
# Stop the request-time observer and wait for its last record.
stop_timing() {
    if [ -n "$TPID" ]; then
        kill -TERM "$TPID" 2>/dev/null || true
        wait "$TPID" 2>/dev/null || true
        TPID=""
    fi
}
# Stop the machine and the server, and remove the disk, the known hosts and
# the scratch directory with the authority's key.
cleanup() {
    stop_timing
    stop_machine
    stop_server
    rm -f "$DISK" "$KNOWN"
    [ -z "$work" ] || rm -rf "$work"
}
trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

# fail REASON: print REASON, stop the machine, and exit 1. What the SSH server
# said is in the machine's service log, which a failure reads off the stopped
# machine's disk, from the partition at 1 MiB.
fail() {
    echo "agents-test: FAIL: $1" >&2
    stop_machine
    stop_server
    if [ -f "$DISK" ]; then
        debugfs -R "cat /var/log/services" "$DISK?offset=1048576" > "$DISK.services" 2>/dev/null
        if [ -s "$DISK.services" ]; then
            echo "agents-test: the machine's service log: $DISK.services" >&2
        fi
    fi
    echo "agents-test: the console log: $LOG" >&2
    exit 1
}

[ "${RACH2_ARCH:-}" = x86_64 ] || fail "the agents layer is x86-64 only; run with ARCH=x86_64"
for key in "$ROOT_KEY" "$TEST_KEY"; do
    [ -f "$key" ] || fail "no demo key at $key; run 'sh krffs/gen_demo_key.sh'"
done
case $FAKEAPI in
    *,*) fail "the server's path has a comma, which QEMU's -netdev cannot take: $FAKEAPI" ;;
esac
rm -f "$DISK" "$KNOWN" "$LOG" "$DISK".*.log "$DISK".*.record "$DISK".*.server
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-agents.XXXXXX") || fail "the scratch directory"

# The host names the rows reach, each once, and the authority that signs the
# server's certificate for them.
HOSTS=$(sed -n 's/^# hosts: //p' "$ROWS_DIR"/*.sh | tr ' ' '\n' | sed '/^$/d' | sort -u | tr '\n' ' ' | sed 's/ $//')
[ -n "$HOSTS" ] || HOSTS=fakeapi.test
# shellcheck disable=SC2086 # the names are one word each
python3 -B "$FAKEAPI" ca "$work/ca" $HOSTS > "$work/ca.log" 2>&1 ||
    fail "the certificate authority: $(cat "$work/ca.log")"

# The disk: the volume at LBA 2048, as the machine has its root, copied
# sparse, since the image is 4 GiB with most of it free.
SIZE_MB=$(( $(wc -c < "$IMAGE") / 1048576 ))
dd if=/dev/zero of="$DISK" bs=1048576 count=0 seek=$((SIZE_MB + 2)) 2>/dev/null ||
    fail "the disk file"
dd if="$IMAGE" of="$DISK" bs=1048576 seek=1 conv=notrunc,sparse 2>/dev/null ||
    fail "the volume on the disk"
"$MKMBR" --lba "$DISK" "2048:$((SIZE_MB * 2048)):83" > /dev/null || fail "the disk's table"

# Four GiB, what an agent's session takes with its helpers; the disk on an
# AHCI port; and a user network restricted to the forwarded ports.
"$QEMU" -cpu max -m 4096 -display none -kernel "$KERNEL" \
    -append "rach2.root=ext2" \
    -device ich9-ahci,id=ahci0 \
    -drive "if=none,id=drv0,file=$DISK,format=raw" -device ide-hd,drive=drv0,bus=ahci0.0 \
    -netdev "user,id=n0,restrict=on,hostfwd=tcp::$PORT-:22,guestfwd=tcp:10.0.2.100:443-cmd:python3 -B $FAKEAPI relay $APIPORT" \
    -device rtl8139,netdev=n0 \
    -serial stdio < /dev/null > "$LOG" 2>&1 &
QPID=$!

"$RUNUTIL" until "$LOG" "ext2: root: /dev/disk0p1" "$(scaled 120)" ||
    fail "the machine did not mount the GNU system's volume as its root"
"$RUNUTIL" until "$LOG" "init: sshd: started as pid" "$(scaled 300)" ||
    fail "init did not start the SSH server"

# ssh_as BOUND USER KEY COMMAND: run COMMAND on the machine as USER, by KEY,
# under the deadline tool at BOUND seconds: ConnectTimeout bounds only the
# connection and its key exchange.
ssh_as() {
    "$DEADLINE" "$1" ssh -i "$3" -p "$PORT" -o StrictHostKeyChecking=no \
        -o "UserKnownHostsFile=$KNOWN" -o "ConnectTimeout=$(scaled 30)" -o BatchMode=yes \
        -o IdentitiesOnly=yes -o PreferredAuthentications=publickey "$2@127.0.0.1" "$4"
}

# The server makes its host keys before it listens, which on an emulated
# machine takes a minute or more. The bound is on the time since the first
# try, and each try is held to what is left of it.
start=$("$RUNUTIL" now)
while :; do
    left=$((start + $(scaled 900) - $("$RUNUTIL" now)))
    [ "$left" -gt 0 ] || fail "no SSH answer: init did not bring OpenSSH's server up"
    try=$(scaled 60)
    [ "$try" -le "$left" ] || try=$left
    ssh_as "$try" test "$TEST_KEY" 'echo AGENTS""READY' 2>/dev/null | grep -q AGENTSREADY && break
    # interval: a try of SSH every two seconds until the server answers
    sleep 2
done
echo "agents-test: the test account logs in over SSH: OK"

lines=""
for name in $HOSTS; do
    lines="${lines}10.0.2.100 $name\\n"
done
ssh_as "$(scaled 60)" root "$ROOT_KEY" "printf '$lines' >> /etc/hosts" ||
    fail "the fake server's names in /etc/hosts"
ssh_as "$(scaled 60)" test "$TEST_KEY" 'rm -rf /tmp/agents && mkdir -p /tmp/agents/fixture' ||
    fail "the rows' directory on the machine"
git -C "$REPO" ls-files tests tools | while IFS= read -r f; do
    [ ! -e "$REPO/$f" ] || printf '%s\n' "$f"
done | tar -cf "$work/tree.tar" -C "$REPO" -T - || fail "the tar of the tree's tests and tools"
"$DEADLINE" "$(scaled 120)" scp -P "$PORT" -i "$TEST_KEY" -o StrictHostKeyChecking=no \
    -o "UserKnownHostsFile=$KNOWN" -o BatchMode=yes -o IdentitiesOnly=yes \
    "$REPO/tests/system/agents_rows.sh" "$ROWS_DIR"/*.sh "$work/ca/ca.pem" "$work/tree.tar" \
    test@127.0.0.1:/tmp/agents/ > /dev/null 2>&1 ||
    fail "scp could not put the rows on the machine"
"$DEADLINE" "$(scaled 120)" scp -P "$PORT" -i "$TEST_KEY" -o StrictHostKeyChecking=no \
    -o "UserKnownHostsFile=$KNOWN" -o BatchMode=yes -o IdentitiesOnly=yes \
    "$REPO/tests/system/fakeapi/repo/"* test@127.0.0.1:/tmp/agents/fixture/ \
    > /dev/null 2>&1 || fail "the shared fixture on the machine"
echo "agents-test: the fake server's names, $HOSTS, and its authority are on the machine: OK"

FAILED=""
TOTAL=0
for file in "$ROWS_DIR"/*.sh; do
    name=${file##*/}
    name=${name%.sh}
    case " ${RACH2_AGENTS_ROWS:-$name} " in
        *" $name "*) ;;
        *) continue ;;
    esac
    script=$(sed -n 's/^# script: //p' "$file")
    if [ -n "$script" ]; then
        script="$REPO/tests/system/$script"
        [ -f "$script" ] || fail "$file names no server script at $script"
    else
        script="$ROWS_DIR/$name.json"
    fi
    bound=$(sed -n 's/^# bound: \([0-9][0-9]*\)$/\1/p' "$file")
    [ -n "$bound" ] || fail "$file has no line '# bound: N'"
    package=$(sed -n 's/^# package: //p' "$file")
    package=${package:-$name}
    version=""
    if [ -f "$PACKAGES/$package.sh" ]; then
        version=$(sed -n 's/^VERSION=//p' "$PACKAGES/$package.sh")
    fi
    TOTAL=$((TOTAL + 1))
    filter=$(sed -n 's/^# script-filter: //p' "$file")
    if [ -n "$filter" ]; then
        python3 -B "$ROWS_DIR/$filter" --script "$script" "$work/$name.json" ||
            fail "the server script for $name"
        script="$work/$name.json"
    fi
    if [ -f "$script" ]; then
        python3 -B "$FAKEAPI" serve "$script" "$DISK.$name.record" "$work/ca" "$APIPORT" \
            > /dev/null 2> "$DISK.$name.server" &
        SPID=$!
        "$RUNUTIL" until "$DISK.$name.server" "fakeapi: listening on" "$(scaled 30)" ||
            fail "the server did not listen for $name: $(cat "$DISK.$name.server")"
    fi
    verifier=$(sed -n 's/^# verifier: //p' "$file")
    verifier="$ROWS_DIR/${verifier:-${name}_verify.py}"
    if [ -f "$verifier" ]; then
        python3 -B "$verifier" --watch "$DISK.$name.record" "$DISK.$name.times.log" &
        TPID=$!
    fi
    began=$("$RUNUTIL" now)
    ssh_as "$(scaled "$bound")" test "$TEST_KEY" \
        "/bin/bash /tmp/agents/agents_rows.sh $name $version" > "$DISK.$name.log" 2>&1
    status=$?
    stop_timing
    took="$(($("$RUNUTIL" now) - began)) s"
    problem=""
    if [ "$status" -eq 124 ]; then
        problem="no answer within its bound"
    elif ! grep -q "^AGENTROW OK $name\$" "$DISK.$name.log"; then
        problem=$(grep '^AGENTROW FAIL' "$DISK.$name.log" || tail -n 3 "$DISK.$name.log")
    elif [ "$status" -ne 0 ]; then
        problem="the row printed its OK, but ssh ended with status $status"
    fi
    if [ -f "$script" ]; then
        stop_server
        if [ "$SERVER_STATUS" != 0 ] && [ -z "$problem" ]; then
            problem="the fake server's verdict: $(tail -n 1 "$DISK.$name.server")"
        fi
    fi
    if [ -z "$problem" ] && [ -f "$verifier" ]; then
        if ! "$DEADLINE" "$(scaled 120)" scp -P "$PORT" -i "$TEST_KEY" \
            -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" -o BatchMode=yes \
            -o IdentitiesOnly=yes "test@127.0.0.1:/tmp/agents/results/$name.tar" \
            "$work/$name.tar" >> "$DISK.$name.log" 2>&1; then
            problem="the row's result archive could not be fetched"
        elif ! python3 -B "$verifier" "$work/$name.tar" "$DISK.$name.record" "$version" \
            > "$work/$name.verify.log" 2>&1; then
            problem="the host's verification: $(tail -n 1 "$work/$name.verify.log")"
        else
            cat "$work/$name.verify.log"
            echo "agents-test: $name: request number and elapsed host seconds:"
            cat "$DISK.$name.times.log"
        fi
        [ ! -f "$work/$name.verify.log" ] || cat "$work/$name.verify.log" >> "$DISK.$name.log"
    fi
    if [ -z "$problem" ]; then
        echo "agents-test: $name: OK in $took"
        continue
    fi
    FAILED="$FAILED $name"
    echo "agents-test: $name: FAIL after $took: $problem" >&2
    echo "agents-test: $name: the row's output: $DISK.$name.log" >&2
    if [ -f "$script" ]; then
        echo "agents-test: $name: the server's log: $DISK.$name.server; its record: $DISK.$name.record" >&2
    fi
done
[ "$TOTAL" -gt 0 ] || fail "tests/system/agents has no rows${RACH2_AGENTS_ROWS:+ named $RACH2_AGENTS_ROWS}"
[ -z "$FAILED" ] || fail "$(echo "$FAILED" | wc -w | tr -d ' ') of $TOTAL rows:$FAILED"
rm -f "$DISK".*.log "$DISK".*.record "$DISK".*.server "$DISK.services"
echo "agents-test: PASS"
