#!/bin/sh
# shellcheck disable=SC2034,SC2016 # Extracted functions and literal mutations.
# Exercise the mutation script's image construction and interrupted command.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
root=$(mktemp -d "${TMPDIR:-/tmp}/rach2-usbdisk-script.XXXXXX")
echo "usbdisk-script: logs: $root"
[ "$#" -eq 0 ] || [ "$*" = --mutate ] || exit 2
cp "$repo/tests/system/usbdisk_mutate.sh" "$root/saved.sh"
cp "$root/saved.sh" "$root/source.sh"
"${CC:-cc}" -O2 -Wall -Wextra -Werror -o "$root/runutil" "$repo/tests/system/runner/runutil.c"
child=
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

extract()
{
    awk '
        /^(build_probe|cleanup|run_logged)\(\)/ {copy = 1}
        copy {print}
        copy && /^}/ {copy = 0}
    ' "$root/source.sh" > "$root/functions.sh"
}

# A conditional caller disables errexit throughout an ordinary shell function.
# Exec a new shell so the extracted builder has the production failure rules,
# with the compiler and flags mk/arch.sh gives the mutation script.
image_build()
{
    sh -eu -c 'REPO=$2; . "$2/mk/arch.sh"; . "$1"; repo=$2; work=$3; build_probe' \
        usbdisk-image "$root/functions.sh" "$repo" "$work"
}

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
    sed '/^    "${CROSS_CC/i\
    [ -f "$work/usbdiskprobe" ] ||
' "$root/saved.sh" > "$root/source.sh"
    extract
    status=0
    image_case stale > "$root/stale.log" 2>&1 || status=$?
    [ "$status" -eq 1 ]
    grep -qx 'usbdisk-script: failed: image kept a stale assertion' "$root/stale.log"
    echo 'usbdisk-script: result: stale probe detected'
    cp "$root/saved.sh" "$root/source.sh"
    rows restored-image
    sed 's|"$work/usbdisk-proof.manifest"|"$work/unknown-probe.manifest"|g' \
        "$root/saved.sh" > "$root/source.sh"
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
    sed '/^    rm -f "$work\/output"$/d' "$root/saved.sh" > "$root/source.sh"
    extract
    status=0
    fifo_case retained > "$root/retained.log" 2>&1 || status=$?
    [ "$status" -eq 1 ]
    grep -qx 'usbdisk-script: failed: interrupted FIFO or command remains' "$root/retained.log"
    echo 'usbdisk-script: result: retained FIFO detected'
    cp "$root/saved.sh" "$root/source.sh"
    rows restored-fifo
fi
