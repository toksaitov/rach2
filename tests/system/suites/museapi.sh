#!/usr/bin/env sh

#
# museapi.sh: the Responses protocol of Meta's Model API in the fake API
# server, proved against Muse Code on the host (`tests/run museapi`).
#
# The suite takes Muse Code's Linux binary at the pinned version from
# tests/system/fakeapi/fetch_muse.sh, which checks it against the digest of
# Meta's release manifest, and runs it in a fresh home, on a clone of
# tests/system/fakeapi/repo, against tests/system/fakeapi/fakeapi.py playing
# tests/system/fakeapi/muse.json. Muse runs in a network namespace of its own
# that has only a loopback interface, so neither Muse nor the commands it runs
# can reach another host. It reaches the server by --base-url, trusts the
# test certificate authority by the setting endpoint_transport.ca_bundle, and
# signs in with a dummy key in META_API_KEY. --yolo turns its sandbox and
# approvals off; its settings file turns off its telemetry and its observer
# agents, whose model calls would run beside the main agent's.
#
# The script answers Muse's model catalog, `GET /muse-code/models`, with the
# Model API's list of models, then has Muse read notes.txt, run check.sh,
# edit greeting.txt and commit with git, each a function of its namespace
# tool `muse`. The server checks each tool result before it answers. The host
# then judges the server's verdict, the record of each request, the file and
# the commit. A second run, whose notes.txt says something else, must have
# the server refuse that tool result and leave the repository as it was;
# Muse retries a refused request for minutes, so the suite stops it once the
# server has refused.
#
# Usage: museapi.sh PREFIX
#
# The logs and the records are PREFIX.NAME.*, kept for the reader.
#

# shellcheck source=tests/system/deadline.sh
. "$(cd "$(dirname "$0")/../../.." && pwd)/tests/system/deadline.sh"
set -u

PREFIX=$1
case $PREFIX in
    /*) ;;
    *) PREFIX="$PWD/$PREFIX" ;;
esac
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
SUITE="$REPO/tests/system/suites/museapi.sh"
RUNUTIL="$REPO/build/tests/system/runutil"
DEADLINE="$REPO/build/tests/system/deadline"
FAKEAPI="$REPO/tests/system/fakeapi/fakeapi.py"
SCRIPT="$REPO/tests/system/fakeapi/muse.json"
FIXTURE="$REPO/tests/system/fakeapi/repo"
MUSE_VERSION=1.4.10-R6427.1
MODEL=muse-spark-1.2
KEY=meta-fakeapi-dummy
PROMPT='Read notes.txt and do what it says. Check the change with check.sh, then commit it.'
# The observer agents' capabilities, each turned off in the settings file.
OBSERVERS='goal-reminder memory scope-reminder skill-reminder todo-reminder verify-reminder'

# Fetch the program, then run the rest of this file again in a network
# namespace with a loopback interface and nothing else, as the same user.
if [ "${RACH2_MUSEAPI_OFFLINE:-}" != 1 ]; then
    MUSE=$(sh "$REPO/tests/system/fakeapi/fetch_muse.sh") || {
        echo "museapi-test: FAIL: Muse Code $MUSE_VERSION could not be fetched" >&2
        exit 1
    }
    # shellcheck disable=SC2016 # the inner shell expands its own arguments
    exec unshare --user --map-root-user --net sh -c \
        'ip link set lo up && exec unshare --user --map-user="$1" --map-group="$2" \
            env RACH2_MUSEAPI_OFFLINE=1 RACH2_MUSE="$3" sh "$4" "$5"' \
        sh "$(id -u)" "$(id -g)" "$MUSE" "$SUITE" "$PREFIX"
fi
MUSE=$RACH2_MUSE

SPID=""
MPID=""
SERVER_STATUS=""
work=""

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
# Stop Muse, which deadline passes on to its process group, and keep its exit
# status in MUSE_STATUS.
stop_muse() {
    mpid=$MPID
    MPID=""
    if [ -n "$mpid" ]; then
        kill -TERM "$mpid" 2>/dev/null || true
        wait "$mpid" 2>/dev/null
        MUSE_STATUS=$?
    fi
}
# Stop Muse and the server and remove the scratch directory, with the homes,
# the clones and the authority's key.
cleanup() {
    stop_muse
    stop_server
    [ -z "$work" ] || rm -rf "$work"
}
trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

# fail REASON: print REASON with the logs to read, and exit 1.
fail() {
    echo "museapi-test: FAIL: $1" >&2
    stop_muse
    stop_server
    echo "museapi-test: the logs and records: $PREFIX.*" >&2
    exit 1
}

mkdir -p "$(dirname "$PREFIX")" || fail "the directory of $PREFIX"
rm -f "$PREFIX".*
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-museapi.XXXXXX") || fail "the scratch directory"

OUT=$(python3 -c '
import socket
try:
    socket.create_connection(("1.1.1.1", 443), timeout=10)
    print("connected")
except OSError as error:
    print(error)
' 2>&1)
case "$OUT" in
    *"unreachable"*) ;;
    *) fail "the namespace reached an outside address: $OUT" ;;
esac
echo "museapi-test: the network has only a loopback interface: OK"

OUT=$("$DEADLINE" "$(scaled 60)" "$MUSE" --version 2>&1)
[ "$OUT" = "Muse Code ${MUSE_VERSION%%-*} ($MUSE_VERSION)" ] ||
    fail "$MUSE is not Muse Code $MUSE_VERSION: $OUT"
echo "museapi-test: $MUSE is Muse Code $MUSE_VERSION: OK"

python3 -B "$FAKEAPI" ca "$work/ca" localhost > "$work/ca.log" 2>&1 ||
    fail "the certificate authority: $(cat "$work/ca.log")"

# start NAME NOTES: clone the fixture with NOTES as its notes.txt into
# $work/NAME/repo, start the server on the script, and start `muse exec`
# there in the background, in the fresh home $work/NAME/home, its environment
# only what it is given. MPID is Muse's deadline, which play and refuse wait
# for. The record is PREFIX.NAME.record, and the logs PREFIX.NAME.server.log
# and PREFIX.NAME.muse.log.
start() {
    name=$1
    home="$work/$name/home"
    tree="$work/$name/repo"
    log="$PREFIX.$name.server.log"
    mkdir -p "$home/.config/muse" "$tree" || fail "the home and the clone of $name"
    printf '[user]\n\tname = Muse Test\n\temail = muse@fakeapi.test\n' > "$home/.gitconfig"
    cp "$FIXTURE/check.sh" "$FIXTURE/greeting.txt" "$tree/" || fail "the fixture"
    printf '%s\n' "$2" > "$tree/notes.txt"
    (cd "$tree" && HOME="$home" && export HOME && git init -q . && git add . &&
        git commit -q -m 'The fixture') || fail "the fixture's commit"
    {
        printf '{\n  "schema_version": 1,\n  "telemetry": {"enabled": false},\n'
        printf '  "runtime_capabilities": {'
        sep=''
        for observer in $OBSERVERS; do
            printf '%s\n    "plugin:tbh-reminders:reminder:%s": {"enabled": false}' \
                "$sep" "$observer"
            sep=','
        done
        printf '\n  }\n}\n'
    } > "$home/.config/muse/settings.json"

    python3 -B "$FAKEAPI" serve "$SCRIPT" "$PREFIX.$name.record" "$work/ca" \
        > /dev/null 2> "$log" &
    SPID=$!
    "$RUNUTIL" until "$log" "fakeapi: listening on" "$(scaled 30)" ||
        fail "the server did not listen: $(cat "$log")"
    port=$(sed -n 's/^fakeapi: listening on 127\.0\.0\.1:\([0-9]*\)$/\1/p' "$log")
    (cd "$tree" && exec env -i PATH="$PATH" HOME="$home" TMPDIR="$work" \
        LANG=C.UTF-8 TERM=dumb META_API_KEY="$KEY" \
        "$DEADLINE" "$(scaled 300)" "$MUSE" exec --yolo --model "$MODEL" \
        --max-model-steps 8 --base-url "https://localhost:$port/v1" \
        -c "endpoint_transport.ca_bundle=\"$work/ca/ca.pem\"" "$PROMPT" \
        < /dev/null > "$PREFIX.$name.muse.log" 2>&1) &
    MPID=$!
}

# play NAME NOTES: start NAME, wait for Muse to end, and stop the server.
# Muse's exit status is MUSE_STATUS, and the server's SERVER_STATUS.
play() {
    start "$1" "$2"
    wait "$MPID"
    MUSE_STATUS=$?
    MPID=""
    stop_server
}

# refuse NAME NOTES: start NAME, wait until the server has refused a request,
# and stop Muse and the server; fail when no refusal comes in 120 seconds.
refuse() {
    start "$1" "$2"
    "$RUNUTIL" until "$log" '^fakeapi: [0-9]*: .*: unexpected: ' "$(scaled 120)" ||
        fail "the server did not refuse a wrong tool result: $(tail -n 1 "$log")"
    stop_muse
    stop_server
}

# summary NAME: each request of PREFIX.NAME.record on a line, with its step;
# a request without the dummy key or Muse's user agent says so.
summary() {
    python3 - "$PREFIX.$1.record" "$KEY" "${MUSE_VERSION%%-*}" <<'EOF'
import json
import sys

record, key, version = sys.argv[1:]
for line in open(record, encoding="utf-8"):
    entry = json.loads(line)
    n = entry["n"]
    if "method" in entry:
        headers = {k.lower(): v for k, v in entry["headers"]}
        fields = [n, entry["method"], entry["path"], "step", entry.get("step", "unexpected")]
        if headers.get("authorization") != "Bearer " + key:
            fields.append("without the key")
        if not headers.get("user-agent", "").startswith(f"muse-build/{version} "):
            fields.append("from " + headers.get("user-agent", "nobody"))
        print(*fields)
    else:
        kind = [k for k in entry if k != "n"][0]
        print(n, kind, entry[kind])
EOF
}

play exchange "$(cat "$FIXTURE/notes.txt")"
[ "$MUSE_STATUS" = 0 ] ||
    fail "muse exec ended with status $MUSE_STATUS: $(tail -n 5 "$PREFIX.exchange.muse.log")"
if [ "$SERVER_STATUS" != 0 ] ||
    ! grep -qx 'fakeapi: result: pass: 6 steps played by 6 requests' "$PREFIX.exchange.server.log"; then
    fail "the server's verdict, status $SERVER_STATUS: $(tail -n 1 "$PREFIX.exchange.server.log")"
fi
echo "museapi-test: Muse played the script, each tool result as it expects: OK"

OUT=$(summary exchange)
EXPECTED='1 GET /muse-code/models step 1
2 POST /v1/responses step 2
3 POST /v1/responses step 3
4 POST /v1/responses step 4
5 POST /v1/responses step 5
6 POST /v1/responses step 6'
[ "$OUT" = "$EXPECTED" ] || fail "the record is not the exchange the script plays:
$OUT"
echo "museapi-test: the model catalog and five turns over HTTPS, with the dummy key: OK"

tree="$work/exchange/repo"
[ "$(cat "$tree/greeting.txt")" = 'hello, rach2' ] ||
    fail "greeting.txt says $(cat "$tree/greeting.txt")"
OUT=$(git -C "$tree" log --format='%s|%an <%ae>' 2>&1)
[ "$OUT" = "$(printf 'Greet Rach2|Muse Test <muse@fakeapi.test>\nThe fixture|Muse Test <muse@fakeapi.test>')" ] ||
    fail "the history is not the fixture and Muse's commit: $OUT"
OUT=$(git -C "$tree" diff --name-only HEAD~1 HEAD 2>&1)
[ "$OUT" = greeting.txt ] || fail "the commit changes more than greeting.txt: $OUT"
OUT=$(git -C "$tree" status --porcelain 2>&1)
[ -z "$OUT" ] || fail "the work tree is not clean after the commit: $OUT"
echo "museapi-test: greeting.txt is changed, and the change is the one new commit: OK"

refuse wrong 'Change the greeting in greeting.txt to: hello, there'
if [ "$SERVER_STATUS" != 1 ] ||
    ! grep -q '^fakeapi: result: fail: [0-9]* failed, the first request 3: POST /v1/responses: step 3: the output of call_read with ' \
        "$PREFIX.wrong.server.log"; then
    fail "the server did not refuse a wrong tool result: status $SERVER_STATUS: $(tail -n 1 "$PREFIX.wrong.server.log")"
fi
tree="$work/wrong/repo"
[ "$(cat "$tree/greeting.txt")" = 'hello, world' ] ||
    fail "after the refusal greeting.txt says $(cat "$tree/greeting.txt")"
OUT=$(git -C "$tree" log --format=%s 2>&1)
[ "$OUT" = 'The fixture' ] || fail "after the refusal the history is $OUT"
echo "museapi-test: a wrong tool result is refused, and the repository is left as it was: OK"
echo "museapi-test: PASS"
