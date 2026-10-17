#!/usr/bin/env sh

#
# fakeapi.sh: the fake API server and its test certificate authority, proved
# from the GNU system with curl (`tests/run fakeapi`).
#
# The agents' suites play a whole exchange with a vendor's service offline,
# against tests/system/fakeapi/fakeapi.py on the host. This suite makes a
# certificate authority and a certificate for fakeapi.test, starts the server
# on the script tests/system/fakeapi/fakeapi-test.json, and boots the GNU
# system on a user network with restrict=on: the guest's route out is closed,
# and it reaches only 10.0.2.100:443, which QEMU forwards to the server
# through `fakeapi.py relay`, while the host reaches the guest's SSH port.
# Root gives the name to /etc/hosts and the authority's certificate to a
# file, and the test account then finds no route to an outside address, has
# curl refuse the server against the system's own store, plays the script
# with curl and, for the WebSocket, with tests/system/fakeapi/wsclient.py,
# and sends a request the script does not expect between two it does. The
# server must refuse that request, play the last step after it, and end with
# a failing verdict that names it; the record must hold each request with the
# step it played and the WebSocket's messages.
#
# Usage: fakeapi.sh KERNEL IMAGE DISK PORT
#

# The build tree and the emulator of the architecture
# tests/system/suites.mk names.
ARCH_BUILD=${ARCH_BUILD:-build}
QEMU=${RACH2_QEMU:-qemu-system-x86_64}

# shellcheck source=tests/system/deadline.sh
. "$(cd "$(dirname "$0")/../../.." && pwd)/tests/system/deadline.sh"
set -u

KERNEL=$1
IMAGE=$2
DISK=$3
PORT=$4
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
RUNUTIL="$REPO/build/tests/system/runutil"
DEADLINE="$REPO/build/tests/system/deadline"
MKMBR="$REPO/build/tools/image/mkmbr"
ROOT_KEY="$REPO/krffs/rach2_demo_ed25519"
TEST_KEY="$REPO/krffs/rach2_test_ed25519"
FAKEAPI="$REPO/tests/system/fakeapi/fakeapi.py"
SCRIPT="$REPO/tests/system/fakeapi/fakeapi-test.json"
LOG="$DISK.log"
KNOWN="$DISK.known"
SERVERLOG="$DISK.server.log"
RECORD="$DISK.record"
QPID=""
SPID=""
SERVER_STATUS=""
work=""

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
# Stop the machine and the server, and remove the disk, the known hosts and
# the scratch directory with the authority's key.
cleanup() {
    stop_machine
    stop_server
    rm -f "$DISK" "$KNOWN"
    [ -z "$work" ] || rm -rf "$work"
}
trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

# fail REASON: print REASON with the logs to read, and exit 1.
fail() {
    echo "fakeapi-test: FAIL: $1" >&2
    stop_machine
    stop_server
    echo "fakeapi-test: the console log: $LOG" >&2
    echo "fakeapi-test: the server's log: $SERVERLOG; its record: $RECORD" >&2
    exit 1
}

for key in "$ROOT_KEY" "$TEST_KEY"; do
    [ -f "$key" ] || fail "no demo key at $key; run 'sh krffs/gen_demo_key.sh'"
done
case $FAKEAPI in
    *,*) fail "the server's path has a comma, which QEMU's -netdev cannot take: $FAKEAPI" ;;
esac
rm -f "$DISK" "$KNOWN" "$LOG" "$SERVERLOG" "$RECORD"
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-fakeapi.XXXXXX") || fail "the scratch directory"

python3 -B "$FAKEAPI" ca "$work/ca" fakeapi.test 10.0.2.100 > "$work/ca.log" 2>&1 ||
    fail "the certificate authority: $(cat "$work/ca.log")"
python3 -B "$FAKEAPI" serve "$SCRIPT" "$RECORD" "$work/ca" > /dev/null 2> "$SERVERLOG" &
SPID=$!
"$RUNUTIL" until "$SERVERLOG" "fakeapi: listening on" "$(scaled 30)" ||
    fail "the server did not listen: $(cat "$SERVERLOG")"
APIPORT=$(sed -n 's/^fakeapi: listening on 127\.0\.0\.1:\([0-9]*\)$/\1/p' "$SERVERLOG")
echo "fakeapi-test: the server listens on 127.0.0.1:$APIPORT, with a certificate of a new authority: OK"

# The disk: the volume at LBA 2048, as the machine has its root, copied
# sparse, since the image is 4 GiB with most of it free.
SIZE_MB=$(( $(wc -c < "$IMAGE") / 1048576 ))
dd if=/dev/zero of="$DISK" bs=1048576 count=0 seek=$((SIZE_MB + 2)) 2>/dev/null ||
    fail "the disk file"
dd if="$IMAGE" of="$DISK" bs=1048576 seek=1 conv=notrunc,sparse 2>/dev/null ||
    fail "the volume on the disk"
"$MKMBR" --lba "$DISK" "2048:$((SIZE_MB * 2048)):83" > /dev/null || fail "the disk's table"

"$QEMU" -cpu max -m 1024 -display none -kernel "$KERNEL" \
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
# under the deadline tool at BOUND seconds.
ssh_as() {
    "$DEADLINE" "$1" ssh -i "$3" -p "$PORT" -o StrictHostKeyChecking=no \
        -o "UserKnownHostsFile=$KNOWN" -o "ConnectTimeout=$(scaled 30)" -o BatchMode=yes \
        -o IdentitiesOnly=yes -o PreferredAuthentications=publickey "$2@127.0.0.1" "$4"
}
# guest COMMAND: run COMMAND as the test account, an ordinary user, as an
# agent runs, with its standard error.
guest() {
    ssh_as "$(scaled 120)" test "$TEST_KEY" "$1" 2>&1
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
    ssh_as "$try" root "$ROOT_KEY" 'echo GNU""READY' 2>/dev/null | grep -q GNUREADY && break
    # interval: a try of SSH every two seconds until the server answers
    sleep 2
done
echo "fakeapi-test: the machine answers SSH on a restricted network: OK"

ssh_as "$(scaled 120)" root "$ROOT_KEY" \
    'mkdir -p -m 755 /tmp/fakeapi && echo "10.0.2.100 fakeapi.test" >> /etc/hosts' ||
    fail "the server's name in /etc/hosts"
"$DEADLINE" "$(scaled 120)" scp -P "$PORT" -i "$ROOT_KEY" -o StrictHostKeyChecking=no \
    -o "UserKnownHostsFile=$KNOWN" -o BatchMode=yes "$work/ca/ca.pem" \
    "$REPO/tests/system/fakeapi/wsclient.py" root@127.0.0.1:/tmp/fakeapi/ > /dev/null 2>&1 ||
    fail "scp could not put the authority's certificate and the client on the machine"
ssh_as "$(scaled 120)" root "$ROOT_KEY" 'chmod 644 /tmp/fakeapi/*' ||
    fail "the modes of the authority's certificate and the client"

CURL="curl -sS --cacert /tmp/fakeapi/ca.pem"

OUT=$(guest 'curl -sS --connect-timeout 20 -o /dev/null http://1.1.1.1/; echo "rc=$?"')
case "$OUT" in
    *rc=0) fail "the machine reached an outside address: $OUT" ;;
    *rc=*) ;;
    *) fail "curl to an outside address did not end: $OUT" ;;
esac
echo "fakeapi-test: the machine has no route to an outside address: OK"

OUT=$(guest 'curl -sS https://fakeapi.test/v1/hello; echo "rc=$?"')
case "$OUT" in
    *rc=60) ;;
    *) fail "curl did not refuse the server's certificate against the system's store: $OUT" ;;
esac
[ ! -s "$RECORD" ] || fail "a refused connection was recorded as a request: $(cat "$RECORD")"
echo "fakeapi-test: curl refuses the server against the system's own store: OK"

OUT=$(guest "$CURL -w ' %{ssl_verify_result} %{http_version}' https://fakeapi.test/v1/hello")
[ "$OUT" = '{"hello": "rach2"} 0 1.1' ] ||
    fail "a GET verified against the test authority: $OUT"
echo "fakeapi-test: a GET over TLS verified against the test authority: OK"

OUT=$(guest "$CURL -w ' %{http_code}' -H 'Content-Type: application/json' -d '{\"say\":\"ping\",\"from\":\"rach2\"}' https://fakeapi.test/v1/echo")
[ "$OUT" = "$(printf 'pong\n 201')" ] || fail "a POST whose JSON the step matches: $OUT"
echo "fakeapi-test: a POST whose JSON body the step matches: OK"

OUT=$(guest "$CURL -N 'https://fakeapi.test/v1/events?stream=1'")
[ "$OUT" = "$(printf 'event: count\ndata: {"n":1}\n\nevent: count\nid: 2\ndata: {"n":2}\n\ndata: two\ndata: lines')" ] ||
    fail "server-sent events: $OUT"
echo "fakeapi-test: server-sent events in a chunked body: OK"

PAD=$(printf '%0200d' 0 | tr 0 x)
OUT=$(guest "python3 /tmp/fakeapi/wsclient.py fakeapi.test /v1/socket /tmp/fakeapi/ca.pem ping '{\"n\":2,\"pad\":\"$PAD\"}'")
[ "$OUT" = "$(printf 'pong keepalive\npong\n{"n": 3}\nclose 1000')" ] ||
    fail "a WebSocket exchange: $OUT"
echo "fakeapi-test: a WebSocket exchange, with a ping and a 16-bit length: OK"

OUT=$(guest "$CURL -w ' %{http_code}' https://fakeapi.test/v1/nothing")
case "$OUT" in
    *'"unexpected_request"'*'the path is /v1/nothing, not /v1/last'*' 400') ;;
    *) fail "the server answered a request its script does not expect: $OUT" ;;
esac
OUT=$(guest "$CURL https://fakeapi.test/v1/last")
[ "$OUT" = last ] || fail "the step after an unexpected request: $OUT"
echo "fakeapi-test: a request the script does not expect is refused, and the step waits: OK"

stop_server
if [ "$SERVER_STATUS" != 1 ] ||
    ! grep -q '^fakeapi: result: fail: 1 failed, the first request 5: GET /v1/nothing: ' "$SERVERLOG"; then
    fail "the server's verdict, status $SERVER_STATUS, did not fail the run for the unexpected request: $(tail -n 1 "$SERVERLOG")"
fi
OUT=$(python3 - "$RECORD" <<'EOF'
import json
import sys

for line in open(sys.argv[1], encoding="utf-8"):
    entry = json.loads(line)
    if "method" in entry:
        agent = next((v for k, v in entry["headers"] if k == "User-Agent"), "")
        fields = [entry["n"], entry["method"], entry["path"], entry.get("step", "unexpected"),
                  agent.split("/")[0], entry["body"][:60]]
        print(" ".join(str(f) for f in fields if f != ""))
    else:
        kind, value = [(k, v) for k, v in entry.items() if k != "n"][0]
        value = value if len(str(value)) < 20 else f"{len(value)} characters"
        print(entry["n"], kind, value)
EOF
)
EXPECTED='1 GET /v1/hello 1 curl
2 POST /v1/echo 2 curl {"say":"ping","from":"rach2"}
3 GET /v1/events 3 curl
4 GET /v1/socket 4
4 received ping
4 sent pong
4 received 216 characters
4 sent {"n": 3}
4 sent_close 1000
4 received_close 1000
5 GET /v1/nothing unexpected curl
6 GET /v1/last 5 curl'
[ "$OUT" = "$EXPECTED" ] || fail "the record is not the exchange the guest made:
$OUT"
echo "fakeapi-test: the server's verdict fails the run for the request, and the record has each request and message: OK"
echo "fakeapi-test: PASS"
