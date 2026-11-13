# shellcheck shell=bash
# worker-session: the project's worker loop, tools/flow/worker.sh, runs a
# session of Claude Code on each of two tasks, as the test account, and
# Claude Code plays tests/system/agents/worker-session.json against the fake
# server. The row makes a git repository of the agent fixtures and flow
# tools, with the branches master and milestone/01-worker at its one commit,
# a bare copy of it as the loop's `origin`, and a clone of that, whose
# worker.sh the loop runs. `gh` on the path is fakegh.py, which serves a
# board of one open milestone, number 1, whose description names the branch
# milestone/01-worker, with two tasks of it Ready, #901 and #902, and #903, a
# subtask of #901, Ready and first on the board, which `board.sh next` must
# not list. Told to by the script, the session on #901 commits on task/901
# and pushes it, closes #903 with a comment that names the commit, rebases
# onto the milestone's branch, pushes HEAD to it, closes #901 with a comment
# that names the commit, and deletes task/901 here and on the origin; the
# session on #902 commits on task/902, pushes it, comments and hands the task
# back with `board.sh release`. Once the second session has started, the row
# writes the loop's stop file and waits for the loop to exit. It returns in
# /tmp/agents/results/worker-session.tar the loop's state directory, the
# board, the calls made to gh, the origin and the program's digest before
# and after the loop; the case judges them on the host with every recorded
# request. The loop's environment has the settings of the claude-print row
# and the model, which worker.sh does not choose.
# hosts: fakeapi.test api.anthropic.com platform.claude.com claude.com
# hosts: downloads.claude.ai raw.githubusercontent.com
# hosts: http-intake.logs.us5.datadoghq.com
# package: claude
# script: agents/worker-session.json

LOOP=

# Stop the loop and its session, if either still runs, as stop.sh does, by
# their files and with no signal: STATE/stop ends the loop once its session
# has, and STATE/session.stop has the session's supervisor stop its group.
# The row's files go only once session.pl -q, the one decider, says that no
# session of the loop may still run.
row_cleanup() {
    [ -d "$WORK/state" ] && [ -f "$WORK/clone/tools/flow/session.pl" ] || return 0
    touch "$WORK/state/stop" "$WORK/state/session.stop"
    if [ -n "$LOOP" ]; then
        wait "$LOOP" 2> /dev/null
        LOOP=
    fi
    until [ "$(perl "$WORK/clone/tools/flow/session.pl" -q "$WORK/state/session")" = ended ]; do
        sleep 1
    done
}

# board: write the board fakegh.py serves, with the origin at WORK/origin.git.
board() {
    cat > "$WORK/board.json" << EOF
{"login": "rach2-test", "origin": "$WORK/origin.git", "milestones": [
 {"number": 1, "title": "The worker loop on Rach2", "state": "OPEN",
  "description": "The milestone of the worker-session row.\n\nBranch: milestone/01-worker"}],
 "issues": [
 {"number": 903, "title": "tests: a subtask the session closes", "state": "OPEN",
  "labels": [], "assignees": [], "status": "Ready", "milestone": 1, "parent": 901,
  "comments": []},
 {"number": 901, "title": "tests: a task the session merges", "state": "OPEN",
  "labels": ["enhancement"], "assignees": [], "status": "Ready", "milestone": 1,
  "parent": null, "comments": []},
 {"number": 902, "title": "tests: a task the session hands back", "state": "OPEN",
  "labels": ["enhancement"], "assignees": [], "status": "Ready", "milestone": 1,
  "parent": null, "comments": []}]}
EOF
}

# until_log TEXT: wait until the loop's log has a line matching the extended
# regular expression TEXT, while the loop runs. The loop's state is read before
# the log, so that a line the loop wrote just before it ended is seen.
until_log() {
    local ended
    while :; do
        ended=0
        kill -0 "$LOOP" 2> /dev/null || ended=1
        grep -Eq "$1" "$WORK/state/loop.log" 2> /dev/null && return 0
        [ "$ended" -eq 0 ] || { fail "the loop ended before [$1] in its log"; return 1; }
        sleep 1
    done
}

row() {
    local status
    tree || return
    mkdir -p "$WORK/home" "$WORK/tmp" "$WORK/bin" "$WORK/state" /tmp/agents/results
    output "the tree's branch" git -C tree branch -M master
    output "the milestone's branch" git -C tree branch milestone/01-worker
    output "the origin" git clone -q --bare tree "$WORK/origin.git"
    output "the loop's clone" git clone -q "$WORK/origin.git" "$WORK/clone"
    HOME=$WORK/home git config --global user.name "Rach2 test"
    HOME=$WORK/home git config --global user.email test@invalid
    board
    printf '#!/bin/sh\nexec python3 -B %s %s "$@"\n' "$WORK/tree/tests/system/agents/fakegh.py" \
        "$WORK/board.json" > "$WORK/bin/gh"
    chmod +x "$WORK/bin/gh"
    output "the tasks on the board" env PATH="$WORK/bin:$PATH" sh "$WORK/clone/tools/flow/board.sh" next
    same "the tasks on the board" "$OUT" \
        "$(printf '901\ttests: a task the session merges\n902\ttests: a task the session hands back')"
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
    until_log ' session [0-9-]* on #902 starts$' || return
    touch "$WORK/state/stop"
    wait "$LOOP"
    status=$?
    LOOP=
    same "the loop's status" "$status" 0
    output "the program's digest after the loop" sha256sum /opt/claude/bin/claude
    printf '%s\n' "$OUT" > "$WORK/binary.after"
    output "the row's archive" tar -cf "/tmp/agents/results/$ROW.tar" -C "$WORK" \
        state board.json board.json.calls origin.git loop.out binary.before binary.after
}
