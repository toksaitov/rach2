#!/bin/sh
#
# record.sh: record Claude Code's output on an 80x25 terminal with util-linux's
# `script`, under TERM=rach2 and TERM=xterm-256color, into this directory.
#
# Usage: sh tests/fixtures/agents/record.sh
#
# Each recording starts `claude` in an empty directory; answers the trust
# menu with Down and Enter and the question on the imports of the
# repository's CLAUDE.md with Enter, keys that reach an empty prompt and do
# nothing when Claude Code asks neither; asks for a reply with emoji, under
# the spinner; opens the model menu through the menu of commands and closes
# it; and exits. The keys are written on a timer, so a
# slow reply may cut a recording short: look at it before committing it.
# rach2's terminfo entry is compiled from vendor/gnu/rach2.ti into scratch.

set -eu

top=$(cd "$(dirname "$0")/../../.." && pwd)
out="$top/tests/fixtures/agents"
mkdir -p "$top/build/tmp"
scratch=$(mktemp -d "$top/build/tmp/agents-record.XXXXXX")
trap 'rm -rf "$scratch"' EXIT INT TERM HUP

tic -x -o "$scratch/terminfo" "$top/vendor/gnu/rach2.ti"

# keys: the bytes the recording types, each after a pause in seconds.
keys() {
    sleep 8
    printf '\033[B'
    sleep 1
    printf '\r'
    sleep 5
    printf '\r'
    sleep 8
    printf 'Reply in one line with a check mark, a rocket emoji and the word '
    printf '\344\275\240\345\245\275, and nothing else.'
    sleep 1
    printf '\r'
    sleep 40
    printf '/model'
    sleep 2
    printf '\r'
    sleep 4
    printf '\033'
    sleep 3
    printf '/exit'
    sleep 2
    printf '\r'
    sleep 5
}

for term in rach2 xterm-256color; do
    work="$scratch/work-$term"
    mkdir -p "$work"
    log="$out/claude-$term.typescript"
    keys | (
        cd "$work" &&
            env -u TMUX -u TMUX_PANE -u CLAUDECODE -u CLAUDE_CODE_ENTRYPOINT -u CLAUDE_CODE_SESSION_ID \
                -u CLAUDE_CODE_CHILD_SESSION -u CLAUDE_PID -u AI_AGENT \
                -u CLAUDE_CODE_MESSAGING_SOCKET -u CLAUDE_CODE_MESSAGING_TOKEN \
                TERMINFO="$scratch/terminfo" TERM="$term" COLUMNS=80 LINES=25 \
                script -q -e -E never -c 'stty cols 80 rows 25; exec claude' "$log"
    ) >/dev/null || :
    echo "record: wrote: $log"
done
