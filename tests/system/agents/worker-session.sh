# shellcheck shell=bash
# worker-session: the project's worker loop, tools/flow/worker.sh, runs a
# session of Claude Code on each of two issues, as the test account, and
# Claude Code plays tests/system/agents/worker-session.json against the fake
# server. The row makes a git repository of the tree's tests and tools, a
# bare copy of it as the loop's `origin`, and a clone of that, whose
# worker.sh the loop runs. `gh` on the path is fakegh.py, which serves a
# board of one phase In progress, #900, and two of its tasks, #901 and #902,
# Ready, and closes an issue when a commit that closes it reaches the
# origin's master, as GitHub does. Told to by the script, the session on
# #901 commits on the issue's branch with `Closes #901` and pushes, and the
# session on #902 commits, pushes, comments and hands the issue back with
# `board.sh release`. Once the second session has started, the row writes
# the loop's stop file and waits for the loop to exit; 600 s only ends the
# wait for a loop that does not. It returns in
# /tmp/agents/results/worker-session.tar the loop's state directory, the
# board, the calls made to gh, the origin and the program's digest before
# and after the loop; worker_session_verify.py judges them on the host with
# every recorded request. The loop's environment has the settings of the
# claude-print row and the model, which worker.sh does not choose.
# bound: 900
# hosts: fakeapi.test api.anthropic.com platform.claude.com claude.com
# hosts: downloads.claude.ai raw.githubusercontent.com
# hosts: http-intake.logs.us5.datadoghq.com
# package: claude
# verifier: worker_session_verify.py

LOOP=

# Stop the loop and the session group it records, if either still runs.
row_cleanup() {
    local group
    if [ -n "$LOOP" ]; then
        kill "$LOOP" 2> /dev/null
        wait "$LOOP" 2> /dev/null
        LOOP=
    fi
    group=$(cat "$WORK/state/session" 2> /dev/null)
    [ -z "$group" ] || kill -KILL "-$group" 2> /dev/null || true
}

# board: write the board fakegh.py serves, with the origin at WORK/origin.git.
board() {
    cat > "$WORK/board.json" << EOF
{"login": "rach2-test", "origin": "$WORK/origin.git", "issues": [
 {"number": 900, "title": "tests: the worker loop on Rach2", "state": "OPEN",
  "body": "The phase of the worker-session row.", "labels": ["phase"], "assignees": [],
  "status": "In progress", "comments": []},
 {"number": 901, "title": "tests: a task the session closes", "state": "OPEN",
  "body": "Part of #900.", "labels": ["enhancement"], "assignees": [], "status": "Ready",
  "comments": []},
 {"number": 902, "title": "tests: a task the session hands back", "state": "OPEN",
  "body": "Part of #900.", "labels": ["enhancement"], "assignees": [], "status": "Ready",
  "comments": []}]}
EOF
}

# until_log TEXT SECONDS: wait until the loop's log has a line matching the
# extended regular expression TEXT, for at most SECONDS, while the loop runs.
until_log() {
    local n=0
    until grep -Eq "$1" "$WORK/state/loop.log" 2> /dev/null; do
        kill -0 "$LOOP" 2> /dev/null || { fail "the loop ended before [$1] in its log"; return 1; }
        [ "$n" -lt "$2" ] || { fail "in $2 s the loop's log had no [$1]"; return 1; }
        n=$((n + 1))
        sleep 1
    done
}

row() {
    local status bound ended
    tree || return
    mkdir -p "$WORK/home" "$WORK/tmp" "$WORK/bin" "$WORK/state" /tmp/agents/results
    output "the tree's branch" git -C tree branch -M master
    output "the origin" git clone -q --bare tree "$WORK/origin.git"
    output "the loop's clone" git clone -q "$WORK/origin.git" "$WORK/clone"
    HOME=$WORK/home git config --global user.name "Rach2 test"
    HOME=$WORK/home git config --global user.email test@invalid
    board
    printf '#!/bin/sh\nexec python3 -B %s %s "$@"\n' "$WORK/tree/tests/system/agents/fakegh.py" \
        "$WORK/board.json" > "$WORK/bin/gh"
    chmod +x "$WORK/bin/gh"
    output "the program's digest before the loop" sha256sum /opt/claude/bin/claude
    printf '%s\n' "$OUT" > "$WORK/binary.before"
    [ -z "$FAILED" ] || return

    env -i PATH="$WORK/bin:/usr/bin:/bin" HOME="$WORK/home" LC_ALL=C TERM=dumb \
        TMPDIR="$WORK/tmp" ANTHROPIC_BASE_URL=https://fakeapi.test \
        ANTHROPIC_API_KEY=rach2-offline-dummy-key "NODE_EXTRA_CA_CERTS=$CA" \
        ANTHROPIC_MODEL=claude-sonnet-4-6 CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC=1 \
        DISABLE_UPDATES=1 CLAUDE_CODE_DISABLE_OFFICIAL_MARKETPLACE_AUTOINSTALL=1 \
        CLAUDE_CODE_ENABLE_PROMPT_SUGGESTION=false \
        sh "$WORK/clone/tools/flow/worker.sh" -s "$WORK/state" > "$WORK/loop.out" 2>&1 &
    LOOP=$!
    until_log ' session [0-9-]* on #902 starts$' 600 || return
    touch "$WORK/state/stop"
    sleep 600 &
    bound=$!
    wait -n -p ended "$LOOP" "$bound"
    status=$?
    kill "$bound" 2> /dev/null
    wait "$bound" 2> /dev/null
    [ "$ended" = "$LOOP" ] || { fail "the loop did not stop in 600 s after its stop file"; return; }
    LOOP=
    same "the loop's status" "$status" 0
    output "the program's digest after the loop" sha256sum /opt/claude/bin/claude
    printf '%s\n' "$OUT" > "$WORK/binary.after"
    output "the row's archive" tar -cf "/tmp/agents/results/$ROW.tar" -C "$WORK" \
        state board.json board.json.calls origin.git loop.out binary.before binary.after
}
