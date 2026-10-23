#!/bin/sh
# Give admitted preparation and execution to one native lifetime owner.
set -eu
[ "$#" -ge 9 ] || {
    echo 'usage: native_sequence.sh PURPOSE TREE OUT ARCHES RUN_ID SITE ETUDE ACCEL SELECTION...' >&2
    exit 2
}
purpose=$1 tree=$2 out=$3 arches=$4 run=$5 site=$6 etude=$7 accel=$8
shift 8
case $purpose in
    push) limit=270 ;;
    ci) limit=540 ;;
    release) limit=13800 ;;
    *) echo 'native-sequence: purpose must be push, ci or release' >&2; exit 2 ;;
esac
case $arches in i686|x86_64|both) ;; *) exit 2 ;; esac
. "$tree/tools/ci/native_site.sh"
native_site_load "$site"
[ -z "${ETUDE_ADMITTED:-}" ] || { echo 'native-sequence: an enclosing claim prevents case admission' >&2; exit 2; }
[ "$limit" -le "$native_site_seconds" ] || limit=$native_site_seconds
ETUDE_BUILD_JOBS=$native_site_jobs JOBS=$native_site_jobs
RACH2_MIN_FREE_GIB=${RACH2_MIN_FREE_GIB:-40}
ETUDE_SITE=$site
export ETUDE_BUILD_JOBS JOBS RACH2_MIN_FREE_GIB ETUDE_SITE
unset MAKEFLAGS MFLAGS
commit=$(git -C "$tree" rev-parse HEAD)
shell_program=$(command -v sh)
env_program=$(command -v env)
prepare_path=$PATH
runtime_path=${ETUDE_RUNTIME_PATH:-$PATH}
case $native_site_endpoint in
    windows)
        # Native executables take Windows paths without MSYS argv rewriting.
        shell_program=$(cygpath -am "$shell_program")
        env_program=$(cygpath -am "$env_program")
        prepare_path=$(cygpath -mp "$prepare_path")
        runtime_path=$(cygpath -mp "$runtime_path")
        MSYS2_ARG_CONV_EXCL='*'
        export MSYS2_ARG_CONV_EXCL
        ;;
esac
for program in "$shell_program" "$env_program"; do
    case $program in /*|[A-Za-z]:/*) ;; *) exit 2 ;; esac
done
# Each counted vector includes argv[0]. The shell only constructs arguments.
count=$#
exec "$etude" observe sequence "$out/sequence.receipt" "$run" \
    "$out/outer-load.jsonl" "$out/outer-stop.request" "$etude" \
    "$prepare_path" "$runtime_path" \
    "$etude" "$((27 + count))" "$etude" admit \
    -d "$native_site_locks" -w "$native_site_wait" -r "$out/preparation.admit.result" \
    "$native_site_ledger" "$run:prepare" "$native_site_account" shared \
    "$native_site_resources" "$native_site_capacity" "$limit" \
    "$env_program" "ETUDE_ADMITTED=$site" "$shell_program" "$tree/tools/ci/native_prepare.sh" \
    "$purpose" "$tree" "$out" "$arches" "$run" "$site" "$etude" "$accel" "$@" \
    "$etude" "$((18 + count))" "$etude" run \
    -t "$tree" -a "$arches" -p "$out/prepare.record" -o "$out" \
    -e "$out/failed-evidence" -I "$out/context/context.json" -g "$commit" -A "$accel" "$@"
