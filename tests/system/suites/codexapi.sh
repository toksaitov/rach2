#!/usr/bin/env sh

#
# codexapi.sh: OpenAI's Responses protocol in the fake API server, proved
# against Codex on the host (`tests/run codexapi`).
#
# The suite runs the host's Codex, which must be the pinned version, in a
# fresh home, on a clone of tests/system/fakeapi/repo, against
# tests/system/fakeapi/fakeapi.py playing tests/system/fakeapi/codex.json.
# It runs in a network namespace of its own that has only a loopback
# interface, so neither Codex nor the commands it runs can reach another
# host. Codex reaches the server through a model provider in its
# configuration whose base_url is the server, trusts the test certificate
# authority by CODEX_CA_CERTIFICATE, and signs in with a dummy key from the
# variable the provider's env_key names; its sandbox and approvals are off.
#
# The script has Codex read notes.txt and run check.sh over a WebSocket,
# then refuses the WebSocket Codex opens again with 426, so the rest goes
# over HTTPS: an apply_patch edit of greeting.txt, check.sh and a commit
# with git. The server checks each tool result before it answers. The host
# then judges the server's verdict, the record of each request and
# WebSocket message, the file and the commit. A second run, whose notes.txt
# says something else, must have the server refuse that tool result and
# leave the repository as it was.
#
# Usage: codexapi.sh PREFIX
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
SUITE="$REPO/tests/system/suites/codexapi.sh"
RUNUTIL="$REPO/build/tests/system/runutil"
DEADLINE="$REPO/build/tests/system/deadline"
FAKEAPI="$REPO/tests/system/fakeapi/fakeapi.py"
SCRIPT="$REPO/tests/system/fakeapi/codex.json"
FIXTURE="$REPO/tests/system/fakeapi/repo"
CODEX=${RACH2_CODEX:-codex}
CODEX_VERSION=0.162.1
KEY=sk-fakeapi-dummy
PROMPT='Read notes.txt and do what it says. Check the change with check.sh, then commit it.'

# Run the rest of this file again in a network namespace with a loopback
# interface and nothing else, as the same user.
if [ "${RACH2_CODEXAPI_OFFLINE:-}" != 1 ]; then
    # shellcheck disable=SC2016 # the inner shell expands its own arguments
    exec unshare --user --map-root-user --net sh -c \
        'ip link set lo up && exec unshare --user --map-user="$1" --map-group="$2" \
            env RACH2_CODEXAPI_OFFLINE=1 sh "$3" "$4"' \
        sh "$(id -u)" "$(id -g)" "$SUITE" "$PREFIX"
fi

SPID=""
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
# Stop the server and remove the scratch directory, with the homes, the
# clones and the authority's key.
cleanup() {
    stop_server
    [ -z "$work" ] || rm -rf "$work"
}
trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

# fail REASON: print REASON with the logs to read, and exit 1.
fail() {
    echo "codexapi-test: FAIL: $1" >&2
    stop_server
    echo "codexapi-test: the logs and records: $PREFIX.*" >&2
    exit 1
}

mkdir -p "$(dirname "$PREFIX")" || fail "the directory of $PREFIX"
rm -f "$PREFIX".*
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-codexapi.XXXXXX") || fail "the scratch directory"

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
echo "codexapi-test: the network has only a loopback interface: OK"

OUT=$("$DEADLINE" "$(scaled 60)" "$CODEX" --version 2>&1)
[ "$OUT" = "codex-cli $CODEX_VERSION" ] ||
    fail "$CODEX is not Codex $CODEX_VERSION: $OUT"
echo "codexapi-test: $CODEX is Codex $CODEX_VERSION: OK"

python3 -B "$FAKEAPI" ca "$work/ca" localhost > "$work/ca.log" 2>&1 ||
    fail "the certificate authority: $(cat "$work/ca.log")"

# play NAME NOTES: clone the fixture with NOTES as its notes.txt into
# $work/NAME/repo, start the server on the script, and run `codex exec` there
# in the fresh home $work/NAME/home, its environment only what it is given.
# Codex's exit status is CODEX_STATUS, and the server's SERVER_STATUS once it
# is stopped. The record is PREFIX.NAME.record, and the logs
# PREFIX.NAME.server.log and PREFIX.NAME.codex.log.
play() {
    name=$1
    home="$work/$name/home"
    tree="$work/$name/repo"
    log="$PREFIX.$name.server.log"
    mkdir -p "$home/.codex" "$tree" || fail "the home and the clone of $name"
    printf '[user]\n\tname = Codex Test\n\temail = codex@fakeapi.test\n' > "$home/.gitconfig"
    cp "$FIXTURE/check.sh" "$FIXTURE/greeting.txt" "$tree/" || fail "the fixture"
    printf '%s\n' "$2" > "$tree/notes.txt"
    (cd "$tree" && HOME="$home" && export HOME && git init -q . && git add . &&
        git commit -q -m 'The fixture') || fail "the fixture's commit"

    python3 -B "$FAKEAPI" serve "$SCRIPT" "$PREFIX.$name.record" "$work/ca" \
        > /dev/null 2> "$log" &
    SPID=$!
    "$RUNUTIL" until "$log" "fakeapi: listening on" "$(scaled 30)" ||
        fail "the server did not listen: $(cat "$log")"
    port=$(sed -n 's/^fakeapi: listening on 127\.0\.0\.1:\([0-9]*\)$/\1/p' "$log")
    cat > "$home/.codex/config.toml" <<EOF
model = "gpt-5.5"
model_provider = "fakeapi"
approval_policy = "never"
sandbox_mode = "danger-full-access"
check_for_update_on_startup = false

[analytics]
enabled = false

[model_providers.fakeapi]
name = "fakeapi"
base_url = "https://localhost:$port/v1"
env_key = "FAKEAPI_KEY"
wire_api = "responses"
supports_websockets = true
EOF
    (cd "$tree" && env -i PATH="$PATH" HOME="$home" CODEX_HOME="$home/.codex" \
        TMPDIR="$work" LANG=C.UTF-8 TERM=dumb FAKEAPI_KEY="$KEY" \
        CODEX_CA_CERTIFICATE="$work/ca/ca.pem" \
        "$DEADLINE" "$(scaled 300)" "$CODEX" exec "$PROMPT" \
        < /dev/null > "$PREFIX.$name.codex.log" 2>&1)
    CODEX_STATUS=$?
    stop_server
}

# summary NAME: each request of PREFIX.NAME.record on a line, with its step
# and whether it asked for a WebSocket, and each WebSocket message by its
# type; a request without the dummy key or Codex's user agent says so.
summary() {
    python3 - "$PREFIX.$1.record" "$KEY" "$CODEX_VERSION" <<'EOF'
import json
import sys

record, key, version = sys.argv[1:]
for line in open(record, encoding="utf-8"):
    entry = json.loads(line)
    n = entry["n"]
    if "method" in entry:
        headers = {k.lower(): v for k, v in entry["headers"]}
        fields = [n, entry["method"], entry["path"], "step", entry.get("step", "unexpected")]
        if headers.get("upgrade", "").lower() == "websocket":
            fields.append("websocket")
        if headers.get("authorization") != "Bearer " + key:
            fields.append("without the key")
        if not headers.get("user-agent", "").startswith(f"codex_exec/{version} "):
            fields.append("from " + headers.get("user-agent", "nobody"))
        print(*fields)
    elif "received" in entry:
        message = json.loads(entry["received"])
        warmup = " warmup" if message.get("generate") is False else ""
        print(n, "received", message.get("type") + warmup)
    elif "sent" in entry:
        print(n, "sent", json.loads(entry["sent"])["type"])
    else:
        kind = [k for k in entry if k != "n"][0]
        print(n, kind, entry[kind])
EOF
}

play exchange "$(cat "$FIXTURE/notes.txt")"
[ "$CODEX_STATUS" = 0 ] ||
    fail "codex exec ended with status $CODEX_STATUS: $(tail -n 5 "$PREFIX.exchange.codex.log")"
if [ "$SERVER_STATUS" != 0 ] ||
    ! grep -qx 'fakeapi: result: pass: 5 steps played by 5 requests' "$PREFIX.exchange.server.log"; then
    fail "the server's verdict, status $SERVER_STATUS: $(tail -n 1 "$PREFIX.exchange.server.log")"
fi
echo "codexapi-test: Codex played the script, each tool result as it expects: OK"

# Codex may end the connection without answering the server's close.
OUT=$(summary exchange | grep -v -e ' sent response\.' -e ' received_close ')
EXPECTED='1 GET /v1/responses step 1 websocket
1 received response.create warmup
1 received response.create
1 received response.create
1 sent_close 1000
2 GET /v1/responses step 2 websocket
3 POST /v1/responses step 3
4 POST /v1/responses step 4
5 POST /v1/responses step 5'
[ "$OUT" = "$EXPECTED" ] || fail "the record is not the exchange the script plays:
$OUT"
echo "codexapi-test: two turns over a WebSocket, and three over HTTPS after a 426, with the dummy key: OK"

tree="$work/exchange/repo"
[ "$(cat "$tree/greeting.txt")" = 'hello, rach2' ] ||
    fail "greeting.txt says $(cat "$tree/greeting.txt")"
OUT=$(git -C "$tree" log --format='%s|%an <%ae>' 2>&1)
[ "$OUT" = "$(printf 'Greet Rach2|Codex Test <codex@fakeapi.test>\nThe fixture|Codex Test <codex@fakeapi.test>')" ] ||
    fail "the history is not the fixture and Codex's commit: $OUT"
OUT=$(git -C "$tree" diff --name-only HEAD~1 HEAD 2>&1)
[ "$OUT" = greeting.txt ] || fail "the commit changes more than greeting.txt: $OUT"
OUT=$(git -C "$tree" status --porcelain 2>&1)
[ -z "$OUT" ] || fail "the work tree is not clean after the commit: $OUT"
echo "codexapi-test: greeting.txt is changed, and the change is the one new commit: OK"

play wrong 'Change the greeting in greeting.txt to: hello, there'
if [ "$SERVER_STATUS" != 1 ] ||
    ! grep -q '^fakeapi: result: fail: [0-9]* failed, the first request 1: turn 2: the output of call_read with ' \
        "$PREFIX.wrong.server.log"; then
    fail "the server did not refuse a wrong tool result: status $SERVER_STATUS: $(tail -n 1 "$PREFIX.wrong.server.log")"
fi
tree="$work/wrong/repo"
[ "$(cat "$tree/greeting.txt")" = 'hello, world' ] ||
    fail "after the refusal greeting.txt says $(cat "$tree/greeting.txt")"
OUT=$(git -C "$tree" log --format=%s 2>&1)
[ "$OUT" = 'The fixture' ] || fail "after the refusal the history is $OUT"
echo "codexapi-test: a wrong tool result is refused, and the repository is left as it was (codex exec status $CODEX_STATUS): OK"
echo "codexapi-test: PASS"
