#!/bin/sh
# agent_fakeapi.sh: the TLS, HTTP and WebSocket workload of etude's
# fakeapi case.
set -u
REPO=${ETUDE_TREE:?}
. "$REPO/tests/system/deadline.sh"
work=${AGENT_WORK:?}
PORT=${AGENT_PORT:?}
ROOT_KEY=${AGENT_ROOT_KEY:?}
TEST_KEY=${AGENT_TEST_KEY:?}
KNOWN=$work/known
RECORD=$work/record

# Report a failed transport or protocol assertion.
fail() {
    echo "fakeapi-test: FAIL: $*" >&2
    exit 1
}
# Run COMMAND as USER by KEY, within the scenario step that owns it.
ssh_as() {
    ssh -F /dev/null -i "$2" -p "$PORT" -o StrictHostKeyChecking=no \
        -o "UserKnownHostsFile=$KNOWN" -o "ConnectTimeout=$(scaled 30)" -o BatchMode=yes \
        -o IdentitiesOnly=yes -o PreferredAuthentications=publickey "$1@127.0.0.1" "$3"
}
# guest COMMAND: run COMMAND as the test account, an ordinary user, as an
# agent runs, with its standard error.
guest() {
    ssh_as test "$TEST_KEY" "$1" 2>&1
}

case $1 in
prepare)
    mkdir -p "$work"
    python3 -B "$REPO/tests/system/fakeapi/fakeapi.py" ca "$work/ca" fakeapi.test 10.0.2.100 || exit 1
    cp "$REPO/tests/system/fakeapi/fakeapi-test.json" "$work/script.json" || exit 1
    ;;
setup)
echo "fakeapi-test: the machine answers SSH on a restricted network: OK"

ssh_as root "$ROOT_KEY" \
    'mkdir -p -m 755 /tmp/fakeapi && echo "10.0.2.100 fakeapi.test" >> /etc/hosts' ||
    fail "the server's name in /etc/hosts"
scp -F /dev/null -P "$PORT" -i "$ROOT_KEY" -o StrictHostKeyChecking=no \
    -o "UserKnownHostsFile=$KNOWN" -o BatchMode=yes -o IdentitiesOnly=yes "$work/ca/ca.pem" \
    "$REPO/tests/system/fakeapi/wsclient.py" root@127.0.0.1:/tmp/fakeapi/ > /dev/null 2>&1 ||
    fail "scp could not put the authority's certificate and the client on the machine"
ssh_as root "$ROOT_KEY" 'chmod 644 /tmp/fakeapi/*' ||
    fail "the modes of the authority's certificate and the client"

    ;;
client)
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
    ;;
verify)
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
    ;;
*) exit 2 ;;
esac
