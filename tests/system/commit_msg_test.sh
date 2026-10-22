#!/bin/sh
#
# commit_msg_test.sh: the commit-msg hook refuses attribution lines and
# nothing else.
#
# Each row gives a private copy of tools/git-hooks/commit-msg one message and
# checks only attribution: status 0 for a message without it, including one
# that mentions trailers and one with the diff `git commit -v` adds, whose
# lines quote attribution after +, - and a space, and 1, with the refusal on
# stderr, for a message with it, the emoji footer Claude Code writes among
# them. The copy is run as git runs it, through its own #! line, so a hook
# that has lost its executable bit fails every row.
#
# Usage: sh tests/system/commit_msg_test.sh
#
set -u

[ "$#" -eq 0 ] || { echo 'commit-msg-test: usage: commit_msg_test.sh' >&2; exit 2; }

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" commit-msg-test "$repo/tests/system/commit_msg_test.sh" "$@"
work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-commit-msg.XXXXXX") || exit 1
hook=$work/commit-msg
cp "$repo/tools/git-hooks/commit-msg" "$hook" || exit 1

# row NAME STATUS FORMAT: run the copy on the message printf makes of FORMAT.
row() {
    # shellcheck disable=SC2059
    printf "$3" >"$work/msg"
    "$hook" "$work/msg" >/dev/null 2>"$work/err"
    got=$?
    if [ "$got" -ne "$2" ]; then
        echo "commit-msg-test: fail: $1: status $got, expected $2" >&2
        return 1
    fi
    if [ "$2" -eq 1 ] && ! grep -q '^commit-msg: refused: attribution on line ' "$work/err"; then
        echo "commit-msg-test: fail: $1: no refusal on stderr" >&2
        return 1
    fi
    return 0
}

# rows: run every row and print the names of the rows that failed.
rows() {
    failed=
    row plain 0 'Describe the three-partition disk in the demo Readme\n' ||
        failed="$failed plain"
    row body 0 'Move the sync detail to the tools Readme\n\nThe policy says what to do.\n' ||
        failed="$failed body"
    row mention 0 'Refuse a Co-Authored-By trailer\n\nA Claude-Session link is refused too.\nRemove noreply@anthropic.com from commit messages.\nReject the generated with Claude Code footer.\n' ||
        failed="$failed mention"
    row verbose 0 'Subject\n\n# ------------------------ >8 ------------------------\n# Do not modify or remove the line above.\ndiff --git a/a b/a\n--- a/a\n+++ b/a\n@@ -1,2 +1,2 @@\n Co-Authored-By: Someone <someone@example.com>\n-Reviewed-by: Claude <noreply@anthropic.com>\n+* Generated with [Claude Code](https://claude.com/claude-code)\n' ||
        failed="$failed verbose"
    row coauthor 1 'Subject\n\nCo-Authored-By: Someone <someone@example.com>\n' ||
        failed="$failed coauthor"
    row session 1 'Subject\n\nClaude-Session: https://claude.ai/code/session_x\n' ||
        failed="$failed session"
    row signoff 1 'Subject\n\nSigned-off-by: Someone <someone@example.com>\n' ||
        failed="$failed signoff"
    row generated 1 'Subject\n\nGenerated with [Claude Code](https://claude.com/claude-code)\n' ||
        failed="$failed generated"
    row robot 1 'Subject\n\n\360\237\244\226 Generated with [Claude Code](https://claude.com/claude-code)\n' ||
        failed="$failed robot"
    row noreply 1 'Subject\n\nReviewed-by: Claude <noreply@anthropic.com>\n' ||
        failed="$failed noreply"
    echo "$failed"
}

failed=$(rows)
if [ -n "$failed" ]; then
    echo "commit-msg-test: result: fail:$failed" >&2
    exit 1
fi
echo 'commit-msg-test: result: pass: 10 rows'
