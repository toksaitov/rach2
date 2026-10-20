#!/bin/sh
# space_test.sh: check disk admission against command fixtures, including a
# Windows query that times out or returns no usable reading.
#
# --mutate permits an unknown reading, ignores a failed query, or permits a
# missing PowerShell in a private copy; each fault must fail its own row.
# --texts checks only that the faults change the copy.
set -eu
case ${1:-} in
    '') mode=check ;;
    --mutate) mode=mutate ;;
    --texts) mode=texts ;;
    *) echo 'space-test: usage: space_test.sh [--mutate | --texts]' >&2; exit 2 ;;
esac
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-space-test.XXXXXX")
copy=$work/space.sh

# fault NAME: copy the source with one fault, or unchanged for none.
fault()
{
    case $1 in
        none) cat ;;
        unknown-success) sed '/no valid free-space reading/{n;s/exit 1/exit 0/;}' ;;
        status-ignored) sed 's/^status=\$?/status=0/' ;;
        missing-success) sed '/powershell.exe is missing/{n;s/exit 1/exit 0/;}' ;;
    esac < "$repo/tools/setup/space.sh" > "$copy"
    [ "$1" = none ] || ! cmp -s "$repo/tools/setup/space.sh" "$copy"
}
if [ "$mode" = texts ]; then
    for f in unknown-success status-ignored missing-success; do fault "$f"; done
    exit 0
fi
mkdir "$work/bin" "$work/windows"
for c in awk tr; do ln -s "$(command -v "$c")" "$work/bin/$c"; done
cat > "$work/bin/df" <<'FIXTURE'
#!/bin/sh
printf 'Filesystem 1024-blocks Used Available Capacity Mounted on\n'
[ "$LINUX_FREE" != unknown ] || exit 1
printf 'fixture 999999999 0 %s 0%% /\n' "$LINUX_FREE"
FIXTURE
cat > "$work/bin/timeout" <<'FIXTURE'
#!/bin/sh
[ "$1 $2 $3" = '-k 5 60' ] || exit 91
shift 3
if [ "$QUERY" = timeout ]; then exit 124; fi
exec "$@"
FIXTURE
cat > "$work/windows/powershell.exe" <<'FIXTURE'
#!/bin/sh
[ "$PWD" = / ] && [ "$RACH2_DISTRO" = fixture ] || exit 92
[ "$WSLENV" = 'KEPT/u:RACH2_DISTRO' ] || exit 93
[ "$1 $2 $3" = '-NoProfile -NonInteractive -Command' ] || exit 94
if read -r input; then exit 95; fi
printf '%s\n' "$QUERY" > "$SPACE_LOG"
case $QUERY in
    good) printf 'D:\\ 44023414784\r\n' ;;
    edge) printf 'D:\\ 42949672960\r\n' ;;
    low) printf 'D:\\ 42949672959\r\n' ;;
    empty) ;;
    malformed) printf 'D:\\ unknown\n' ;;
    root) printf 'unknown 999999999999\n' ;;
    duplicate) printf 'D:\\ 999999999999\nD:\\ 999999999999\n' ;;
    noise) printf 'error\nD:\\ 999999999999\n' ;;
    partial) printf 'D:\\ 999999999999\n'; exit 1 ;;
    failed) exit 1 ;;
    *) exit 96 ;;
esac
FIXTURE
chmod 755 "$work/bin/df" "$work/bin/timeout" "$work/windows/powershell.exe"

# row NAME: require the expected status and diagnostic, and require that
# successful admission reaches the next command only after a valid reading.
row()
{
    LINUX_FREE=104857600 QUERY=good WSL_DISTRO_NAME=fixture
    WSLENV=KEPT/u SPACE_LOG=$work/query
    export LINUX_FREE QUERY WSL_DISTRO_NAME WSLENV SPACE_LOG
    floor=40
    fixture_path=$work/windows:$work/bin
    want=0 text=
    case $1 in
        linux) WSL_DISTRO_NAME= ;;
        disabled) floor=0; QUERY=timeout; LINUX_FREE=unknown ;;
        linux-low) LINUX_FREE=41943039; want=1; text='39 GiB free under' ;;
        linux-unknown) LINUX_FREE=unknown; want=1; text='unknown GiB free under' ;;
        missing) fixture_path=$work/bin; want=1; text='powershell.exe is missing' ;;
        good|edge) QUERY=$1 ;;
        low) QUERY=low; want=1; text='39 GiB free on D:' ;;
        timeout) QUERY=timeout; want=1; text='PowerShell status 124' ;;
        failed|partial) QUERY=$1; want=1; text='PowerShell status 1' ;;
        empty|malformed|root|duplicate|noise)
            QUERY=$1; want=1; text='no valid free-space reading' ;;
        *) return 1 ;;
    esac
    rm -f "$work/query" "$work/started"
    status=0
    printf 'input must stay unread\n' | PATH="$fixture_path" /bin/sh -c '
        /bin/sh "$1" run "a run" "$2" "$3" || exit $?
        : > "$4"
    ' sh "$copy" "$floor" "$work" "$work/started" > "$work/out" 2>&1 || status=$?
    if [ "$status" != "$want" ]; then
        echo "space-test: failed: $1: status $status, expected $want" >&2
        cat "$work/out" >&2
        return 1
    fi
    if [ "$want" = 0 ]; then
        [ -e "$work/started" ] && [ ! -s "$work/out" ] || return 1
        case $1 in
            good|edge) [ "$(cat "$work/query")" = "$QUERY" ] || return 1 ;;
            *) [ ! -e "$work/query" ] || return 1 ;;
        esac
    else
        [ ! -e "$work/started" ] &&
            grep -q '^run: result: refused: ' "$work/out" &&
            grep -Fq "$text" "$work/out" || return 1
    fi
}

# rows: check every admission outcome without waiting for a real timeout.
rows()
{
    for r in linux disabled linux-low linux-unknown missing good edge low \
        timeout failed partial empty malformed root duplicate noise; do
        row "$r" || { echo "space-test: failed: $r" >&2; return 1; }
    done
}
fault none
rows
if [ "$mode" = mutate ]; then
    for pair in unknown-success:empty status-ignored:partial missing-success:missing; do
        f=${pair%:*} r=${pair#*:}
        fault "$f"
        if row "$r" > "$work/fault.out" 2>&1; then
            echo "space-test: fault passed: $f" >&2
            exit 1
        fi
        echo "space-test: result: caught: $f ($r)"
    done
    fault none
    rows
fi
echo 'space-test: result: pass: disk admission and unavailable Windows readings'
