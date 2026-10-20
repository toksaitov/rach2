#!/bin/sh
# shellcheck disable=SC2034,SC2016 # Extracted functions and literal mutations.
# usbdisk_script_test.sh [SCRIPT]: check build_probe, cleanup and run_logged
# of tests/system/usbdisk_mutate.sh, or of SCRIPT, a copy of it, copied out of
# the script and run on their own.
#
# The image row builds the probe and its image twice, with a changed
# assertion in the probe between, and requires the image to have the changed
# probe; a compiler that fails must stop the build with its status. The FIFO
# rows interrupt a command that run_logged runs, and require that the command
# and the FIFO are gone, that the saved vfs.c is put back, and that run_logged
# works again afterwards. USBDISK_SCRIPT_ROOT, when set, is the tree whose
# build and sources the rows use, in place of the one that holds this file.
#
# tests/proofs/usbdisk-script.json requires each of four faults to fail its
# row: a probe built only when none is built, a manifest that no set-ID
# allowlist names, a builder called in a condition, where errexit is off, and
# a FIFO kept after cleanup.
set -eu
self=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)/$(basename -- "$0")
repo=${USBDISK_SCRIPT_ROOT:-$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)}
[ "$#" -le 1 ] || { echo 'usbdisk-script: usage: usbdisk_script_test.sh [SCRIPT]' >&2; exit 2; }
script=${1:-$repo/tests/system/usbdisk_mutate.sh}
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" usbdisk-script "$self" "$@"
root=
child=
keep=
# Stop the running case. Then remove the work directory, unless the test
# failed after printing its path, which sets `keep`; a signal clears `keep`.
cleanup_test()
{
    exit_status=$?
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
    fi
    [ -n "$root" ] || return 0
    if [ "$exit_status" -eq 0 ] || [ -z "$keep" ]; then
        rm -rf "$root"
    fi
}
trap cleanup_test EXIT
trap 'keep=; exit 129' HUP
trap 'keep=; exit 130' INT
trap 'keep=; exit 143' TERM
root=$(mktemp -d "${TMPDIR:-/tmp}/rach2-usbdisk-script.XXXXXX")
echo "usbdisk-script: logs: $root"
keep=1
# The architecture, from ARCH as usbdisk_mutate.sh takes it, for the
# mk/arch.sh the extracted builder sources, which reads RACH2_ARCH alone.
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
export RACH2_ARCH
cp "$script" "$root/source.sh"
"${CC:-cc}" -O2 -Wall -Wextra -Werror -o "$root/runutil" "$repo/tests/system/runner/runutil.c"

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
    if ! grep -q 'the child volume would not mount' "$work/usbdiskprobe.c"; then
        echo 'usbdisk-script: failed: usbdiskprobe.c lacks the assertion the image row changes' >&2
        return 1
    fi
    if ! image_build > "$work/first.log" 2>&1; then
        cat "$work/first.log" >&2
        if [ -e "$work/extracted" ]; then
            echo 'usbdisk-script: failed: the first image build failed after extraction' >&2
        else
            echo 'usbdisk-script: failed: the first image build stopped before extraction' >&2
        fi
        return 1
    fi
    cp "$work/extracted" "$work/old-probe"
    sed 's/the child volume would not mount/current-source-assertion/' \
        "$work/usbdiskprobe.c" > "$work/new.c"
    mv "$work/new.c" "$work/usbdiskprobe.c"
    if ! image_build > "$work/second.log" 2>&1; then
        cat "$work/second.log" >&2
        echo 'usbdisk-script: failed: the second image build failed' >&2
        return 1
    fi
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
