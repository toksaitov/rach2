#!/bin/sh
# su_command.sh KEY HOST PORT USER KNOWN-HOSTS COMMAND: run COMMAND as root
# on an acceptance machine, through su in an SSH session as USER, and print
# what it printed and exit with its status.
#
# runner/ptydrive gives ssh a pseudo-terminal and answers su's one password
# prompt from $RACH2_ACCEPT_PASSWORD, which never appears in an argument or a
# transcript. COMMAND's output is printed from between two markers, so that
# su's prompt and the session's other output are left out. The status is 1
# when the session fails, the prompt was not answered exactly once, or the
# markers are missing.
set -eu

if [ "$#" -ne 6 ] || [ -z "${RACH2_ACCEPT_PASSWORD:-}" ]; then
    echo 'accept-su: usage: KEY HOST PORT USER KNOWN-HOSTS COMMAND; set RACH2_ACCEPT_PASSWORD' >&2
    exit 2
fi
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../../.." && pwd)
driver="$repo/build/tests/system/ptydrive"
runner="$repo/build/tests/system/runutil"
if [ ! -x "$driver" ] || [ ! -x "$runner" ]; then
    echo 'accept-su: build/tests/system/ptydrive and runutil must be built' >&2
    exit 2
fi
umask 077
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-accept-su.XXXXXX")
child=
# shellcheck disable=SC2329 # The exit trap calls this function.
cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
    fi
    rm -rf "$work"
}
# bash 3.2, the Mac's sh, gives an EXIT trap a status of 0 when `set -u`
# stops a script that also has `set -e`, so a run that reached no success
# exit fails here.
finished=
trap 'status=$?; cleanup; [ -n "$finished" ] || [ "$status" -ne 0 ] || status=1; exit "$status"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# Print $1 quoted for a shell.
quote()
{
    printf "'"
    printf '%s' "$1" | sed "s/'/'\\\\''/g"
    printf "'"
}

token="RACH2_ACCEPT_$(basename "$work")"
command="{ printf '\\n%s\\n' '$token-begin'; /bin/sh -c $(quote "$6") 2>/dev/null; accept_status=\$?; printf '\\n%s %s\\n' '$token-end' \"\$accept_status\"; } | cat"
# The driver filters credential bytes before either transcript is written.
"$runner" watch accept-su 100 "$work/jobs" \
    "$driver" --rows 40 --cols 120 --timeout 90 \
    --password-env RACH2_ACCEPT_PASSWORD ssh -tt -i "$1" -p "$3" \
    -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$5" \
    -o ConnectTimeout=10 -o BatchMode=yes "$4@$2" \
    "su -c $(quote "$command")" > "$work/raw" 2>&1 &
child=$!
rc=0
wait "$child" || rc=$?
child=
if [ "$rc" -ne 0 ]; then
    echo 'accept-su: the terminal command could not run' >&2
    exit 1
fi
tr -d '\r' < "$work/raw" > "$work/text"
if ! awk '
    /^PROMPTED=/ { prompts++; if ($0 != "PROMPTED=1") bad = 1 }
    /^ANSWERED=/ { answers++; if ($0 != "ANSWERED=1") bad = 1 }
    END { exit bad || prompts != 1 || answers != 1 }
' "$work/text" || grep -qx TIMEDOUT "$work/text"; then
    echo 'accept-su: password authentication did not complete' >&2
    exit 1
fi
status=$(awk -v token="$token-end" '$1 == token { print $2 }' "$work/text")
case "$status" in
    ''|*[!0-9]*) echo 'accept-su: the command did not report its status' >&2; exit 1 ;;
esac
[ "$status" -le 255 ] || exit 1
awk -v begin="$token-begin" -v end="$token-end" '
    $0 == begin { inside = 1; starts++; next }
    inside && $1 == end {
        if (have && pending != "") printf "%s", pending
        inside = 0; ends++; next
    }
    inside { if (have) print pending; pending = $0; have = 1 }
    END { exit starts != 1 || ends != 1 || inside }
' "$work/text" || exit 1
finished=1
exit "$status"
