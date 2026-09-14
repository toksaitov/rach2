#!/bin/sh
# Text commands for acceptance, authenticated by the owner through su.
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
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

quote()
{
    printf "'"
    printf '%s' "$1" | sed "s/'/'\\\\''/g"
    printf "'"
}

token="RACH2_ACCEPT_$(basename "$work")"
command="{ printf '\\n%s\\n' '$token-begin'; /bin/sh -c $(quote "$6") 2>/dev/null; accept_status=\$?; printf '\\n%s %s\\n' '$token-end' \"\$accept_status\"; } | cat"
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
if ! grep -qx 'PROMPTED=1' "$work/text" || grep -qx TIMEDOUT "$work/text"; then
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
exit "$status"
