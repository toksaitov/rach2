#!/bin/sh
# launch.sh: admit a build or an old runner under the installed site policy.
set -eu

# Refuse before execution when the installed authority cannot be used.
refuse()
{
    echo "etude-launch: result: refused: $*" >&2
    [ -z "$result" ] || printf 'refused\n' > "$result"
    exit 2
}

result=
if [ "${1:-}" = -r ]; then
    [ "$#" -ge 3 ] || exit 2
    result=$2
    shift 2
fi
[ "$#" -ge 2 ] || { echo 'usage: etude-launch [-r FILE] build|legacy|alone COMMAND [ARGS...]' >&2; exit 2; }
kind=$1; shift
case $kind in build|legacy|alone) ;; *) refuse "unknown launch kind $kind" ;; esac
site=${ETUDE_SITE:-/etc/rach2/etude-site}
if [ ! -e "$site" ] && [ ! -L "$site" ] && [ "${ETUDE_SITE+x}" != x ]; then
    exec "$@"
fi
[ -r "$site" ] || refuse "cannot read $site"
header="" endpoint="" launcher="" ledger="" locks="" bridge="" capacity="" wait="" seconds=""
boundary=""
seen=" "
while IFS=' ' read -r key value || [ -n "$key$value" ]; do
    case $seen in *" $key "*) refuse "duplicate field $key" ;; esac
    seen="$seen$key "
    [ -n "$value" ] || refuse "empty field $key"
    case $key in
        etude-site) [ -z "$header" ] || refuse 'duplicate header'; header=$value ;;
        endpoint) [ -z "$endpoint" ] || refuse 'duplicate endpoint'; endpoint=$value ;;
        launcher) [ -z "$launcher" ] || refuse 'duplicate launcher'; launcher=$value ;;
        ledger) [ -z "$ledger" ] || refuse 'duplicate ledger'; ledger=$value ;;
        locks) [ -z "$locks" ] || refuse 'duplicate locks'; locks=$value ;;
        bridge) [ -z "${bridge:-}" ] || refuse 'duplicate bridge'; bridge=$value ;;
        capacity) [ -z "$capacity" ] || refuse 'duplicate capacity'; capacity=$value ;;
        wait) [ -z "$wait" ] || refuse 'duplicate wait'; wait=$value ;;
        seconds) [ -z "$seconds" ] || refuse 'duplicate seconds'; seconds=$value ;;
        boundary_lazy) boundary=$value ;;
        *) refuse "unknown field $key" ;;
    esac
done < "$site"
[ "$header" = 1 ] || refuse 'incompatible site format'
# The optional boundary_lazy permits the lazy 72 GiB boundary profile it names.
case $boundary in ''|bigmem-boundary-v1) ;; *) refuse 'boundary_lazy must be bigmem-boundary-v1' ;; esac
case $(uname -s)/$endpoint in Linux/linux|Darwin/macos|MINGW*/windows|MSYS*/windows|UCRT*/windows|CLANG*/windows) ;; *) refuse 'endpoint is disabled or incompatible' ;; esac
for path in "$site" "$launcher" "$ledger" "$locks" "${bridge:-}"; do
    case $path in /*|[A-Za-z]:/*) ;; *) refuse "not an absolute path: $path" ;; esac
done
[ -x "$bridge" ] && [ -x "$launcher" ] && [ -d "$locks" ] && [ -d "$ledger" ] ||
    refuse 'installed endpoint is unavailable'
[ "$(head -n 1 "$ledger/claims" 2>/dev/null)" = 'etude-ledger 1' ] ||
    refuse 'ledger is unavailable or incompatible'
printf '%s\n' "$capacity" | awk -F, '
    NF != 3 { exit 1 }
    { for (i = 1; i <= 3; i++) if ($i !~ /^[1-9][0-9]*$/ || length($i) > 15) exit 1 }
' || refuse 'capacity must give three positive amounts of at most 15 digits'
for bound in "$wait" "$seconds"; do
    case $bound in ''|*[!0-9]*|0*) refuse 'invalid time bound' ;; esac
    [ "${#bound}" -le 5 ] && [ "$bound" -le 86400 ] || refuse 'invalid time bound'
done
if [ "$kind" = legacy ]; then
    grep -q RACH2_PORT_OFFSET tests/system/suites.mk 2>/dev/null ||
        refuse 'the checkout does not support reserved port offsets'
fi
if [ -n "${ETUDE_ADMITTED:-}" ]; then
    [ "$ETUDE_ADMITTED" = "$site" ] || refuse 'nested launch changed the site'
    if [ -n "$result" ]; then
        status=0
        "$@" || status=$?
        if [ "$status" -ge 128 ]; then
            printf 'incomplete\n' > "$result"
        else
            printf 'exited %s\n' "$status" > "$result"
        fi
        exit "$status"
    fi
    exec "$@"
fi
[ -z "${RACH2_PORT_OFFSET+x}${SSH_TEST_PORT+x}" ] || refuse 'port overrides bypass admission'
# Check a caller's pool limit before using it in arithmetic.
positive()
{
    case $1 in ''|*[!0-9]*|0*) refuse "invalid $2" ;; esac
    [ "${#1}" -le 15 ] || refuse "invalid $2"
}

# Size the old runner from the selected suites without starting its build.
legacy_resources()
{
    case ${1##*/} in sh|bash|dash) shift ;; esac
    case ${1:-} in tests/run|*/tests/run) shift ;; *) run_share "$RACH2_MEMORY_MIB"; return ;; esac
    if [ "${1:-}" = inventory ]; then build_resources true; return; fi
    [ -r tests/registry ] || refuse 'cannot read the suite registry'
    # serial is an optional host isolation field after the architectures.
    selection=$(printf '%s\n' "$@" | awk -F'|' -v arch="${ARCH:-i686}" '
        NR == FNR {
            if ($0 ~ /^#/ || $1 == "") next
            order[++n] = $1; tier[$1] = $3; mem[$1] = $5 ? $5 : 128
            arches[$1] = $6; isolation[$1] = $7; next
        }
        { requested[$0] = 1 }
        END {
            for (i = 1; i <= n; i++) {
                s = order[i]
                if (arches[s] != "" && index("," arches[s] ",", "," arch ",") == 0) continue
                if (requested[s] || requested["all"] || (requested["core"] && tier[s] == "core"))
                    print mem[s], isolation[s]
            }
        }
    ' tests/registry -) || refuse 'cannot select suites'
    [ -n "$selection" ] || refuse 'no suites selected'
    guest=$(printf '%s\n' "$selection" | awk -v w="$RACH2_WORKERS" -v budget="$RACH2_MEMORY_MIB" '
        $1 !~ /^[1-9][0-9]*$/ { bad = 1 }
        { a[++n] = $1; if ($1 > largest) largest = $1 }
        END {
            if (bad) exit 1
            for (i = 1; i <= n; i++) for (j = i + 1; j <= n; j++)
                if (a[j] > a[i]) { t = a[i]; a[i] = a[j]; a[j] = t }
            for (i = 1; i <= n && i <= w; i++) sum += a[i]
            if (sum > budget) sum = budget
            if (sum < largest) sum = largest
            printf "%.0f\n", sum
        }
    ') || refuse 'invalid suite memory'
    half=$(sysctl -n hw.memsize 2>/dev/null || awk '/^MemTotal:/ {printf "%.0f\n", $2 * 1024}' /proc/meminfo)
    positive "$half" 'host memory'
    half=$((half / 2 / 1048576))
    if printf '%s\n' "$selection" | awk -v half="$half" '$1 > half || $2 == "serial" { found = 1 } END { exit !found }'; then
        mode=alone
    fi
    if printf '%s\n' "$selection" | awk '$2 == "serial" { found = 1 } END { exit !found }'; then
        RACH2_WORKERS=1
    fi
    run_share "$guest"
}

# Give the processors and memory of a test run of RACH2_WORKERS workers whose
# guests take GUEST MiB in all; an opaque command, whose builds and suites
# cannot be read before it runs, is given the whole guest pool.
run_share() # run_share GUEST
{
    cpus=${RACH2_CPUS:-2}
    positive "$cpus" RACH2_CPUS
    [ "$cpus" -le 64 ] || refuse 'invalid RACH2_CPUS'
    processors=$((RACH2_WORKERS * cpus * 500))
    [ "$processors" -ge "$((RACH2_WORKERS * 1000))" ] || processors=$((RACH2_WORKERS * 1000))
    # Native host checks use one GiB per worker beside the guest pool.
    memory=$((($1 + RACH2_WORKERS * 1152 + 1024) * 1048576))
    [ "$memory" -ge "$build_memory" ] || memory=$build_memory
}

# Count make's job limit, including the flags inherited by its recursive entry.
build_resources()
{
    plan=$( { printf '%s\n' "${MAKEFLAGS:-}"; printf '%s\n' "$@"; } | awk -v max="$((cpu / 1000))" -v jobs="${JOBS:-4}" '
        function take(t) {
            if (pending) {
                pending = 0
                if (t ~ /^[1-9][0-9]*$/) { j = t; return }
            }
            if (t == "-j" || t == "--jobs") { j = max; pending = 1 }
            else if (t ~ /^-j[0-9]+$/) j = substr(t, 3)
            else if (t ~ /^--jobs=/) j = substr(t, 8)
            else if (t ~ /^JOBS=/) jobs = substr(t, 6)
        }
        NR == 1 {
            n = split($0, flags, /[[:space:]]+/)
            for (i = 1; i <= n && flags[i] != "--"; i++) take(flags[i])
            pending = 0; next
        }
        { take($0) }
        END {
            if (j == "") j = 1
            if (jobs !~ /^[1-9][0-9]*$/ || j !~ /^[1-9][0-9]*$/) exit 1
            if (j + 0 > jobs + 0) jobs = j
            print jobs "," j
        }
    ') || refuse 'invalid build job count'
    jobs=${plan%,*}
    requested_jobs=${plan#*,}
    positive "$jobs" 'build job count'
    positive "$requested_jobs" 'make job count'
    [ "$jobs" -le 1024 ] || refuse 'invalid build job count'
    processors=$((jobs * 1000))
    memory=$(((jobs + 1) * 1073741824))
}

cpu=${capacity%%,*}
site_memory=${capacity#*,}; site_memory=${site_memory%%,*}
site_disk=${capacity##*,}
RACH2_WORKERS=${RACH2_WORKERS:-8}
RACH2_MEMORY_MIB=${RACH2_MEMORY_MIB:-16384}
positive "$RACH2_WORKERS" RACH2_WORKERS
[ "$RACH2_WORKERS" -le 1024 ] || refuse 'invalid RACH2_WORKERS'
positive "$RACH2_MEMORY_MIB" RACH2_MEMORY_MIB
mode=shared
# Preparation may grow a root and its images, including the GNU root.
disk=42949672960
build_memory=5368709120
case $kind in
    build)
        build_resources "$@"
        for goal do [ "$goal" != test ] || mode=alone; done
        case ${1##*/} in
            sh|bash|dash)
                build_processors=$processors build_claim=$memory
                run_share "$RACH2_MEMORY_MIB"
                [ "$processors" -ge "$build_processors" ] || processors=$build_processors
                [ "$memory" -ge "$build_claim" ] || memory=$build_claim
                ;;
        esac
        ;;
    legacy) legacy_resources "$@" ;;
    alone) mode=alone ;;
esac
make_real=$(command -v make) || refuse 'make is unavailable'
case $make_real in */etude-bin/make) make_real=${ETUDE_MAKE_REAL:-} ;; esac
ETUDE_MAKE_REAL=$make_real
case $ETUDE_MAKE_REAL in /*|[A-Za-z]:/*) ;; *) refuse 'make is not an absolute path' ;; esac
make_dir=${launcher%/*}/etude-bin
[ -x "$make_dir/make" ] || refuse 'installed make wrapper is unavailable'
PATH=$make_dir:$PATH
export ETUDE_MAKE_REAL PATH
ETUDE_BUILD_JOBS=${jobs:-$RACH2_WORKERS}
JOBS=$ETUDE_BUILD_JOBS
export ETUDE_BUILD_JOBS JOBS
if [ "$mode" = alone ]; then
    resources=$capacity
else
    [ "$processors" -le "$cpu" ] && [ "$memory" -le "$site_memory" ] && [ "$disk" -le "$site_disk" ] ||
        refuse 'launch exceeds site capacity'
    resources=$processors,$memory,$disk
fi
if [ "$kind" = build ]; then
    case ${1##*/} in make|gmake) set -- "$@" "-j$requested_jobs" ;; esac
fi
ETUDE_ADMITTED=$site
export ETUDE_ADMITTED RACH2_WORKERS RACH2_MEMORY_MIB
nonce=$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')
[ "${#nonce}" -eq 32 ] || refuse 'cannot make an attempt identity'
environment=$(command -v env) || refuse 'env is unavailable'
if [ -n "$result" ]; then
    exec "$bridge" -i -p -d "$locks" -w "$wait" -r "$result" "$ledger" \
        "launch:$nonce" "$(id -un)" "$mode" "$resources" "$capacity" "$seconds" "$environment" "$@"
fi
exec "$bridge" -i -p -d "$locks" -w "$wait" "$ledger" \
    "launch:$nonce" "$(id -un)" "$mode" "$resources" "$capacity" "$seconds" "$environment" "$@"
