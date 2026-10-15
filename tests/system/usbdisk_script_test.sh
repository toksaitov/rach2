#!/bin/sh
# shellcheck disable=SC2034,SC2016 # Extracted functions and literal mutations.
# usbdisk_script_test.sh [--mutate | --texts]: check usbdisk_mutate.sh's
# build_probe, cleanup and run_logged, copied out of the script and run on
# their own.
#
# The image row builds the probe and its image twice, with a changed
# assertion in the probe between, and requires the image to have the changed
# probe; a compiler that fails must stop the build with its status. The FIFO
# rows interrupt a command that run_logged runs, and require that the command
# and the FIFO are gone, that the saved vfs.c is put back, and that run_logged
# works again afterwards. --mutate also runs four faults, each of which must
# fail its row: `stale` builds the probe only when none is built; `unknown`
# stages from a manifest that no set-ID allowlist names, which must stop the
# build before the probe is extracted; the builder is called in a condition,
# where errexit is off; and `retained` keeps the FIFO after cleanup. The rows
# of the restored script must pass after each. --texts checks that each fault
# changes usbdisk_mutate.sh and that the probe has the assertion the image row
# changes.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
if [ "${1:-}" != --texts ]; then
    . "$repo/tests/system/runlock.sh"
    runlock_hold "$repo" usbdisk-script "$repo/tests/system/usbdisk_script_test.sh" "$@"
fi
root=$(mktemp -d "${TMPDIR:-/tmp}/rach2-usbdisk-script.XXXXXX")
echo "usbdisk-script: logs: $root"
[ "$#" -eq 0 ] || [ "$*" = --mutate ] || [ "$*" = --texts ] || exit 2

# script_edit FAULT SCRIPT: write $root/source.sh as SCRIPT, a copy of
# usbdisk_mutate.sh, with FAULT in it; fail when the edit changes nothing,
# since its text has left the script.
script_edit()
{
    case $1 in
        stale)
            sed '/^    "${CROSS_CC/i\
    [ -f "$work/usbdiskprobe" ] ||
' "$2" > "$root/source.sh" ;;
        unknown)
            sed 's|"$work/usbdisk-proof.manifest"|"$work/unknown-probe.manifest"|g' \
                "$2" > "$root/source.sh" ;;
        retained)
            sed '/^    rm -f "$work\/output"$/d' "$2" > "$root/source.sh" ;;
    esac
    if cmp -s "$2" "$root/source.sh"; then
        echo "usbdisk-script: the $1 fault is not in usbdisk_mutate.sh" >&2
        exit 1
    fi
}

if [ "$*" = --texts ]; then
    for fault in stale unknown retained; do
        script_edit "$fault" "$repo/tests/system/usbdisk_mutate.sh"
    done
    if ! grep -q 'the child volume would not mount' "$repo/tests/usbdiskprobe.c"; then
        echo 'usbdisk-script: the probe no longer prints the assertion the rows edit' >&2
        exit 1
    fi
    rm -rf "$root"
    echo 'usbdisk-script: texts: every fault is once in its source'
    exit 0
fi
# The architecture, from ARCH as usbdisk_mutate.sh takes it, for the
# mk/arch.sh the extracted builder sources, which reads RACH2_ARCH alone.
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
export RACH2_ARCH
cp "$repo/tests/system/usbdisk_mutate.sh" "$root/saved.sh"
cp "$root/saved.sh" "$root/source.sh"
"${CC:-cc}" -O2 -Wall -Wextra -Werror -o "$root/runutil" "$repo/tests/system/runner/runutil.c"
child=
# Stop the running case and put the saved script back into the copy.
cleanup_test()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
    fi
    cp "$root/saved.sh" "$root/source.sh"
}
trap cleanup_test EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# Copy build_probe, cleanup and run_logged out of the copy of the script into
# functions.sh.
extract()
{
    awk '
        /^(build_probe|cleanup|run_logged)\(\)/ {copy = 1}
        copy {print}
        copy && /^}/ {copy = 0}
    ' "$root/source.sh" > "$root/functions.sh"
}

# Run build_probe in a new shell with errexit set, as the script runs it, with
# the compiler and flags mk/arch.sh gives the script. A function called in a
# condition runs with errexit off throughout, so it is not called directly.
image_build()
{
    sh -eu -c 'REPO=$2; . "$2/mk/arch.sh"; . "$1"; repo=$2; work=$3; build_probe' \
        usbdisk-image "$root/functions.sh" "$repo" "$work"
}

# image_case NAME: build the image twice in NAME-image, the second time with
# the probe's assertion changed, and require the second image to have the
# changed probe; then require a compiler that exits 23 to stop the build with
# status 23. Return 1 when a check fails.
image_case()
{
    work=$root/$1-image
    mkdir "$work"
    cp "$repo/tests/usbdiskprobe.c" "$work/usbdiskprobe.c"
    image_build > "$work/first.log" 2>&1 || return 1
    cp "$work/extracted" "$work/old-probe"
    sed 's/the child volume would not mount/current-source-assertion/' \
        "$work/usbdiskprobe.c" > "$work/new.c"
    mv "$work/new.c" "$work/usbdiskprobe.c"
    image_build > "$work/second.log" 2>&1 || return 1
    if cmp -s "$work/extracted" "$work/old-probe" ||
        ! strings "$work/extracted" | grep -qx current-source-assertion; then
        echo 'usbdisk-script: failed: image kept a stale assertion' >&2
        return 1
    fi
    echo 'usbdisk-script: result: image contains the changed probe assertion'
    cat > "$work/compiler-fails" <<'COMPILER'
#!/bin/sh
echo 'usbdisk-script: compiler refused' >&2
exit 23
COMPILER
    chmod +x "$work/compiler-fails"
    status=0
    CROSS_CC="$work/compiler-fails" image_build > "$work/compiler.log" 2>&1 || status=$?
    if [ "$status" -ne 23 ]; then
        echo 'usbdisk-script: failed: compiler failure was swallowed' >&2
        return 1
    fi
    echo 'usbdisk-script: result: compiler failure stops with earlier artifacts present'
}

# fifo_case PHASE: run a 30-second command under run_logged in a phase script
# and send the script SIGTERM. The command and the FIFO must be gone, the
# saved vfs.c must be back, and run_logged must then run a command again.
# Return 1 when a check fails.
fifo_case()
{
    phase=$1
    work=$root/$phase
    mkdir "$work"
    printf 'original source\n' > "$work/vfs.saved"
    printf 'mutated source\n' > "$work/vfs.c"
    cat > "$work/command.sh" <<'COMMAND'
#!/bin/sh
echo "$$" > "$1/pid"
exec "$2" sleep 30
COMMAND
    cat > "$work/phase.sh" <<'PHASE'
#!/bin/sh
set -eu
work=$1
runner=$2/runutil
source=$work/vfs.c
child=
recorder=
. "$2/functions.sh"
trap cleanup EXIT
trap 'exit 143' TERM
run_logged "$work/interrupted.log" sh "$work/command.sh" "$work" "$runner"
PHASE
    sh "$work/phase.sh" "$work" "$root" > "$work/phase.log" 2>&1 &
    child=$!
    tries=0
    while [ ! -s "$work/pid" ] && [ "$tries" -lt 100 ]; do
        "$root/runutil" sleep 0.05
        tries=$((tries + 1))
    done
    [ -s "$work/pid" ] || return 1
    kill -TERM "$child"
    status=0
    wait "$child" || status=$?
    child=
    [ "$status" -eq 143 ] || return 1
    pid=$(cat "$work/pid")
    if kill -0 "$pid" 2>/dev/null || [ -e "$work/output" ]; then
        echo 'usbdisk-script: failed: interrupted FIFO or command remains' >&2
        return 1
    fi
    cmp "$work/vfs.saved" "$work/vfs.c" || return 1
    (
        runner=$root/runutil
        child=
        recorder=
        # shellcheck disable=SC1091
        . "$root/functions.sh"
        run_logged "$work/resumed.log" sh -c 'echo usbdisk-script-resumed'
    ) > "$work/continuation.log" 2>&1 || return 1
    grep -qx usbdisk-script-resumed "$work/resumed.log" || return 1
    echo "usbdisk-script: result: $phase stopped, restored and resumed"
}

# rows NAME: extract the functions and run the image row and both FIFO rows.
rows()
{
    extract
    image_case "$1" || return 1
    fifo_case "$1-mutant" || return 1
    fifo_case "$1-restoration" || return 1
}

rows fixed
if [ "$#" -ne 0 ]; then
    # The stale binary is allowed only on the second invocation, reproducing
    # a source edit followed by a run that reuses an earlier built probe.
    script_edit stale "$root/saved.sh"
    extract
    status=0
    image_case stale > "$root/stale.log" 2>&1 || status=$?
    [ "$status" -eq 1 ]
    grep -qx 'usbdisk-script: failed: image kept a stale assertion' "$root/stale.log"
    echo 'usbdisk-script: result: stale probe detected'
    cp "$root/saved.sh" "$root/source.sh"
    rows restored-image
    script_edit unknown "$root/saved.sh"
    extract
    status=0
    image_case unknown > "$root/unknown.log" 2>&1 || status=$?
    [ "$status" -eq 1 ]
    grep -q 'no allowlist names this manifest' "$root/unknown-image/first.log"
    [ ! -e "$root/unknown-image/extracted" ]
    echo 'usbdisk-script: result: unlisted manifest stops before extraction'
    cp "$root/saved.sh" "$root/source.sh"
    rows restored-manifest
    status=0
    (
        REPO=$repo
        # shellcheck disable=SC1091
        . "$repo/mk/arch.sh"
        image_build()
        {
            # shellcheck disable=SC1091
            . "$root/functions.sh"
            build_probe
        }
        image_case swallowed
    ) > "$root/swallowed.log" 2>&1 || status=$?
    [ "$status" -eq 1 ]
    grep -qx 'usbdisk-script: failed: compiler failure was swallowed' "$root/swallowed.log"
    echo 'usbdisk-script: result: conditional function swallowed failure and was detected'
    rows restored-shell
    script_edit retained "$root/saved.sh"
    extract
    status=0
    fifo_case retained > "$root/retained.log" 2>&1 || status=$?
    [ "$status" -eq 1 ]
    grep -qx 'usbdisk-script: failed: interrupted FIFO or command remains' "$root/retained.log"
    echo 'usbdisk-script: result: retained FIFO detected'
    cp "$root/saved.sh" "$root/source.sh"
    rows restored-fifo
fi
