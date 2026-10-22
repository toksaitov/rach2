#!/bin/sh
# password_test.sh: check that su_command.sh answers the password prompt
# without the password reaching any transcript, against fixtures/pty_secret.c
# in place of ssh, with a dummy password, real pseudo-terminals and a private
# build of runner/ptydrive.c, in seven cases of prompt and echo and two of a
# connection the machine reset or refused before the session.
#
# PTYDRIVE names a driver built elsewhere to use in place of the private
# build.
set -eu

[ "$#" -eq 0 ] || { echo 'accept-password: usage: password_test.sh' >&2; exit 2; }
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../../.." && pwd)
. "$repo/tools/etude/hold.sh"
etude_hold "$repo" accept-password "$repo/tests/system/accept/password_test.sh" "$@"
work=
child=

# Stop the running case and remove the scratch directory.
# shellcheck disable=SC2329 # The exit trap calls this function.
cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
    fi
    [ -z "$work" ] || rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-password-test.XXXXXX")
mkdir -p "$work/tests/system/accept" "$work/build/tests/system" "$work/bin" "$work/tmp"
cp "$repo/tests/system/accept/su_command.sh" "$work/tests/system/accept/"

"${CC:-cc}" -O2 -Wall -Wextra -Werror -o "$work/bin/ssh" \
    "$repo/tests/system/fixtures/pty_secret.c"
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
# after_read TEXT: write TEXT, and return once the driver has read it, so
# that what is written next comes to the driver in a read of its own.
cat > "$work/bin/after_read" <<'AFTER'
#!/bin/sh
before=$(($(wc -l < "$PTYDRIVE_READS")))
printf '%s' "$1"
until [ "$(($(wc -l < "$PTYDRIVE_READS")))" -gt "$before" ]; do
    sleep 0.01
done
AFTER
chmod +x "$work/bin/su" "$work/bin/rm" "$work/bin/after_read"

PATH="$work/bin:$PATH"
TMPDIR="$work/tmp"
RACH2_ACCEPT_PASSWORD=Q7pL9xZ2mK4vT8sN
export PATH TMPDIR RACH2_ACCEPT_PASSWORD

if [ -n "${PTYDRIVE:-}" ]; then
    cp "$PTYDRIVE" "$work/build/tests/system/ptydrive"
else
    "${CC:-cc}" -O2 -Wall -Wextra -Werror -o "$work/build/tests/system/ptydrive" \
        "$repo/tests/system/runner/ptydrive.c"
fi

# check CASE STATUS COMMAND [OUTPUT]: run su_command.sh over the fixture's
# CASE with COMMAND, and require STATUS, no byte of the password in any
# transcript, and the counts and output the case must give.
check()
{
    PTY_SECRET_CASE=$1
    expected=$2
    PTY_SECRET_AUDIT="$work/$phase.$1"
    PTYDRIVE_READS="$PTY_SECRET_AUDIT/reads"
    export PTY_SECRET_CASE PTY_SECRET_AUDIT PTYDRIVE_READS
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
        refused)
            if [ "$(wc -l < "$PTY_SECRET_AUDIT/tries")" -ne 4 ]; then
                echo "accept-password: $phase/refused: not tried exactly four times" >&2
                return 1
            fi
            if ! grep -q '^accept-su: session: .*Connection refused$' "$PTY_SECRET_AUDIT/errors"; then
                echo "accept-password: $phase/refused: the session's end was not printed" >&2
                return 1
            fi
            [ ! -s "$PTY_SECRET_AUDIT/returned" ] || return 1
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

# Run the nine cases.
cases()
{
    check good 7 "printf '%s' \"a 'quote' and a dollar \\\$\"; exit 7" \
        "a 'quote' and a dollar \$" || return 1
    # shellcheck disable=SC2016 # These variables are set by the dummy peer.
    check echo 0 'printf "%s" "$PTY_DUMMY_RECEIVED"' '[redacted]' || return 1
    # shellcheck disable=SC2016
    check split 0 'after_read "$PTY_DUMMY_FIRST"; printf "\r%s" "$PTY_DUMMY_LAST"' \
        '[redacted]' || return 1
    # shellcheck disable=SC2016
    check repeat 1 'after_read Pass; printf word:; if IFS= read -r value; then echo answered > "$PTY_SECRET_AUDIT/second"; fi' || return 1
    check joined 1 'printf safe' || return 1
    check long 0 'printf safe' safe || return 1
    check prefix 1 'printf safe' || return 1
    check reset 0 'printf safe' safe || return 1
    check refused 1 'printf safe' || return 1
    echo "accept-password: $phase: 9 checks passed"
}

phase=fixed
cases
echo "accept-password: logs: $work"
