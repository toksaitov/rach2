#!/bin/sh
# Prepare one admitted native sequence; execution uses only its C runner.
#
# native_prepare.sh PURPOSE TREE OUT ARCHES RUN_ID SITE ETUDE ACCEL SELECTION...
#
# ETUDE is the caller's verified immutable executable. OUT already contains
# the outer observer's header. Every output is new; partial failure evidence
# is retained. The caller owns admission and the observer on every exit.
set -eu
set -C

finished=
finish()
{
    status=$?
    trap - 0
    [ -n "$finished" ] || [ "$status" -ne 0 ] || status=1
    exit "$status"
}
trap finish 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
refuse()
{
    echo "native preparation: refused: $*" >&2
    exit 2
}
[ "$#" -ge 9 ] || refuse 'expected PURPOSE TREE OUT ARCHES RUN_ID SITE ETUDE ACCEL SELECTION...'
purpose=$1 tree=$2 out=$3 arches=$4 run=$5 site=$6 etude=$7 accel=$8
shift 8
case $purpose in
    phase_first_run)
        [ -n "${ETUDE_PHASE_SELECTION_EXPORT:-}" ] && [ -f "$ETUDE_PHASE_SELECTION_EXPORT" ] ||
            refuse 'phase_first_run requires a preallocated selection export'
        ;;
    ci|push|release|focused)
        [ "${ETUDE_PHASE_SELECTION_EXPORT+x}" != x ] ||
            refuse 'routine preparation cannot use a phase selection exemption'
        ;;
    *) refuse "invalid purpose: $purpose" ;;
esac
case $arches in both|i686|x86_64) ;; *) refuse "invalid architectures: $arches" ;; esac
case $accel in tcg|kvm|hvf|whpx) ;; *) refuse "invalid accelerator: $accel" ;; esac
for word do case $word in ''|-*) refuse "invalid selection word: $word" ;; esac; done
[ -n "${ETUDE_ADMITTED:-}" ] || refuse 'the sequence is not admitted'
case ${ETUDE_PHYSICAL_TRACE:+set}/${ETUDE_PHYSICAL_STOP_REQUEST:+set} in
    /|set/set) ;; *) refuse 'physical observation needs both trace and stop paths' ;;
esac
windows=0
case $(uname -s) in MINGW*|MSYS*|UCRT*|CLANG*|CYGWIN*) windows=1 ;; esac
native_path()
{
    if [ "$windows" -eq 1 ]; then cygpath -am "$1"
    else case $1 in /*) printf '%s\n' "$1" ;; *) printf '%s/%s\n' "$PWD" "$1" ;; esac
    fi
}
program()
{
    found=$(command -v "$1") || refuse "program unavailable: $1"
    [ -f "$found" ] && [ -x "$found" ] || refuse "not an executable file: $found"
    native_path "$found"
}
tree=$(cd "$tree" && pwd -P)
out=$(cd "$out" && pwd -P)
site=$(native_path "$site")
[ "$(native_path "$ETUDE_ADMITTED")" = "$site" ] || refuse 'the admitted site differs'
[ -r "$site" ] || refuse 'site policy cannot be read'
etude=$(program "$etude")
python=$(program python3)
compiler=$(program "${CC:-cc}")
qemu_i686=$(program qemu-system-i386)
qemu_x86_64=$(program qemu-system-x86_64)
if [ "$windows" -eq 1 ]; then
    MSYS2_ARG_CONV_EXCL='*'
    export MSYS2_ARG_CONV_EXCL
fi
python_path()
{
    if [ "$windows" -eq 1 ]; then
        case $("$python" -c 'import sys; print(sys.platform)') in
            msys|cygwin) cygpath -au "$1" ;;
            win32) native_path "$1" ;;
            *) refuse 'unsupported preparation Python platform' ;;
        esac
    else native_path "$1"
    fi
}
tree_native=$(native_path "$tree")
out_native=$(native_path "$out")
generator=$(python_path "$tree/tools/etude/measurement_context.py")
[ -f "$generator" ] || refuse 'measurement context generator is absent'
for name in prepare.record preparation.receipt.json selection.list selection.export.json source.json source.descriptor.json context; do
    [ ! -e "$out/$name" ] && [ ! -L "$out/$name" ] || refuse "output exists: $out/$name"
done
[ -r "$out/outer-load.jsonl" ] || refuse 'outer observation is absent'
[ ! -e "$out/outer-stop.request" ] && [ ! -L "$out/outer-stop.request" ] || refuse 'outer observation already stopped'
commit=$(git -C "$tree" rev-parse HEAD)
ETUDE_ADMITTED=$site ETUDE_SITE=$site RACH2_QEMU_ACCEL=$accel
ETUDE_PREPARATION_RUN_ID=$run
ETUDE_PREPARATION_RECEIPT=$out_native/preparation.receipt.json
ETUDE_PREPARATION_COMMIT=$commit
export ETUDE_ADMITTED ETUDE_SITE RACH2_QEMU_ACCEL
export ETUDE_PREPARATION_RUN_ID ETUDE_PREPARATION_RECEIPT ETUDE_PREPARATION_COMMIT
"$etude" prepare collection -t "$tree_native" -a "$arches" -p "$out_native/prepare.record" "$@"
unset ETUDE_PREPARATION_RUN_ID ETUDE_PREPARATION_RECEIPT ETUDE_PREPARATION_COMMIT
if [ "${ETUDE_PREPARATION_EXPORT:-0}" = 1 ]; then
    [ "$windows" -eq 0 ] && [ "$(uname -s)" = Linux ] || refuse 'producer export requires Linux'
    "$python" "$(python_path "$tree/tools/etude/preparation_import.py")" export \
        --tree "$tree_native" --commit "$commit" --record "$out_native/prepare.record" \
        --receipt "$out_native/preparation.receipt.json" --etude "$etude" --output "$out_native/producer.json"
fi
if [ "$purpose" != phase_first_run ]; then
    [ -n "${ETUDE_REFERENCE:-}" ] || refuse 'routine preparation needs the selected reference policy and history'
    "$python" "$(python_path "$tree/tools/ci/reference.py")" --tree "$tree_native" --etude "$etude" \
        --reference "$(native_path "$ETUDE_REFERENCE")" --output "$out_native/reference.forecast" \
        --site "$site" --accelerator "$accel" --preparation "$out_native/preparation.receipt.json"
fi
"$etude" run -N -J "$out_native/selection.export.json" -t "$tree_native" -a "$arches" -A "$accel" "$@"
"$etude" run -N -t "$tree_native" -a "$arches" -p "$out_native/prepare.record" -A "$accel" "$@" > "$out/selection.list"
"$python" "$generator" source --tree "$tree_native" --commit "$commit" --output "$out_native/source.json" > "$out/source.descriptor.json"
source_sha=$("$python" -c 'import json,sys; print(json.load(open(sys.argv[1]))["sha256"])' "$(python_path "$out/source.descriptor.json")")
set -- compose --tree "$tree_native" --commit "$commit" \
    --source-snapshot "$out_native/source.json" --source-sha256 "$source_sha" \
    --record "$out_native/prepare.record" --site "$site" --etude "$etude" \
    --tool "$compiler" --tool "$python" --tool "$qemu_i686" --tool "$qemu_x86_64" \
    --run-id "$run" --purpose "$purpose" --preparation "$out_native/preparation.receipt.json" \
    --outer-trace "$out_native/outer-load.jsonl" --outer-stop-request "$out_native/outer-stop.request" \
    --output "$out_native/context" \
    --cache-method "Public preparation profile $purpose; clean-inhouse=${ETUDE_PREPARATION_CLEAN:-0}; imported=${ETUDE_PREPARATION_IMPORT:+yes}. Artifact hashes are verified; OS cache remains natural." \
    --ordinary-preparation 'Selected candidate production, verification, source capture and context composition occur inside admission.' \
    --one-time-production 'Existing installed toolchains and pinned download caches are retained; candidate artifact production is ordinary preparation.'
# A fixed x86-64 helper may also be prepared for an i686-only selection.
for arch in i686 x86_64; do
    if command -v "$arch-elf-gcc" >/dev/null 2>&1; then
        cross=$(program "$arch-elf-gcc")
        set -- "$@" --tool "$cross"
    fi
done
if [ -n "${ETUDE_PREPARATION_IMPORT:-}" ]; then
    for tool in make rsync; do
        resolved=$(program "$tool")
        set -- "$@" --tool "$resolved"
    done
    transport=$("$python" -c 'import json,sys; print(json.load(open(sys.argv[1]))["producer"]["transport"])' \
        "$(python_path "$ETUDE_PREPARATION_IMPORT")")
    case $transport in
        ssh) resolved=$(program ssh); set -- "$@" --tool "$resolved" ;;
        local)
            if [ "$windows" -eq 1 ]; then
                resolved=$(program cygpath)
                set -- "$@" --tool "$resolved"
            fi
            ;;
        *) refuse "invalid producer transport: $transport" ;;
    esac
fi
if [ -n "${ETUDE_PHYSICAL_TRACE:-}" ]; then
    set -- "$@" --physical-trace "$(native_path "$ETUDE_PHYSICAL_TRACE")" \
        --physical-stop-request "$(native_path "$ETUDE_PHYSICAL_STOP_REQUEST")"
fi
if [ "$purpose" = phase_first_run ]; then
    set -- "$@" --selection-export "$(native_path "$ETUDE_PHASE_SELECTION_EXPORT")"
fi
"$python" "$generator" "$@"
finished=1
