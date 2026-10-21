# shellcheck shell=bash
# agy-print: Antigravity CLI plays the exchange of
# tests/system/fakeapi/agy-session.json with `agy -p`, as the test account.
# Asked to, it reads the fixture with view_file, runs a command with
# run_command, edits the file with replace_file_content and commits it with
# git through run_command, and the fake server checks each function
# response before it answers. The row makes a home with the settings of
# agy_test.py's client on Linux and the fixture's repository, and returns in
# /tmp/agents/results/agy-print.tar the repository, the program's digest
# before and after the session, the free frames sampled each second and the
# time of each step; agy_session_verify.py judges them on the host with every
# recorded request. The Gemini API is the fake server, which
# GOOGLE_GEMINI_BASE_URL names. The names of the program's other services,
# which it asks for feature flags and sends usage logs to even with
# telemetry off, and of the Gemini API as shipped, are the fake server's too:
# the script answers the first two with 404 however often they come, and a
# request it does not have fails the row. The machine has no route anywhere
# else. The row's script has the last answer held, and while the program
# waits for it the row requires the process to run Google's file with
# nothing preloaded.
# hosts: fakeapi.test generativelanguage.googleapis.com
# hosts: antigravity-unleash.goog play.googleapis.com
# script: fakeapi/agy-session.json
# script-filter: agy_session_verify.py
# package: agy
# verifier: agy_session_verify.py

PROMPT="Read fixture.txt, check it with the shell, change answer=before to answer=after, and commit the change."

# session_start MODE: make the session's home, scratch and the fixture's
# repository in SESSION, WORK/session, record MODE, the repository's path,
# the program's digest and the start, and sample the free frames until
# session_end.
session_start() {
    SESSION=$WORK/session
    mkdir -p "$SESSION/home/.gemini/antigravity-cli" "$SESSION/tmp" "$SESSION/repo" \
        /tmp/agents/results
    printf '%s\n' '{"modelProvider": "gemini", "enableTelemetry": false}' \
        > "$SESSION/home/.gemini/antigravity-cli/settings.json"
    printf 'rach2 fixture 493\nanswer=before\n' > "$SESSION/repo/fixture.txt"
    cd "$SESSION/repo" || { fail "the session's repository"; return 1; }
    output "git init" env HOME="$SESSION/home" git init -q --initial-branch=master
    git config user.name "Rach2 test"
    git config user.email test@invalid
    git config commit.gpgsign false
    output "git add" env HOME="$SESSION/home" git add fixture.txt
    output "the fixture's commit" env HOME="$SESSION/home" \
        git commit -qm "Create the offline fixture"
    printf '%s\n' "$1" > "$SESSION/mode"
    printf '%s\n' "$SESSION/repo" > "$SESSION/at"
    output "the program's digest before the session" sha256sum /opt/agy/bin/agy
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
    output "the program's digest after the session" sha256sum /opt/agy/bin/agy
    printf '%s\n' "$OUT" > "$SESSION/binary.after"
    output "the session's archive" tar -cf "/tmp/agents/results/$ROW.tar" -C "$SESSION" \
        repo mode at binary.before binary.after memory seconds steps
}

# session_env TERM: print the environment of the session, a variable to a
# line: the settings of agy_test.py's client, with TERM.
session_env() {
    printf '%s\n' PATH=/usr/bin:/bin "HOME=$SESSION/home" LC_ALL=C "TERM=$1" \
        "TMPDIR=$SESSION/tmp" GEMINI_API_KEY=rach2-offline-dummy-key \
        GOOGLE_GEMINI_BASE_URL=https://fakeapi.test "SSL_CERT_FILE=$CA" \
        AGY_CLI_DISABLE_AUTO_UPDATE=true
}

# session_unmodified WHAT PID: require the process PID to run Google's file
# with nothing preloaded. Besides glibc's libraries the program maps the
# shared memory of its SQLite databases, the summaries of its conversations
# and one for each conversation.
session_unmodified() {
    local data=$SESSION/home/.gemini/antigravity-cli
    unmodified "$1" "$2" /opt/agy/bin/agy \
        /lib64/ld-linux-x86-64.so.2 /lib/libc.so.6 /lib/libm.so.6 /lib/libpthread.so.0 \
        /lib/libdl.so.2 /lib/librt.so.1 /lib/libresolv.so.2 \
        "$data"/*.db-shm "$data"/conversations/*.db-shm
}

# Stop the sampler and the private tmux server on every exit path.
row_cleanup() {
    if [ -n "${SAMPLER:-}" ]; then
        kill "$SAMPLER" 2> /dev/null
        wait "$SAMPLER" 2> /dev/null
        SAMPLER=
    fi
    tmux -L agy kill-server 2> /dev/null || true
}

row() {
    local pid status env
    session_start print || return
    mapfile -t env < <(session_env dumb)
    env -i "${env[@]}" agy -p "$PROMPT" --output-format json --dangerously-skip-permissions \
        --print-timeout 10m > "$SESSION/client.log" 2>&1 &
    pid=$!
    if held "agy -p" "$pid"; then
        step "the last answer held"
        session_unmodified "agy -p" "$pid"
        release "$pid"
    fi
    wait "$pid"
    status=$?
    step "the session ended"
    [ "$status" = 0 ] ||
        fail "agy -p: status $status: $(tail -n 3 "$SESSION/client.log" | tr '\n' ' ')"
    session_end
}
