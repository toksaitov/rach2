#!/bin/sh
# Use only a dummy secret, real PTYs and private compiled copies of the driver.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../../.." && pwd)
if [ "${1:-}" != --texts ]; then
    . "$repo/tests/system/runlock.sh"
    runlock_hold "$repo" accept-password "$repo/tests/system/accept/password_test.sh" "$@"
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-password-test.XXXXXX")
mkdir -p "$work/tests/system/accept" "$work/build/tests/system" "$work/bin" "$work/tmp"
cp "$repo/tests/system/accept/su_command.sh" "$work/tests/system/accept/"
cp "$repo/tests/system/runner/ptydrive.c" "$work/original.c"
cp "$work/original.c" "$work/ptydrive.c"
child=

# shellcheck disable=SC2329 # The exit trap restores the private source.
cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
    fi
    cp "$work/original.c" "$work/ptydrive.c"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

"${CC:-cc}" -O2 -Wall -Wextra -Werror -o "$work/bin/ssh" \
    "$repo/tests/system/fixtures/pty_secret.c"
"${CC:-cc}" -O2 -Wall -Wextra -Werror -o "$work/build/tests/system/runutil" \
    "$repo/tests/system/runner/runutil.c"
cat > "$work/bin/su" <<'SU'
#!/bin/sh
[ "$1" = -c ] || exit 2
exec /bin/sh -c "$2"
SU
cat > "$work/bin/rm" <<'RM'
#!/bin/sh
# Save the helper's actual transcripts immediately before its cleanup.
for directory do :; done
case "$directory" in
    */rach2-accept-su.*)
        for name in raw text; do
            if [ -f "$directory/$name" ]; then
                cp "$directory/$name" "$PTY_SECRET_AUDIT/$name" || exit 1
            fi
        done
        ;;
esac
exec /bin/rm "$@"
RM
chmod +x "$work/bin/su" "$work/bin/rm"

PATH="$work/bin:$PATH"
TMPDIR="$work/tmp"
RACH2_ACCEPT_PASSWORD=Q7pL9xZ2mK4vT8sN
export PATH TMPDIR RACH2_ACCEPT_PASSWORD
unset RACH2_DEADLINES RACH2_DEADLINE_FACTOR


# password_edit MUTATION: $work/ptydrive.c is the saved driver with MUTATION in
# it, and `diagnostic` the words its failing case prints; fail when the edit
# changes nothing, since its text has left the driver.
password_edit()
{
    case "$1" in
        repeat)
            sed 's/if (asked > 1)/if (asked > 100)/' "$work/original.c" > "$work/ptydrive.c"
            diagnostic='the second prompt was answered'
            ;;
        redact)
            awk '
                /static void secret_byte/ { function_seen = 1 }
                { print }
                function_seen && /^\{/ {
                    print "    putchar(byte); return;"
                    function_seen = 0
                }
            ' "$work/original.c" > "$work/ptydrive.c"
            diagnostic='credential bytes reached raw'
            ;;
        count)
            sed 's/asked, answered);/(asked != 0), answered);/' "$work/original.c" > "$work/ptydrive.c"
            diagnostic='prompt or answer count is wrong'
            ;;
    esac
    if cmp -s "$work/original.c" "$work/ptydrive.c"; then
        echo "accept-password: the $1 fault is not in ptydrive.c" >&2
        exit 1
    fi
}

if [ "${1:-}" = --texts ]; then
    for mutation in repeat redact count; do
        password_edit "$mutation"
    done
    cp "$work/original.c" "$work/ptydrive.c"
    trap - EXIT
    rm -rf "$work"
    echo 'accept-password: texts: every fault is once in its source'
    exit 0
fi

compile()
{
    "${CC:-cc}" -O2 -Wall -Wextra -Werror -o "$work/build/tests/system/ptydrive" "$work/ptydrive.c"
    cp "$work/build/tests/system/ptydrive" "$work/$phase.ptydrive"
}

check()
{
    PTY_SECRET_CASE=$1
    expected=$2
    PTY_SECRET_AUDIT="$work/$phase.$1"
    export PTY_SECRET_CASE PTY_SECRET_AUDIT
    mkdir "$PTY_SECRET_AUDIT"
    status=0
    sh "$work/tests/system/accept/su_command.sh" key fixture 22 test known "$3" \
        > "$PTY_SECRET_AUDIT/returned" 2> "$PTY_SECRET_AUDIT/errors" &
    child=$!
    wait "$child" || status=$?
    child=
    for name in raw text returned errors; do
        if [ -f "$PTY_SECRET_AUDIT/$name" ] &&
            grep -Eq 'Q7pL9xZ2|mK4vT8sN' "$PTY_SECRET_AUDIT/$name"; then
            echo "accept-password: $phase/$1: credential bytes reached $name" >&2
            return 1
        fi
    done
    if [ "$1" = repeat ] && [ -e "$PTY_SECRET_AUDIT/second" ]; then
        echo "accept-password: $phase/repeat: the second prompt was answered" >&2
        return 1
    fi
    if [ "$status" -ne "$expected" ]; then
        echo "accept-password: $phase/$1: status $status, expected $expected" >&2
        return 1
    fi
    case "$1" in
        repeat|joined)
            if ! grep -qx 'PROMPTED=2' "$PTY_SECRET_AUDIT/raw" ||
                ! grep -qx 'ANSWERED=1' "$PTY_SECRET_AUDIT/raw"; then
                echo "accept-password: $phase/$1: prompt or answer count is wrong" >&2
                return 1
            fi
            [ ! -s "$PTY_SECRET_AUDIT/returned" ] || return 1
            ;;
        prefix)
            grep -Fq '[redacted]' "$PTY_SECRET_AUDIT/raw" || return 1
            ;;
        *)
            grep -qx 'one answer' "$PTY_SECRET_AUDIT/first" || return 1
            grep -qx 'PROMPTED=1' "$PTY_SECRET_AUDIT/raw" || return 1
            grep -qx 'ANSWERED=1' "$PTY_SECRET_AUDIT/raw" || return 1
            [ -s "$PTY_SECRET_AUDIT/text" ] || return 1
            [ "$(cat "$PTY_SECRET_AUDIT/returned")" = "$4" ] || {
                echo "accept-password: $phase/$1: returned text changed" >&2
                return 1
            }
            ;;
    esac
}

cases()
{
    check good 7 "printf '%s' \"a 'quote' and a dollar \\\$\"; exit 7" \
        "a 'quote' and a dollar \$" || return 1
    # shellcheck disable=SC2016 # These variables are set by the dummy peer.
    check echo 0 'printf "%s" "$PTY_DUMMY_RECEIVED"' '[redacted]' || return 1
    # shellcheck disable=SC2016
    check split 0 'printf "%s" "$PTY_DUMMY_FIRST"; sleep 0.03; printf "\r%s" "$PTY_DUMMY_LAST"' \
        '[redacted]' || return 1
    # shellcheck disable=SC2016
    check repeat 1 'printf Pass; sleep 0.03; printf word:; if IFS= read -r value; then echo answered > "$PTY_SECRET_AUDIT/second"; fi' || return 1
    check joined 1 'printf safe' || return 1
    check long 0 'printf safe' safe || return 1
    check prefix 1 'printf safe' || return 1
    echo "accept-password: $phase: 7 checks passed"
}

phase=fixed
compile
cases
case "${1:-}" in
    '') ;;
    --mutate)
        for mutation in repeat redact count; do
            password_edit "$mutation"
            phase=$mutation
            compile
            if cases > "$work/$mutation.log" 2>&1; then
                echo "accept-password: the $mutation mutation was not detected" >&2
                exit 1
            fi
            grep -Fq "$diagnostic" "$work/$mutation.log" || {
                echo "accept-password: the $mutation mutation failed for another reason" >&2
                exit 1
            }
            echo "accept-password: mutation: $mutation detected"
            cp "$work/original.c" "$work/ptydrive.c"
            phase=restored-$mutation
            compile
            cmp -s "$work/fixed.ptydrive" "$work/$phase.ptydrive"
            cases
        done
        ;;
    *) echo 'accept-password: usage: [--mutate | --texts]' >&2; exit 2 ;;
esac
echo "accept-password: logs: $work"
