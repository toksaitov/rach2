#!/bin/sh
# Prepare and run one native collection, then project its complete history.
set -eu
kind=$1
out=$2
shift 2
case $kind in core|full|push) ;; *) exit 2 ;; esac
tree=$(pwd -P)
[ -d "$out" ] || { echo 'ci: refused: allocate persistent OUT with retention.py first' >&2; exit 2; }
out=$(cd "$out" && pwd -P)
[ ! -e "$out/start" ] || { echo "ci: refused: result directory was already used: $out" >&2; exit 2; }
sha=$(git rev-parse HEAD)
if ! git diff --quiet || ! git diff --cached --quiet; then
    echo 'ci: refused: routine collection requires a clean committed tree' >&2; exit 2;
fi
case ${ARCH:-i686} in
    i686) arches=i686; selected=i686 ;;
    x86_64) arches=x86_64; selected=x86_64 ;;
    'i686 x86_64') arches=both; selected='i686 x86_64' ;;
    *) echo 'ci: refused: invalid architecture selection' >&2; exit 2 ;;
esac
site=${ETUDE_SITE:-/etc/rach2/etude-site}
. "$tree/tools/ci/native_site.sh"
native_site_load "$site"
etude=${ETUDE:-$native_site_etude}
[ -x "$etude" ] || { echo 'ci: refused: preinstalled immutable native etude is unavailable' >&2; exit 2; }
case $native_site_endpoint in
    windows)
        tree=$(cygpath -am "$tree")
        out=$(cygpath -am "$out")
        site=$(cygpath -am "$site")
        etude=$(cygpath -am "$etude")
        ;;
esac
case $etude in "$tree"/build/*|"$out"/*)
    echo 'ci: refused: ETUDE must be installed outside mutable preparation outputs' >&2; exit 2 ;;
esac
case $etude in /*|[A-Za-z]:/*) ;; *) echo 'ci: refused: ETUDE must be absolute' >&2; exit 2 ;; esac
purpose=${ETUDE_PURPOSE:-ci}
[ "$kind" != full ] || purpose=${ETUDE_PURPOSE:-release}
[ "$kind" != push ] || purpose=push
case $kind/$purpose in core/ci|core/push|full/ci|full/release|full/push|push/push) ;;
    *) echo 'ci: refused: purpose does not match the routine selection' >&2; exit 2 ;;
esac
for filter in "${RACH2_CASES:-}" "${RACH2_AGENTS_ROWS:-}" "${RACH2_ACCEPT_AGENTS:-}"; do
    [ "$kind" = push ] || [ -z "$filter" ] || { echo 'ci: refused: routine selection cannot inherit case filters' >&2; exit 2; }
done
run="routine-$purpose-${GITHUB_RUN_ID:-local}-${GITHUB_RUN_ATTEMPT:-1}-$$"
[ -n "${ETUDE_RETENTION:-}" ] || { echo 'ci: refused: selected-site retention policy is absent' >&2; exit 2; }
retention_script=$tree/tools/ci/retention.py
case $(uname -s) in MINGW*|MSYS*|UCRT*|CLANG*|CYGWIN*) retention_script=$(cygpath -au "$retention_script") ;; esac
python3 "$retention_script" --policy "$ETUDE_RETENTION" --site "$site" begin "$out" "$run"
(set -C; : > "$out/start")
accel=${RACH2_QEMU_ACCEL:-}
[ -n "$accel" ] || accel=$(sh "$tree/tools/ci/site.sh" "$tree/tools/ci/site" | sed -n 's/^accelerator=//p')
"$etude" run -t "$tree" inventory > "$out/suites.inventory"
if [ "$kind" != push ]; then
set -f
# shellcheck disable=SC2046 # the C inventory contains validated suite names
set -- $(awk -F'|' -v a="$arches" '
    ($2 == "core" || $1 == "style" || $1 == "host") &&
    ($3 == "" || a == "both" || index("," $3 ",", "," a ",")) { print $1 }
' "$out/suites.inventory")
set +f
else
    [ "$#" -gt 0 ] || { echo 'ci: refused: push requires explicit suites' >&2; exit 2; }
fi
if [ "$kind" = full ]; then
    set -- all
    if [ -n "${SUITES:-}" ]; then
        set -f
        # SUITES is a list of public ARCH:SUITE operands.
        # shellcheck disable=SC2086
        set -- $SUITES
        set +f
        have_i686= have_x86_64=
        for suite do
            case $suite in
                i686:*) have_i686=1 ;;
                x86_64:*) have_x86_64=1 ;;
                *) echo "ci: refused: invalid selected suite: $suite" >&2; exit 2 ;;
            esac
        done
        case $have_i686/$have_x86_64 in
            1/1) arches=both ;;
            1/) arches=i686 ;;
            /1) arches=x86_64 ;;
        esac
    fi
fi
for arch in $selected; do
    result=$out/$kind-$arch
    mkdir -p "$result"
    printf 'etude-native-1\n' > "$result/native"
    printf '%s\n' "$sha" > "$result/commit"
    : > "$result/failed"
done
"$etude" observe parent > "$out/caller.identity"
IFS=' ' read -r caller_pid caller_creation caller_extra < "$out/caller.identity"
case $caller_pid in ''|*[!0-9]*|0) echo 'ci: refused: invalid native caller PID' >&2; exit 2 ;; esac
case $caller_creation in ''|*[!0-9]*|0) echo 'ci: refused: invalid native caller creation' >&2; exit 2 ;; esac
[ -z "$caller_extra" ] || exit 2
ETUDE_SEQUENCE_PARENT=$caller_pid:$caller_creation
export ETUDE_SEQUENCE_PARENT
status=0
# The native owner mirrors phase output to OUT/log and the console.
# Keep this immediate parent alive; an intermediate pipeline could outlive it.
sh "$tree/tools/ci/native_sequence.sh" "$purpose" "$tree" "$out" "$arches" "$run" \
    "$site" "$etude" "$accel" "$@" || status=$?
printf '%s\n' "$status" > "$out/sequence.status"
if [ ! -f "$out/log" ]; then
    printf 'ci: native sequence stopped before its output log opened (status %s)\n' "$status" > "$out/log"
fi
[ "$status" -lt 128 ] || exit 125
: > "$out/failed"
result_status=0
for arch in $selected; do
    result=$out/$kind-$arch
    if [ "$kind" = full ] && [ -n "${SUITES:-}" ]; then
        case " $SUITES " in
            *" $arch:"*) ;;
            *) : > "$result/skipped"; : > "$result/done"; continue ;;
        esac
    fi
    suites=
    if [ -s "$out/selection.list" ]; then
        suites=$(awk -v a="$arch" '$2 == a || $2 == "none" { if (!seen[$1]++) print $1 }' \
            "$out/selection.list" | paste -sd, -)
    fi
    printf '%s\n' "$suites" > "$result/suites"
    verdict=2
    if [ -n "$suites" ]; then
        if [ "$kind" = push ]; then
            set -- --selection "$out/selection.export.json"
        else
            set --
        fi
        "$etude" check verdict "$@" --project-architecture --require-budget -b "$out/store" \
            -c "$sha" "$arch" "$kind" "$suites" "$out/history" > "$result/verdict" 2>&1 && verdict=0 || verdict=$?
    else
        printf 'ci: no complete native selection\n' > "$result/verdict"
    fi
    cat "$result/verdict"
    failures=$(sed -n 's/^etude check: result: failed: //p' "$result/verdict")
    if [ "$verdict" != 0 ]; then
        [ "$verdict" = 1 ] && [ -n "$failures" ] || failures="runner $failures"
    elif [ "$status" != 0 ]; then
        # A foreign ordinary failure may legitimately project to a pass.
        [ "$status" = 1 ] || failures=runner
    fi
    for failure in $failures; do printf '%s\n' "$failure"; done | sort -u > "$result/failed"
    if [ -s "$result/failed" ]; then
        result_status=1
        sed "s/^/$arch:/" "$result/failed" >> "$out/failed"
    fi
    cp "$out/log" "$result/log"
    cp "$result/verdict" "$result/err"
    : > "$result/done"
done
python3 "$retention_script" --policy "$ETUDE_RETENTION" --site "$site" seal "$out"
exit "$result_status"
