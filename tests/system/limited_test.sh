#!/bin/sh
#
# limited_test.sh: lib.sh's `limited` takes a usage limit only from the line
# that reports it, never from a session's own text that quotes it.
#
# Each row writes a session's output and errors in the forms Claude Code's
# stream-json and Codex's --json give them, calls `limited` from a private
# copy of tools/flow/lib.sh, and checks its status and the message it prints.
# The limits are Claude Code's result line with "is_error":true, after its
# synthetic message or before a trailing line, and Codex's message in its
# errors or in a turn.failed event; the others end normally with the limit's
# text in the final message, in a failed tool's output, or earlier in the
# output. With --mutate each fault below is put into the copy and must fail
# exactly its expected rows; the copy is restored before the rows run again
# at the end. --texts puts each fault into the copy and requires only that it
# changes the file, and runs no row.
#
# Usage: sh tests/system/limited_test.sh [--mutate | --texts]
#
set -u

case ${1:-} in
    '') mode=check ;;
    --mutate) mode=mutate ;;
    --texts) mode=texts ;;
    *) echo 'limited-test: usage: limited_test.sh [--mutate | --texts]' >&2; exit 2 ;;
esac
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/limited-test.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
lib=$work/lib.sh

# fault NAME: put fault NAME into the private copy of lib.sh, or the original
# for `none`, and fail when a fault leaves the copy unchanged.
fault()
{
    # shellcheck disable=SC2016 # the dollar is in the text sed matches
    case $1 in
        none) cat ;;
        any-line) sed 's/^        grep -E .*"is_error":true.*$/        cat/' ;;
        no-err) sed 's/^        tail -n 2 "\$2"; }/        :; }/' ;;
    esac < "$repo/tools/flow/lib.sh" > "$lib"
    [ "$1" = none ] || ! cmp -s "$repo/tools/flow/lib.sh" "$lib"
}

dot=$(printf '\302\267')
msg="You've hit your session limit $dot resets 9:20am (Asia/Almaty)"
codex="You've hit your usage limit. Try again at 3:05 PM."
quote="The loop logged ${msg} after a session that ended 0."
ok='{"type":"result","subtype":"success","is_error":false,"terminal_reason":"completed"'
bad='{"type":"result","subtype":"success","is_error":true,"terminal_reason":"api_error"'

# write NAME: write the output and errors of session NAME to work/out and
# work/err.
write()
{
    : > "$work/err"
    case $1 in
        claude-limit)
            printf '%s\n' \
                "{\"type\":\"assistant\",\"message\":{\"model\":\"<synthetic>\",\"content\":[{\"type\":\"text\",\"text\":\"$msg\"}]},\"error\":\"rate_limit\"}" \
                "$bad,\"api_error\":\"usage_limit_reached\",\"result\":\"$msg\"}" ;;
        claude-limit-trailing)
            printf '%s\n' '{"type":"system","subtype":"init"}' \
                "$bad,\"api_error\":\"usage_limit_reached\",\"result\":\"$msg\"}" \
                '{"type":"command_lifecycle","state":"cancelled"}' ;;
        codex-err)
            printf '%s\n' '{"type":"thread.started"}' '{"type":"turn.started"}'
            printf 'ERROR: %s\n' "$codex" > "$work/err" ;;
        codex-event)
            printf '%s\n' '{"type":"turn.started"}' \
                "{\"type\":\"turn.failed\",\"error\":{\"message\":\"$codex\"}}" ;;
        claude-quote)
            printf '%s\n' \
                "{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"$quote\"}]}}" \
                "$ok,\"result\":\"$quote\"}" ;;
        claude-quote-keys)
            printf '%s\n' \
                "$ok,\"result\":\"The line had \\\"is_error\\\":true and \\\"type\\\":\\\"error\\\": $quote\"}" ;;
        claude-tool)
            printf '%s\n' \
                "{\"type\":\"user\",\"message\":{\"content\":[{\"type\":\"tool_result\",\"is_error\":true,\"content\":\"$quote\"}]}}" \
                '{"type":"assistant","message":{"content":[{"type":"text","text":"Reading the log."}]}}' ;;
        codex-quote)
            printf '%s\n' \
                "{\"type\":\"item.completed\",\"item\":{\"type\":\"agent_message\",\"text\":\"$quote\"}}" \
                '{"type":"turn.completed"}' ;;
        claude-earlier)
            printf '%s\n' "$bad,\"result\":\"$msg\"}" \
                '{"type":"assistant","message":{"content":[{"type":"text","text":"Done."}]}}' \
                "$ok,\"result\":\"Done.\"}" ;;
        empty) : > "$work/out" ;;
    esac > "$work/out"
}

# rows: run every row against the copy, print `limited-test: failed: NAME`
# for each that fails, and fail when any did.
rows()
{
    failed=0
    for row in claude-limit claude-limit-trailing codex-err codex-event \
        claude-quote claude-quote-keys claude-tool codex-quote claude-earlier empty; do
        write "$row"
        result=0
        # shellcheck source=tools/flow/lib.sh
        why=$(. "$lib" && limited "$work/out" "$work/err") || result=$?
        case $row in
            claude-*limit*) want=0 expect="hit your session limit $dot resets 9:20am (Asia/Almaty)" ;;
            codex-err|codex-event) want=0 expect="hit your usage limit. Try again at 3:05 PM." ;;
            *) want=1 expect= ;;
        esac
        if [ "$result" -ne "$want" ] || [ "$why" != "$expect" ]; then
            echo "limited-test: failed: $row"
            failed=1
        fi
    done
    return "$failed"
}

# expect FAULT ROWS: with FAULT in the copy, require exactly ROWS to fail.
expect()
{
    fault "$1" || { echo "limited-test: fault $1 changes nothing" >&2; return 1; }
    rows > "$work/mutated.out" && { echo "limited-test: fault passed: $1" >&2; return 1; }
    got=$(sed 's/^limited-test: failed: //' "$work/mutated.out" | tr '\n' ' ')
    if [ "$got" != "$2 " ]; then
        echo "limited-test: fault $1 failed rows: $got; expected: $2" >&2
        return 1
    fi
}

if [ "$mode" = texts ]; then
    fault any-line && fault no-err && exit 0
    echo 'limited-test: a fault changes nothing in tools/flow/lib.sh' >&2
    exit 1
fi
fault none
rows >&2 || exit 1
if [ "$mode" = mutate ]; then
    expect any-line 'claude-quote claude-quote-keys claude-tool codex-quote' || exit 1
    expect no-err codex-err || exit 1
    fault none
    rows >&2 || exit 1
    echo 'limited-test: result: pass: both faults caught and restored'
fi
echo 'limited-test: result: pass: 10 rows'
