#!/bin/sh
#
# agentapi.sh: the program of the host API cases, tests/cases/host.codexapi.json
# and tests/cases/host.museapi.json: the Responses protocol of the fake API
# server, tests/system/fakeapi/fakeapi.py, proved against a coding agent on
# the host, OpenAI's Codex or Meta's Muse Code.
#
# A case runs the agent at the version it pins, in a fresh home, on a clone of
# tests/system/fakeapi/repo, against the server playing the agent's script,
# tests/system/fakeapi/codex.json or muse.json. The program runs itself again
# in namespaces of its own: a network namespace with only a loopback
# interface, so that neither the agent nor the commands it runs can reach
# another host, and a PID namespace whose first process it is, so that every
# process the case starts ends when the program does. The agent signs in with
# a dummy key, reaches the server at a port of 127.0.0.1, and trusts the test
# certificate authority that the program makes; its sandbox and approvals are
# off.
#
# Codex reaches the server through a model provider in its configuration
# whose base_url is the server, trusts the authority by CODEX_CA_CERTIFICATE,
# and takes the key from the variable the provider's env_key names. Its
# script has it read notes.txt and run check.sh over a WebSocket, then
# refuses the WebSocket Codex opens again with 426, so the rest goes over
# HTTPS: an apply_patch edit of greeting.txt, check.sh and a commit with git.
#
# Muse is the Linux binary that tests/system/fakeapi/fetch_muse.sh downloads
# and checks against the digest of Meta's release manifest, which the cases'
# preparation links as build/downloads/muse/muse. It reaches the server by
# --base-url, trusts the authority by the setting
# endpoint_transport.ca_bundle, and takes the key from META_API_KEY. --yolo
# turns its sandbox and approvals off, and its settings file turns off its
# telemetry and its observer agents, whose model calls would run beside the
# main agent's. Its script answers Muse's model catalog,
# `GET /muse-code/models`, with the Model API's list of models, then has Muse
# read notes.txt, run check.sh, edit greeting.txt and commit with git, each a
# function of its namespace tool `muse`.
#
# Each run is one case, named by its arguments. In `exchange` the server
# checks each tool result before it answers, and the program judges the
# agent's exit, the server's verdict, the record of each request, the key and
# the agent each request names, the file and the commit. In `refusal`
# notes.txt says something else, and the server must refuse the agent's tool
# result and leave the repository as it was; Muse retries a refused request
# for minutes, so the program stops it once the server has refused. Both
# cases first require the namespace to have only its loopback interface and
# the agent to be the pinned version, and last that every process the case
# started has ended and the homes, the clones and the authority's key are
# removed. An assertion that a failure before it leaves without its evidence
# is reported as not run.
#
# The program writes the case's report records on its standard output
# (tools/etude/Readme.md, Report records), with the lines a person reads
# between them, and exits 0 when every assertion passed and 1 otherwise.
# etude runs it in the root of the tree, with TMPDIR a scratch directory of
# the case's own, which etude keeps after a failure.
#
# Usage, in the root of the tree:
#     sh tests/cases/host/agentapi.sh codex|muse exchange|refusal
#
# The logs and the server's record go to a private directory under TMPDIR,
# whose path the first line prints; a run that passes or is stopped by a
# signal removes it, and one that fails keeps it.
#

# shellcheck disable=SC2329 # the stop functions are called from the traps
set -u

usage() {
    echo "agentapi: usage: agentapi.sh codex|muse exchange|refusal" >&2
    exit 2
}
[ "$#" -eq 2 ] || usage
AGENT=$1
CASE=$2
case "$AGENT $CASE" in
    "codex exchange" | "codex refusal" | "muse exchange" | "muse refusal") ;;
    *) usage ;;
esac
[ -f tests/system/fakeapi/fakeapi.py ] || {
    echo "agentapi: run it in the root of the tree" >&2
    exit 2
}
REPO=$(pwd -P)
case $0 in
    /*) SELF=$0 ;;
    *) SELF=$REPO/$0 ;;
esac
ID=host.${AGENT}api.$CASE

FAKEAPI="$REPO/tests/system/fakeapi/fakeapi.py"
SCRIPT="$REPO/tests/system/fakeapi/$AGENT.json"
FIXTURE="$REPO/tests/system/fakeapi/repo"
CODEX=codex
CODEX_VERSION=0.162.1
MUSE="$REPO/build/downloads/muse/muse"
MUSE_VERSION=1.4.10-R6427.1
MUSE_MODEL=muse-spark-1.2
# The observer agents' capabilities, each turned off in Muse's settings file.
OBSERVERS='goal-reminder memory scope-reminder skill-reminder todo-reminder verify-reminder'
PROMPT='Read notes.txt and do what it says. Check the change with check.sh, then commit it.'
case $AGENT in
    codex)
        NAME=Codex
        KEY=sk-fakeapi-dummy
        USER_AGENT="codex_exec/$CODEX_VERSION "
        STEPS=5
        EXCHANGE='1 GET /v1/responses step 1 websocket
1 received response.create warmup
1 received response.create
1 received response.create
1 sent_close 1000
2 GET /v1/responses step 2 websocket
3 POST /v1/responses step 3
4 POST /v1/responses step 4
5 POST /v1/responses step 5'
        REFUSAL='the first request 1: turn 2: the output of call_read with '
        ;;
    muse)
        NAME=Muse
        KEY=meta-fakeapi-dummy
        USER_AGENT="muse-build/${MUSE_VERSION%%-*} "
        STEPS=6
        EXCHANGE='1 GET /muse-code/models step 1
2 POST /v1/responses step 2
3 POST /v1/responses step 3
4 POST /v1/responses step 4
5 POST /v1/responses step 5
6 POST /v1/responses step 6'
        REFUSAL='the first request 3: POST /v1/responses: step 3: the output of call_read with '
        ;;
esac
case $CASE in
    exchange) ASSERTIONS='isolated version exit verdict exchange credentials greeting commit cleanup' ;;
    refusal) ASSERTIONS='isolated version refused unchanged cleanup' ;;
esac

# ---- The report. ----

RECORDS=0
FAILED=""
REPORTED=" "

# Write the report record WORDS, which fits in one write of 255 bytes.
record() {
    printf '@etude 1 %s\n' "$*"
}

# Make TEXT printable ASCII of at most 160 bytes with no space at either
# end, for a record's detail, which a space there would keep from parsing.
clip() {
    _clip=$(printf '%s' "$1" | tr -c ' -~' '?' | sed 's/^ *//' | cut -c1-160 | sed 's/ *$//')
    printf '%s' "${_clip:-?}"
}

# pass ID: report that the assertion ID passed.
pass() {
    record "assert $1 pass"
    RECORDS=$((RECORDS + 1))
    REPORTED="$REPORTED$1 "
}

# fail ID DETAIL: report that the assertion ID failed, with DETAIL, which is
# printed whole for the reader.
fail() {
    printf 'agentapi: %s: %s\n' "$1" "$2"
    record "assert $1 fail $(clip "$2")"
    RECORDS=$((RECORDS + 1))
    REPORTED="$REPORTED$1 "
    FAILED="$FAILED $1"
}

# notrun ID REASON: report that the assertion ID could not be judged.
notrun() {
    record "notrun $1 $(clip "$2")"
    RECORDS=$((RECORDS + 1))
    REPORTED="$REPORTED$1 "
}

# ---- The case's own namespaces. ----

# Outside the namespaces, run this file again inside them, and report the
# case's failure when they cannot be made: an unprivileged user namespace
# that is root in a network namespace, which brings its loopback interface
# up, and a PID namespace with its own /proc, in which the program runs as
# the same user in a user namespace of its own. The program inside writes
# LOGS/started, and every record after `begin`.
if [ -z "${RACH2_AGENTAPI_LOGS:-}" ]; then
    if [ -n "${ETUDE_CASE:-}" ] && [ "$ETUDE_CASE" != "$ID" ]; then
        echo "agentapi: ETUDE_CASE is $ETUDE_CASE, and the arguments name $ID" >&2
        exit 2
    fi
    LOGS=""
    trap 'status=$?; [ "$status" -ne 0 ] || rm -rf ${LOGS:+"$LOGS"}; exit "$status"' EXIT
    trap 'rm -rf ${LOGS:+"$LOGS"}; exit 129' HUP
    trap 'rm -rf ${LOGS:+"$LOGS"}; exit 130' INT
    trap 'rm -rf ${LOGS:+"$LOGS"}; exit 143' TERM
    LOGS=$(umask 077; mktemp -d "${TMPDIR:-/tmp}/rach2-agentapi.XXXXXX") || {
        echo "agentapi: cannot create a temporary directory" >&2
        exit 1
    }
    printf 'agentapi: logs: %s\n' "$LOGS"
    record "begin $ID"
    # shellcheck disable=SC2016 # the inner shell expands its own arguments
    unshare --user --map-root-user --net --pid --fork --kill-child --mount-proc sh -c \
        'ip link set lo up && exec unshare --user --map-user="$1" --map-group="$2" \
            env RACH2_AGENTAPI_LOGS="$3" sh "$4" "$5" "$6"' \
        sh "$(id -u)" "$(id -g)" "$LOGS" "$SELF" "$AGENT" "$CASE"
    status=$?
    [ ! -e "$LOGS/started" ] || exit "$status"
    fail isolated "the namespaces of the case could not be made: unshare exited with status $status"
    for a in $ASSERTIONS; do
        [ "$a" = isolated ] || notrun "$a" "the case did not start in its namespaces"
    done
    record "end $RECORDS"
    echo "agentapi: result: failed: $ID: isolated"
    exit 1
fi

LOGS=$RACH2_AGENTAPI_LOGS
: > "$LOGS/started"

SPID=""
CPID=""
RPID=""
MPID=""
SERVER_STATUS=""
AGENT_STATUS=""
work=""

# Stop the server, which writes its verdict as it ends, keep its exit status
# in SERVER_STATUS, and wait for the copy of its log to end.
stop_server() {
    if [ -n "$SPID" ]; then
        kill -TERM "$SPID" 2>/dev/null
        wait "$SPID"
        SERVER_STATUS=$?
        SPID=""
    fi
    if [ -n "$CPID" ]; then
        wait "$CPID"
        CPID=""
        exec 4<&-
    fi
}

# Stop Muse, and wait for the process that runs it to end.
stop_muse() {
    if [ -n "$MPID" ]; then
        kill -TERM "$MPID" 2>/dev/null
        MPID=""
    fi
    if [ -n "$RPID" ]; then
        wait "$RPID"
        RPID=""
    fi
}

trap 'stop_muse; stop_server; rm -rf ${work:+"$work"}' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(umask 077; mktemp -d "${TMPDIR:-/tmp}/rach2-agentapi-work.XXXXXX") || {
    echo "agentapi: cannot create the work directory" >&2
    exit 1
}

# check_cleanup: stop the agent and the server, and report whether every
# process the case started has ended, a zombie counting as ended, and the
# work directory, with the homes, the clones and the authority's key, is
# removed. The processes are read from the PID namespace's /proc, in which
# this program is process 1.
check_cleanup() {
    stop_muse
    stop_server
    left=""
    if [ "$$" != 1 ]; then
        left=" the program is process $$, not the first of a PID namespace"
    else
        for p in /proc/[0-9]*; do
            [ "$p" != /proc/1 ] || continue
            { read -r stat < "$p/stat"; } 2>/dev/null || continue
            state=${stat##*") "}
            [ "${state%% *}" != Z ] || continue
            comm=${stat#*"("}
            left="$left ${p#/proc/} (${comm%")"*})"
        done
    fi
    [ -z "$work" ] || rm -rf "$work"
    if [ -n "$left" ]; then
        fail cleanup "processes outlive the case:$left"
    elif [ -n "$work" ] && [ -e "$work" ]; then
        fail cleanup "$work is not removed"
    else
        pass cleanup
    fi
    work=""
}

# finish: report the cleanup, report each assertion of the case that a
# failure left without its evidence as not run, end the report, and exit 0
# when every assertion passed and 1 otherwise.
finish() {
    case "$REPORTED" in
        *" cleanup "*) ;;
        *) check_cleanup ;;
    esac
    for a in $ASSERTIONS; do
        case "$REPORTED" in
            *" $a "*) ;;
            *) notrun "$a" "not judged after the failure of$FAILED" ;;
        esac
    done
    record "end $RECORDS"
    if [ -n "$FAILED" ]; then
        echo "agentapi: result: failed: $ID:$FAILED"
        exit 1
    fi
    echo "agentapi: result: passed: $ID"
    exit 0
}

# abort ID DETAIL: fail the assertion ID with DETAIL and finish the case.
abort() {
    fail "$1" "$2"
    finish
}

# ---- The server and the agent. ----

# watch_log LOG FIFO NOTICE: copy the server's standard error from FIFO to
# LOG, and write to NOTICE `listening PORT` when the server listens and
# `refused` when it first refuses a request.
watch_log() {
    exec 3> "$3"
    refused=""
    while IFS= read -r line; do
        printf '%s\n' "$line" >> "$1"
        case $line in
            "fakeapi: listening on 127.0.0.1:"*)
                echo "listening ${line##*:}" >&3
                ;;
            "fakeapi: "[0-9]*": "*": unexpected: "*)
                [ -n "$refused" ] || echo refused >&3
                refused=1
                ;;
        esac
    done < "$2"
}

# await WORD...: read the notices of descriptor 4 until one whose first word
# is a WORD, and keep it in NOTICE; return 1 when every writer has closed
# the notices first.
await() {
    while IFS= read -r NOTICE <&4; do
        for w in "$@"; do
            case $NOTICE in
                "$w" | "$w "*) return 0 ;;
            esac
        done
    done
    return 1
}

# serve: start the server on the agent's script, with its record LOGS/record
# and its log LOGS/server.log, and set PORT once it listens. SPID is the
# server, CPID the copy of its log, and descriptor 4 reads the notices of the
# copy and of Muse's runner. Return 1 when the server ends before it listens.
serve() {
    mkfifo "$work/server.err" "$work/notice" || return 1
    watch_log "$LOGS/server.log" "$work/server.err" "$work/notice" &
    CPID=$!
    python3 -B "$FAKEAPI" serve "$SCRIPT" "$LOGS/record" "$work/ca" \
        > /dev/null 2> "$work/server.err" &
    SPID=$!
    exec 4< "$work/notice"
    await listening || return 1
    PORT=${NOTICE#listening }
}

# fixture NOTES: make the agent's fresh home, $work/home, and a clone of the
# fixture whose notes.txt says NOTES, $work/repo, with the fixture committed.
fixture() {
    home=$work/home
    tree=$work/repo
    mkdir -p "$home" "$tree" || return 1
    printf '[user]\n\tname = %s Test\n\temail = %s@fakeapi.test\n' "$NAME" "$AGENT" \
        > "$home/.gitconfig"
    cp "$FIXTURE/check.sh" "$FIXTURE/greeting.txt" "$tree/" || return 1
    printf '%s\n' "$1" > "$tree/notes.txt"
    (cd "$tree" && HOME=$home && export HOME && git init -q . && git add . &&
        git commit -q -m 'The fixture')
}

# run_codex: write Codex's configuration for the server at PORT, and run
# `codex exec` in the clone, its environment only what it is given. Its exit
# status is AGENT_STATUS.
run_codex() {
    mkdir -p "$home/.codex"
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
base_url = "https://localhost:$PORT/v1"
env_key = "FAKEAPI_KEY"
wire_api = "responses"
supports_websockets = true
EOF
    (cd "$tree" && exec env -i PATH="$PATH" HOME="$home" CODEX_HOME="$home/.codex" \
        TMPDIR="$work" LANG=C.UTF-8 TERM=dumb FAKEAPI_KEY="$KEY" \
        CODEX_CA_CERTIFICATE="$work/ca/ca.pem" \
        "$CODEX" exec "$PROMPT" < /dev/null > "$LOGS/agent.log" 2>&1 4<&-)
    AGENT_STATUS=$?
}

# start_muse: write Muse's settings, and start `muse exec` in the clone
# against the server at PORT, its environment only what it is given. RPID is
# the process that runs it, which writes the notices `started PID` and, once
# Muse ends, `ended STATUS`; MPID is Muse.
start_muse() {
    mkdir -p "$home/.config/muse"
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
    (
        cd "$tree" || exit 1
        env -i PATH="$PATH" HOME="$home" TMPDIR="$work" LANG=C.UTF-8 TERM=dumb \
            META_API_KEY="$KEY" "$MUSE" exec --yolo --model "$MUSE_MODEL" \
            --max-model-steps 8 --base-url "https://localhost:$PORT/v1" \
            -c "endpoint_transport.ca_bundle=\"$work/ca/ca.pem\"" "$PROMPT" \
            < /dev/null > "$LOGS/agent.log" 2>&1 3>&- 4<&- &
        echo "started $!" >&3
        wait "$!"
        echo "ended $?" >&3
    ) 3> "$work/notice" &
    RPID=$!
    await started || return 1
    MPID=${NOTICE#started }
}

# summary MODE: read LOGS/record. In the mode `exchange`, print each request
# on a line, with its step and whether it asked for a WebSocket, and each
# WebSocket message by its type. In the mode `credentials`, print each
# request that lacks the dummy key or the agent's user agent, and a line
# when there is no request.
summary() {
    python3 -I - "$1" "$LOGS/record" "$KEY" "$USER_AGENT" <<'EOF'
import json
import sys

mode, record, key, agent = sys.argv[1:]
requests = 0
for line in open(record, encoding="utf-8"):
    entry = json.loads(line)
    n = entry["n"]
    if "method" in entry:
        requests += 1
        headers = {k.lower(): v for k, v in entry["headers"]}
        if mode == "credentials":
            if headers.get("authorization") != "Bearer " + key:
                print(n, "without the dummy key")
            if not headers.get("user-agent", "").startswith(agent):
                print(n, "from", headers.get("user-agent", "nobody"))
            continue
        fields = [n, entry["method"], entry["path"], "step", entry.get("step", "unexpected")]
        if headers.get("upgrade", "").lower() == "websocket":
            fields.append("websocket")
        print(*fields)
    elif mode == "credentials":
        continue
    elif "received" in entry:
        message = json.loads(entry["received"])
        warmup = " warmup" if message.get("generate") is False else ""
        print(n, "received", message.get("type") + warmup)
    elif "sent" in entry:
        print(n, "sent", json.loads(entry["sent"])["type"])
    else:
        kind = [k for k in entry if k != "n"][0]
        print(n, kind, entry[kind])
if mode == "credentials" and requests == 0:
    print("no request")
EOF
}

# ---- The cases. ----

# The namespace has no interface but loopback, and an outside address is
# unreachable from it.
check_isolation() {
    links=$(ip -o link show 2>&1 | awk -F': ' '{ printf "%s%s", sep, $2; sep = " " }')
    [ "$links" = lo ] || abort isolated "the namespace has the interfaces $links"
    out=$(python3 -I -c '
import socket
try:
    socket.create_connection(("1.1.1.1", 443), timeout=10)
    print("connected")
except OSError as error:
    print(error)
' 2>&1)
    case $out in
        *unreachable*) pass isolated ;;
        *) abort isolated "the namespace reached an outside address: $out" ;;
    esac
}

# The agent is the version the case pins.
check_version() {
    case $AGENT in
        codex)
            out=$("$CODEX" --version 2>&1)
            [ "$out" = "codex-cli $CODEX_VERSION" ] ||
                abort version "$CODEX is not Codex $CODEX_VERSION: $out"
            ;;
        muse)
            [ -x "$MUSE" ] ||
                abort version "$MUSE is not prepared; make build/downloads/muse/muse fetches it"
            out=$("$MUSE" --version 2>&1)
            [ "$out" = "Muse Code ${MUSE_VERSION%%-*} ($MUSE_VERSION)" ] ||
                abort version "$MUSE is not Muse Code $MUSE_VERSION: $out"
            ;;
    esac
    pass version
}

# The agent plays the script whole: it ends with status 0, the server's
# verdict passes, the record is the exchange the script plays, each request
# has the dummy key and the agent's user agent, and greeting.txt is changed
# in one new commit.
case_exchange() {
    fixture "$(cat "$FIXTURE/notes.txt")" ||
        abort exit "the agent did not run: the clone of the fixture could not be made"
    serve || abort exit "the agent did not run: the server did not listen: $(tail -n 3 "$LOGS/server.log")"
    case $AGENT in
        codex) run_codex ;;
        muse)
            start_muse || abort exit "Muse did not start"
            await ended || abort exit "Muse's runner ended before Muse"
            AGENT_STATUS=${NOTICE#ended }
            MPID=""
            stop_muse
            ;;
    esac
    stop_server
    if [ "$AGENT_STATUS" = 0 ]; then
        pass exit
    else
        fail exit "$AGENT exec ended with status $AGENT_STATUS: $(tail -n 5 "$LOGS/agent.log")"
    fi
    if [ "$SERVER_STATUS" = 0 ] &&
        grep -qx "fakeapi: result: pass: $STEPS steps played by $STEPS requests" "$LOGS/server.log"; then
        pass verdict
    else
        fail verdict "the server ended with status $SERVER_STATUS: $(tail -n 1 "$LOGS/server.log")"
    fi
    # Codex may end the connection without answering the server's close.
    out=$(summary exchange 2>&1 | grep -v -e ' sent response\.' -e ' received_close ')
    if [ "$out" = "$EXCHANGE" ]; then
        pass exchange
    else
        fail exchange "the record is not the exchange the script plays: $out"
    fi
    out=$(summary credentials 2>&1)
    if [ -z "$out" ]; then
        pass credentials
    else
        fail credentials "a request lacks the dummy key or $NAME's user agent: $out"
    fi
    out=$(cat "$tree/greeting.txt" 2>&1)
    if [ "$out" = 'hello, rach2' ]; then
        pass greeting
    else
        fail greeting "greeting.txt says $out"
    fi
    out=$(git -C "$tree" log --format='%s|%an <%ae>' 2>&1)
    who="$NAME Test <$AGENT@fakeapi.test>"
    if [ "$out" != "$(printf 'Greet Rach2|%s\nThe fixture|%s' "$who" "$who")" ]; then
        fail commit "the history is not the fixture and $NAME's commit: $out"
    elif out=$(git -C "$tree" diff --name-only HEAD~1 HEAD 2>&1) && [ "$out" != greeting.txt ]; then
        fail commit "the commit changes more than greeting.txt: $out"
    elif out=$(git -C "$tree" status --porcelain 2>&1) && [ -n "$out" ]; then
        fail commit "the work tree is not clean after the commit: $out"
    else
        pass commit
    fi
    finish
}

# The server refuses the tool result of notes.txt that says something else,
# and the repository is left as it was.
case_refusal() {
    fixture 'Change the greeting in greeting.txt to: hello, there' ||
        abort refused "the agent did not run: the clone of the fixture could not be made"
    serve || abort refused "the agent did not run: the server did not listen: $(tail -n 3 "$LOGS/server.log")"
    early=""
    case $AGENT in
        codex) run_codex ;;
        muse)
            start_muse || abort refused "Muse did not start"
            await refused ended || abort refused "the notices ended before a refusal"
            case $NOTICE in
                ended*) early="Muse ended with status ${NOTICE#ended } before the server refused a request" ;;
            esac
            stop_muse
            ;;
    esac
    stop_server
    if [ -n "$early" ]; then
        fail refused "$early"
    elif [ "$SERVER_STATUS" = 1 ] &&
        grep -q "^fakeapi: result: fail: [0-9]* failed, $REFUSAL" "$LOGS/server.log"; then
        pass refused
    else
        fail refused "the server did not refuse a wrong tool result: status $SERVER_STATUS: $(tail -n 1 "$LOGS/server.log")"
    fi
    out=$(cat "$tree/greeting.txt" 2>&1)
    history=$(git -C "$tree" log --format=%s 2>&1)
    if [ "$out" != 'hello, world' ]; then
        fail unchanged "after the refusal greeting.txt says $out"
    elif [ "$history" != 'The fixture' ]; then
        fail unchanged "after the refusal the history is $history"
    else
        pass unchanged
    fi
    finish
}

# ---- The run. ----

check_isolation
check_version
python3 -B "$FAKEAPI" ca "$work/ca" localhost > "$LOGS/ca.log" 2>&1 || {
    case $CASE in
        exchange) abort exit "the agent did not run: the certificate authority: $(cat "$LOGS/ca.log")" ;;
        refusal) abort refused "the agent did not run: the certificate authority: $(cat "$LOGS/ca.log")" ;;
    esac
}
"case_$CASE"
