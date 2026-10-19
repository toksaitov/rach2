# shellcheck shell=bash
# codex-exec: Codex plays the exchange of tests/system/fakeapi/codex.json with
# `codex exec`, as the test account, in a fresh home and a clone of the shared
# fixture. Asked to, it reads notes.txt and runs check.sh over a WebSocket;
# the server refuses the WebSocket Codex opens again with 426, and Codex goes
# on over HTTPS: it edits greeting.txt with apply_patch, runs check.sh and
# commits with git. The fake server checks each tool result before it answers.
# Codex reaches the server through a model provider of the home's
# ~/.codex/config.toml, trusts the test authority by CODEX_CA_CERTIFICATE and
# signs in with a dummy key; its sandbox is off by /etc/codex/config.toml,
# which the row does not override. Every name Codex reaches as shipped is the
# fake server's too, so a request to any of them that the script does not have
# fails the row; the home's settings turn off Codex's plugins, which would
# fetch their catalog from chatgpt.com and api.github.com. The row first logs
# in as a person does, in a login shell, whose profile starts the user's D-Bus
# session bus, and gives Codex the bus's address, as a program run from that
# login has it; the login shell Codex runs to learn the environment then finds
# the bus and starts none. The row's script has the last answer held, and
# while Codex waits for it the row requires the process to run OpenAI's
# program with nothing preloaded: it maps no file but the program, the time
# zone and the shared memory of its SQLite databases in ~/.codex. Once the
# session has ended, every process Codex started must exit.
# The row returns in /tmp/agents/results/codex-exec.tar the repository, the
# program's digest before and after the session, the free frames sampled each
# second and the time of each step; codex_session_verify.py judges them on the
# host with every recorded request.
# bound: 900
# hosts: fakeapi.test api.openai.com chatgpt.com auth.openai.com api.github.com
# hosts: raw.githubusercontent.com
# script: fakeapi/codex.json
# script-filter: codex_session_verify.py
# package: codex
# verifier: codex_session_verify.py

PROMPT="Read notes.txt and do what it says. Check the change with check.sh, then commit it."

# session_start MODE: make the session's home, scratch and the fixture's
# repository in SESSION, WORK/session, with Codex's settings for the fake
# server, start the session bus of the home's login in BUS, record MODE, the
# program's digest and the start, and sample the free frames until
# session_end.
session_start() {
    SESSION=$WORK/session
    mkdir -p "$SESSION/home/.codex" "$SESSION/tmp" "$SESSION/repo" /tmp/agents/results
    # shellcheck disable=SC2016 # the login shell expands the variable
    output "the login's session bus" env -i "HOME=$SESSION/home" TERM=dumb \
        bash -lc 'printf %s "$DBUS_SESSION_BUS_ADDRESS"'
    BUS=$OUT
    [ -n "$BUS" ] || { fail "the login gives no session bus"; return 1; }
    printf '[user]\n\tname = Codex Test\n\temail = codex@fakeapi.test\n' > "$SESSION/home/.gitconfig"
    cat > "$SESSION/home/.codex/config.toml" <<'EOF'
model = "gpt-5.5"
model_provider = "fakeapi"
approval_policy = "never"

[features]
plugins = false

[analytics]
enabled = false

[model_providers.fakeapi]
name = "fakeapi"
base_url = "https://fakeapi.test/v1"
env_key = "FAKEAPI_KEY"
wire_api = "responses"
supports_websockets = true
EOF
    cp /tmp/agents/fixture/* "$SESSION/repo/"
    cd "$SESSION/repo" || { fail "the session's repository"; return 1; }
    output "git init" env HOME="$SESSION/home" git init -q .
    output "git add" git add .
    output "the fixture's commit" env HOME="$SESSION/home" git commit -qm "The fixture"
    printf '%s\n' "$1" > "$SESSION/mode"
    output "the program's digest before the session" sha256sum /opt/codex/bin/codex
    printf '%s\n' "$OUT" > "$SESSION/binary.before"
    : > "$SESSION/steps"
    sample_memory > "$SESSION/memory" &
    SAMPLER=$!
    BEGAN=$(date +%s)
}

# step WHAT: record the seconds since session_start at WHAT.
step() {
    printf '%s %s\n' "$(($(date +%s) - BEGAN))" "$1" >> "$SESSION/steps"
}

# session_end: stop sampling, record the session's length and the program's
# digest, and archive what the host verifies.
session_end() {
    printf '%s\n' "$(($(date +%s) - BEGAN))" > "$SESSION/seconds"
    kill "$SAMPLER"
    wait "$SAMPLER" 2> /dev/null
    SAMPLER=
    output "the program's digest after the session" sha256sum /opt/codex/bin/codex
    printf '%s\n' "$OUT" > "$SESSION/binary.after"
    output "the session's archive" tar -cf "/tmp/agents/results/$ROW.tar" -C "$SESSION" \
        repo mode binary.before binary.after memory seconds steps
}

# session_env TERM: print the environment of the session, a variable to a
# line: that of the codexapi suite's Codex, with TERM, the session bus BUS,
# and MARK, which every process Codex starts inherits.
session_env() {
    printf '%s\n' PATH=/usr/bin:/bin "HOME=$SESSION/home" LANG=C.UTF-8 "TERM=$1" \
        "TMPDIR=$SESSION/tmp" FAKEAPI_KEY=sk-fakeapi-dummy "CODEX_CA_CERTIFICATE=$CA" \
        "DBUS_SESSION_BUS_ADDRESS=$BUS" "$MARK"
}

MARK=RACH2_AGENTS_ROW=$ROW

# left: print, a process to a line, each process of the test account that
# has MARK in its environment or runs a program under /opt/codex or the
# session's directory, where Codex copies itself to start its background
# server.
left() {
    local dir exe
    for dir in /proc/[0-9]*; do
        exe=$(readlink "$dir/exe" 2> /dev/null) || continue
        case $exe in
            /opt/codex/* | "$SESSION"/*) ;;
            *) tr '\0' '\n' < "$dir/environ" 2> /dev/null | grep -qx "$MARK" || continue ;;
        esac
        printf '%s %s\n' "${dir#/proc/}" "$exe"
    done
}

# gone WHAT: wait for every process Codex started to exit, once its session
# has ended; 120 s only ends the wait for one that does not.
gone() {
    exited "$1: the processes left" 120 left
}

# Stop the sampler, the private tmux server, what Codex left and the
# login's session bus on every exit path.
row_cleanup() {
    if [ -n "${SAMPLER:-}" ]; then
        kill "$SAMPLER" 2> /dev/null
        wait "$SAMPLER" 2> /dev/null
        SAMPLER=
    fi
    tmux -L codex kill-server 2> /dev/null || true
    if [ -n "${SESSION:-}" ]; then
        left | awk '{ print $1 }' | xargs -r kill -9 2> /dev/null
    fi
    if [ -n "${BUS:-}" ]; then
        pkill -u "$(id -u)" -f -- "dbus-daemon --session --address=$BUS "
    fi
}

row() {
    local pid status env
    session_start exec || return
    mapfile -t env < <(session_env dumb)
    env -i "${env[@]}" codex exec "$PROMPT" < /dev/null > "$SESSION/client.log" 2>&1 &
    pid=$!
    if held "codex exec" "$pid"; then
        step "the last answer held"
        unmodified "codex exec" "$pid" /opt/codex/bin/codex \
            "$(readlink -f /etc/localtime)" "$SESSION/home/.codex/*.sqlite-shm"
        release "$pid"
    fi
    wait "$pid"
    status=$?
    step "the session ended"
    [ "$status" = 0 ] ||
        fail "codex exec: status $status: $(tail -n 3 "$SESSION/client.log" | tr '\n' ' ')"
    gone "codex exec"
    session_end
}
