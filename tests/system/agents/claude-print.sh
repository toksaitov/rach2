# shellcheck shell=bash
# claude-print: Claude Code plays the exchange of
# tests/system/fakeapi/claude-session.json with `claude -p`, as the test
# account. Asked to, it reads the fixture with Read, runs a command with
# Bash, edits the file with Edit and commits it with git through Bash, and
# the fake server checks each tool result before it answers. The row makes
# an empty home and the fixture's repository, and returns in
# /tmp/agents/results/claude-print.tar the repository, the program's digest
# before and after the session, the free frames sampled each second and the
# time of each step; claude_session_verify.py judges them on the host with
# every recorded request. The settings are those of claude_test.py's client
# on Linux, with the fake server as the gateway ANTHROPIC_BASE_URL names. Every
# name Claude Code reaches as shipped is the fake server's too, so a request
# to any of them that the script does not have fails the row, and the
# machine has no route anywhere else. The row's script has the last answer
# held, and while the program waits for it the row requires the process to
# run Anthropic's file with nothing preloaded.
# hosts: fakeapi.test api.anthropic.com platform.claude.com claude.com
# hosts: downloads.claude.ai raw.githubusercontent.com
# hosts: http-intake.logs.us5.datadoghq.com
# script: fakeapi/claude-session.json
# script-filter: claude_session_verify.py
# package: claude
# verifier: claude_session_verify.py

PROMPT="Read fixture.txt, check it with the shell, change answer=before to answer=after, and commit the change."

# session_start MODE: make the session's home, scratch and the fixture's
# repository in SESSION, WORK/session, record MODE, the repository's path,
# the program's digest and the start, and sample the free frames until
# session_end.
session_start() {
    SESSION=$WORK/session
    mkdir -p "$SESSION/home" "$SESSION/tmp" "$SESSION/repo" /tmp/agents/results
    printf 'rach2 fixture 489\nanswer=before\n' > "$SESSION/repo/fixture.txt"
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
    output "the program's digest before the session" sha256sum /opt/claude/bin/claude
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
    output "the program's digest after the session" sha256sum /opt/claude/bin/claude
    printf '%s\n' "$OUT" > "$SESSION/binary.after"
    output "the session's archive" tar -cf "/tmp/agents/results/$ROW.tar" -C "$SESSION" \
        repo mode at binary.before binary.after memory seconds steps
}

# session_env TERM: print the environment of the session, a variable to a
# line: the settings of claude_test.py's client, with TERM.
session_env() {
    printf '%s\n' PATH=/usr/bin:/bin "HOME=$SESSION/home" LC_ALL=C "TERM=$1" \
        "TMPDIR=$SESSION/tmp" ANTHROPIC_BASE_URL=https://fakeapi.test \
        ANTHROPIC_API_KEY=rach2-offline-dummy-key "NODE_EXTRA_CA_CERTS=$CA" \
        CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC=1 DISABLE_UPDATES=1 \
        CLAUDE_CODE_DISABLE_OFFICIAL_MARKETPLACE_AUTOINSTALL=1 \
        CLAUDE_CODE_ENABLE_PROMPT_SUGGESTION=false
}

# Stop the sampler and the private tmux server on every exit path.
row_cleanup() {
    if [ -n "${SAMPLER:-}" ]; then
        kill "$SAMPLER" 2> /dev/null
        wait "$SAMPLER" 2> /dev/null
        SAMPLER=
    fi
    tmux -L claude kill-server 2> /dev/null || true
}

row() {
    local pid status env
    session_start print || return
    mapfile -t env < <(session_env dumb)
    env -i "${env[@]}" claude -p "$PROMPT" \
        --model claude-sonnet-4-6 --permission-mode bypassPermissions --tools Read,Bash,Edit \
        --strict-mcp-config --mcp-config '{"mcpServers":{}}' --setting-sources user \
        --settings '{"sandbox":{"enabled":false}}' --no-session-persistence --max-turns 8 \
        > "$SESSION/client.log" 2>&1 &
    pid=$!
    if held "claude -p" "$pid"; then
        step "the last answer held"
        unmodified "claude -p" "$pid" /opt/claude/bin/claude \
            /lib64/ld-linux-x86-64.so.2 /lib/libc.so.6 /lib/libm.so.6 /lib/libpthread.so.0 \
            /lib/libdl.so.2 /lib/librt.so.1
        release "$pid"
    fi
    wait "$pid"
    status=$?
    step "the session ended"
    [ "$status" = 0 ] ||
        fail "claude -p: status $status: $(tail -n 3 "$SESSION/client.log" | tr '\n' ' ')"
    session_end
}
