#!/bin/sh
# space_test.sh: check disk admission against command fixtures, including a
# Windows query that times out or returns no usable path, and a known disk
# path that needs no query.
#
# --mutate permits an unknown reading, ignores a failed query, permits a
# missing PowerShell, or forgets the kept disk path in a private copy; each
# fault must fail its own row. --texts checks only that the faults change
# the copy.
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
        status-ignored) sed 's/^\( *\)status=\$?/\1status=0/' ;;
        missing-success) sed '/powershell.exe is missing/{n;s/exit 1/exit 0/;}' ;;
        cache-ignored) sed '/IFS= read -r disk/s/^\[[^]]*\] ||/: ||/' ;;
    esac < "$repo/tools/setup/space.sh" > "$copy"
    [ "$1" = none ] || ! cmp -s "$repo/tools/setup/space.sh" "$copy"
}
if [ "$mode" = texts ]; then
    for f in unknown-success status-ignored missing-success cache-ignored; do fault "$f"; done
    exit 0
fi
mkdir "$work/bin" "$work/windows"
for c in awk mkdir mv rm tr; do ln -s "$(command -v "$c")" "$work/bin/$c"; done
cat > "$work/bin/df" <<'FIXTURE'
#!/bin/sh
printf 'Filesystem 1024-blocks Used Available Capacity Mounted on\n'
case $2 in
    "$RACH2_WSL_MOUNTS"/*)
        [ -e "$2" ] || exit 1
        printf '%s 999999999 0 %s 0%% %s/d\n' "$WINDOWS_FS" "$WINDOWS_FREE" \
            "$RACH2_WSL_MOUNTS" ;;
    *)
        [ "$LINUX_FREE" != unknown ] || exit 1
        printf 'fixture 999999999 0 %s 0%% /\n' "$LINUX_FREE" ;;
esac
FIXTURE
cat > "$work/bin/timeout" <<'FIXTURE'
#!/bin/sh
[ "$1 $2 $3" = '-k 5 60' ] || exit 91
shift 3
case $1 in
    df) [ "$DRIVE" != timeout ] || exit 124 ;;
    *) [ "$QUERY" != timeout ] || exit 124 ;;
esac
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
    good) printf 'D:\\WSL\\fixture\\ext4.vhdx\r\n' ;;
    lower) printf 'd:\\WSL\\fixture\\ext4.vhdx\n' ;;
    absent) printf 'D:\\WSL\\moved\\ext4.vhdx\n' ;;
    empty) ;;
    root) printf '\\\\server\\share\\ext4.vhdx\n' ;;
    duplicate) printf 'D:\\WSL\\fixture\\ext4.vhdx\nD:\\WSL\\fixture\\ext4.vhdx\n' ;;
    noise) printf 'error\nD:\\WSL\\fixture\\ext4.vhdx\n' ;;
    partial) printf 'D:\\WSL\\fixture\\ext4.vhdx\n'; exit 1 ;;
    failed) exit 1 ;;
    *) exit 96 ;;
esac
FIXTURE
chmod 755 "$work/bin/df" "$work/bin/timeout" "$work/windows/powershell.exe"
mkdir -p "$work/mnt/d/WSL/fixture"
: > "$work/mnt/d/WSL/fixture/ext4.vhdx"

# row NAME: require the expected status and diagnostic, require that
# successful admission reaches the next command only after a valid reading,
# and require that only a valid reading keeps the disk path it was read by.
row()
{
    LINUX_FREE=104857600 WINDOWS_FREE=42991616 WINDOWS_FS="D:\\"
    QUERY=good DRIVE=good WSL_DISTRO_NAME=fixture WSLENV=KEPT/u
    SPACE_LOG=$work/query XDG_CACHE_HOME=$work/cache RACH2_WSL_MOUNTS=$work/mnt
    export LINUX_FREE WINDOWS_FREE WINDOWS_FS QUERY DRIVE WSL_DISTRO_NAME \
        WSLENV SPACE_LOG XDG_CACHE_HOME RACH2_WSL_MOUNTS
    floor=40
    fixture_path=$work/windows:$work/bin
    want=0 text='' kept='' query=''
    disk='D:\WSL\fixture\ext4.vhdx'
    case $1 in
        linux) WSL_DISTRO_NAME= ;;
        disabled) floor=0; QUERY=timeout; LINUX_FREE=unknown ;;
        linux-low) LINUX_FREE=41943039; want=1; text='39 GiB free under' ;;
        linux-unknown) LINUX_FREE=unknown; want=1; text='unknown GiB free under' ;;
        missing) fixture_path=$work/bin; want=1; text='powershell.exe is missing' ;;
        good) query=good ;;
        lower) QUERY=lower; query=lower ;;
        edge) WINDOWS_FREE=41943040; query=good ;;
        low) WINDOWS_FREE=41943039; query=good; want=1; text='39 GiB free on D:' ;;
        cached) kept=$disk; QUERY=timeout ;;
        cached-low)
            kept=$disk; QUERY=timeout
            WINDOWS_FREE=41943039; want=1; text='39 GiB free on D:' ;;
        stale) kept='D:\WSL\moved\ext4.vhdx'; query=good ;;
        stale-timeout)
            kept='D:\WSL\moved\ext4.vhdx'; QUERY=timeout
            want=1; text='PowerShell status 124' ;;
        timeout) QUERY=timeout; want=1; text='PowerShell status 124' ;;
        failed|partial) QUERY=$1; want=1; text='PowerShell status 1' ;;
        empty|root|duplicate|noise|absent)
            QUERY=$1; want=1; text='no valid free-space reading' ;;
        drive-timeout) DRIVE=timeout; want=1; text='no valid free-space reading' ;;
        drive-foreign) WINDOWS_FS=fixture; want=1; text='no valid free-space reading' ;;
        drive-unknown) WINDOWS_FREE=unknown; want=1; text='no valid free-space reading' ;;
        *) return 1 ;;
    esac
    rm -rf "$work/query" "$work/started" "$work/cache"
    if [ -n "$kept" ]; then
        mkdir "$work/cache" "$work/cache/rach2"
        printf '%s\n' "$kept" > "$work/cache/rach2/wsl-disk.fixture"
    fi
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
    if [ -n "$query" ]; then
        [ "$(cat "$work/query")" = "$query" ] || return 1
    elif [ "$want" = 0 ]; then
        [ ! -e "$work/query" ] || return 1
    fi
    if [ -n "$query" ]; then
        kept=$disk
        [ "$query" != lower ] || kept='d:\WSL\fixture\ext4.vhdx'
    fi
    if [ "$want" = 0 ]; then
        [ -e "$work/started" ] && [ ! -s "$work/out" ] || return 1
    else
        [ ! -e "$work/started" ] &&
            grep -q '^run: result: refused: ' "$work/out" &&
            grep -Fq "$text" "$work/out" || return 1
    fi
    case $1 in linux|disabled|linux-low|linux-unknown) return 0 ;; esac
    if [ -n "$kept" ]; then
        [ "$(cat "$work/cache/rach2/wsl-disk.fixture")" = "$kept" ] &&
            [ "$(ls "$work/cache/rach2")" = wsl-disk.fixture ] || return 1
    else
        [ ! -e "$work/cache/rach2/wsl-disk.fixture" ] || return 1
    fi
}

# rows: check every admission outcome without waiting for a real timeout.
rows()
{
    for r in linux disabled linux-low linux-unknown missing good lower edge \
        low cached cached-low stale stale-timeout timeout failed partial \
        empty root duplicate noise absent drive-timeout drive-foreign \
        drive-unknown; do
        row "$r" || { echo "space-test: failed: $r" >&2; return 1; }
    done
}
fault none
rows
if [ "$mode" = mutate ]; then
    for pair in unknown-success:drive-foreign status-ignored:partial \
        missing-success:missing cache-ignored:cached; do
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
echo 'space-test: result: pass: disk admission, kept disk paths and unavailable Windows readings'
