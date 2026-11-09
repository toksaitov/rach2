#!/bin/sh
# deqp.sh: dEQP-GLES2, GLES3, GLES31 or VK of VK-GL-CTS, every Nth case of a
# list, in a few processes, inside a graphics guest, Rach2's or Debian's.
#
#   deqp.sh [-v VERSION] [-e N] [-j JOBS] [-l LIST] [-f FLAKES] [-c CTS]
#           [-o OUT] [-q] SUITE [OPTION...]
#
# SUITE is gles2, gles3, gles31 or vk. The cases are every Nth (1, every
# case) of LIST, counted from its first, as the second discovery run's step 9
# chose them: by default the release's mustpass list, the AOSP list
# GLESN-main for GLES and vk-default with its includes expanded for Vulkan,
# which the build stages compressed in CTS/mustpass; LIST is a file of one
# case a line, `#` beginning a comment. VERSION is 1.4.7.1, the Lenovo's
# references, or 1.4.1.1, Mesa 25.0.7's CI pin; CTS is
# /opt/vk-gl-cts-VERSION. GLES runs glcts and Vulkan deqp-vk, from their
# directories, with dEQP's watchdog on at 300 s a case, its own limit, the
# OPTIONs after SUITE added. GLES draws to 256x256 pbuffers of the config
# rgba8888d24s8ms0 on EGL's surfaceless platform, as Mesa's CI runs it and
# step 9 did; Vulkan takes the ICDs the environment names.
#
# JOBS processes (4) run at once, each given every JOBSth case of the
# sample. A process that dies runs again on the cases it had left, and the
# case it was running is `Crash`, or `Timeout` when dEQP's watchdog ended
# it; a process that ends with cases left and none running, twice in a row
# without a case done, leaves those cases `Missing`, as not in the program.
# Each case that does not pass, Missing aside, then runs once more; the
# first status is its result, and a case that passes the second time is a
# flake. So is one that does not pass and matches a line of FLAKES, an
# extended regular expression.
#
# OUT (.) gets the record; results.csv.gz, `CASE,STATUS` a line in the
# sample's order, the form of step 9's results; flakes, `CASE,FIRST,SECOND,WHY`
# a line, WHY being `rerun` or `named`; summary.txt, the count of each status;
# and logs/, each process's output compressed, with its .qpa logs when -q
# is given. It prints one line:
#
#   deqp: result: SUITE VERSION cases N of M every E jobs J pass P fail F
#       notsupported S warning W crash C timeout T other O missing X flake K
#
# where a warning is QualityWarning or CompatibilityWarning, and other is
# every other status. The status is 0 when every case ran and 1 otherwise;
# a case's failure is a result, which `judge.py deqp` compares with a
# reference's.
set -eu

RUNNER_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=common.sh
. "$RUNNER_DIR/common.sh"

usage()
{
    echo "usage: deqp.sh [-v VERSION] [-e N] [-j JOBS] [-l LIST] [-f FLAKES] [-c CTS] [-o OUT] [-q] SUITE [OPTION...]" >&2
    exit 2
}

version=1.4.7.1 every=1 jobs=4 list='' flakes='' cts='' out=. keep=0
while getopts v:e:j:l:f:c:o:q o; do
    case $o in
        v) version=$OPTARG ;;
        e) every=$OPTARG ;;
        j) jobs=$OPTARG ;;
        l) list=$OPTARG ;;
        f) flakes=$OPTARG ;;
        c) cts=$OPTARG ;;
        o) out=$OPTARG ;;
        q) keep=1 ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))
[ $# -ge 1 ] || usage
suite=$1
shift
case $every$jobs in *[!0-9]*) usage ;; esac
[ "$every" -ge 1 ] && [ "$jobs" -ge 1 ] || usage
cts=${cts:-/opt/vk-gl-cts-$version}
case $suite in
    gles2 | gles3 | gles31)
        dir=$cts/gl prog=glcts package=dEQP-$(echo "$suite" | tr gles GLES)
        info="$package.info.renderer $package.info.version"
        set -- --deqp-surface-type=pbuffer --deqp-gl-config-name=rgba8888d24s8ms0 \
            --deqp-surface-width=256 --deqp-surface-height=256 "$@"
        EGL_PLATFORM=surfaceless
        export EGL_PLATFORM
        ;;
    vk)
        dir=$cts/vulkan prog=deqp-vk package=dEQP-VK info=dEQP-VK.info.device
        ;;
    *)
        echo "deqp: no suite $suite; gles2, gles3, gles31 or vk" >&2
        exit 2
        ;;
esac
[ -x "$dir/$prog" ] || { echo "deqp: no $dir/$prog" >&2; exit 1; }
if [ -z "$list" ]; then
    case $suite in
        vk) list=$cts/mustpass/vk-default.txt.gz ;;
        *) list=$cts/mustpass/$suite-main.txt.gz ;;
    esac
fi
[ -f "$list" ] || { echo "deqp: no list $list" >&2; exit 1; }
[ -z "$flakes" ] || [ -f "$flakes" ] || { echo "deqp: no flakes file $flakes" >&2; exit 1; }
options="--deqp-watchdog=enable --deqp-watchdog-total-time-limit=300 --deqp-watchdog-interval-time-limit=300 --deqp-log-images=disable --deqp-log-shader-sources=disable $*"

DEQP_PIDS=
# deqp_cleanup: end the runs still going and the dEQP processes they started,
# whose process IDs are in the work directory's .pid files, then the
# runner's own.
deqp_cleanup()
{
    for pid in $DEQP_PIDS; do
        kill -TERM "$pid" 2>/dev/null || true
    done
    for f in "${RUNNER_WORK:-/nonexistent}"/*.pid; do
        [ ! -f "$f" ] || kill -KILL "$(cat "$f")" 2>/dev/null || true
    done
    for pid in $DEQP_PIDS; do
        wait "$pid" 2>/dev/null || true
    done
    runner_cleanup
}

runner_init deqp "$out"
trap deqp_cleanup EXIT
runner_start plain
work=$RUNNER_WORK
mkdir -p "$RUNNER_OUT/logs"
logs=$work/logs
mkdir -p "$logs"
case $list in
    *.gz) gzip -d < "$list" ;;
    *) cat "$list" ;;
esac | sed -e 's/#.*//' -e 's/[[:space:]]*$//' -e '/^$/d' > "$work/list"
all=$(wc -l < "$work/list" | tr -d ' ')
awk -v e="$every" '(NR - 1) % e == 0' "$work/list" > "$work/sample"
cases=$(wc -l < "$work/sample" | tr -d ' ')
[ "$cases" -gt 0 ] || runner_die "the list $list has no case"
runner_record "case deqp $suite $version every $every of $list, $jobs processes"
runner_record "list $list $all cases sha256 $(sha256sum < "$work/list" | awk '{ print $1 }')"
runner_record "sample $cases cases sha256 $(sha256sum < "$work/sample" | awk '{ print $1 }')"
runner_record "program $dir/$prog $(grep '^release ' "$cts/build-info" 2>/dev/null | cut -d' ' -f2)"
runner_record "options $options"
for v in EGL_PLATFORM VK_DRIVER_FILES VK_ICD_FILENAMES MESA_LOADER_DRIVER_OVERRIDE \
    GALLIUM_DRIVER LIBGL_ALWAYS_SOFTWARE LP_NUM_THREADS; do
    eval "x=\${$v-}"
    # shellcheck disable=SC2154 # x is set by eval
    [ -z "$x" ] || runner_record "env $v=$x"
done
[ -z "$flakes" ] || runner_record "flakes $flakes $(grep -c -v -E '^[[:space:]]*(#|$)' "$flakes") patterns"

# parse OUT: the cases of a process's output OUT that ended, as CASE,STATUS,
# with the case it was running, if any, as Crash, or Timeout when dEQP's
# watchdog ended it.
parse()
{
    awk '
        /^Test case \047/ {
            cur = substr($0, 12)
            sub(/\047\.\.$/, "", cur)
            next
        }
        cur != "" && /^  (Pass|Fail|QualityWarning|CompatibilityWarning|Pending|NotSupported|ResourceError|InternalError|Crash|Timeout|Waiver|DeviceLost) \(/ {
            print cur "," $1
            cur = ""
            next
        }
        /Watchdog timer timeout/ { wd = 1 }
        END { if (cur != "") print cur "," (wd ? "Timeout" : "Crash") }' "$1"
}

# run_cases NAME TODO RESULTS: run the cases of TODO in one process after
# another, each given the cases the last left, until each has a status in
# RESULTS. NAME names the logs.
run_cases()
{
    name=$1 todo=$2 res=$3
    n=0 idle=0
    : > "$res"
    while [ -s "$todo" ]; do
        n=$((n + 1))
        log=$logs/$name-$n
        st=0
        # shellcheck disable=SC2086 # options are words
        (cd "$dir" && exec "./$prog" --deqp-caselist-file="$todo" --deqp-log-filename="$log.qpa" \
            $options) > "$log.out" 2>&1 < /dev/null &
        echo $! > "$work/$name.pid"
        wait $! || st=$?
        rm -f "$work/$name.pid"
        parse "$log.out" > "$log.done"
        if [ ! -s "$log.done" ]; then
            if [ "$n" -eq 1 ]; then
                echo "deqp: $name: $prog ran no case, status $st: $(tail -n 3 "$log.out")" >&2
                return 1
            fi
            idle=$((idle + 1))
        else
            idle=0
        fi
        cat "$log.done" >> "$res"
        awk -F, 'NR == FNR { done[$1]; next } !($0 in done)' "$log.done" "$todo" > "$todo.left"
        mv "$todo.left" "$todo"
        if [ -s "$todo" ] && { [ "$idle" -ge 2 ] || { [ "$st" -eq 0 ] && [ "$idle" -ge 1 ]; }; }; then
            sed 's/$/,Missing/' "$todo" >> "$res"
            : > "$todo"
        fi
        [ "$keep" -eq 1 ] || rm -f "$log.qpa"
    done
}

# run_all NAME FILE RESULTS: run the cases of FILE in $jobs processes at
# once, each given every $jobs-th case, and put their statuses in RESULTS.
run_all()
{
    DEQP_PIDS=
    j=0
    while [ "$j" -lt "$jobs" ]; do
        awk -v j="$j" -v n="$jobs" '(NR - 1) % n == j' "$2" > "$work/$1-$j.todo"
        if [ -s "$work/$1-$j.todo" ]; then
            run_cases "$1-$j" "$work/$1-$j.todo" "$work/$1-$j.res" &
            DEQP_PIDS="$DEQP_PIDS $!"
        fi
        j=$((j + 1))
    done
    failed=0
    for pid in $DEQP_PIDS; do
        wait "$pid" || failed=1
    done
    DEQP_PIDS=
    [ "$failed" -eq 0 ] || runner_die "a process ran no case; its log is above"
    cat "$work/$1"-*.res > "$3"
}

# The renderer, from dEQP's own information cases.
mkdir -p "$work/info"
for c in $info; do
    # shellcheck disable=SC2086 # options are words
    (cd "$dir" && exec "./$prog" -n "$c" --deqp-log-filename="$work/info/$c.qpa" $options) \
        > "$work/info/$c.out" 2>&1 < /dev/null || true
    case $c in
        *.info.device)
            for k in deviceName apiVersion driverVersion; do
                runner_record "renderer $k $(sed -n "s/.*$k: \\(.*\\)/\\1/p" "$work/info/$c.qpa" | sed 's/<\/Text>.*//' | head -n 1)"
            done
            ;;
        *)
            runner_record "renderer ${c##*.} $(sed -n 's/.*<Text>\(.*\)<\/Text>.*/\1/p' "$work/info/$c.qpa" | head -n 1)"
            ;;
    esac
done

echo "deqp: running $cases of $all cases of $suite in $jobs processes" >&2
t0=$(date +%s)
run_all first "$work/sample" "$work/first"
# The second run: every case that did not pass, Missing aside.
awk -F, '$2 !~ /^(Pass|NotSupported|QualityWarning|CompatibilityWarning|Missing)$/ { print $1 }' \
    "$work/first" > "$work/again"
: > "$work/second"
if [ -s "$work/again" ]; then
    echo "deqp: running $(wc -l < "$work/again" | tr -d ' ') cases again" >&2
    run_all second "$work/again" "$work/second"
fi
t1=$(date +%s)

# The results in the sample's order, the flakes and the counts.
awk -F, 'NR == FNR { s[$1] = $2; next } { print $0 "," (($0 in s) ? s[$0] : "Missing") }' \
    "$work/first" "$work/sample" > "$work/results.csv"
gzip -n -9 < "$work/results.csv" > "$RUNNER_OUT/results.csv.gz"
pass='^(Pass|NotSupported|QualityWarning|CompatibilityWarning)$'
awk -F, -v ok="$pass" 'NR == FNR { s[$1] = $2; next }
    $2 !~ ok && $2 != "Missing" && ($1 in s) && s[$1] ~ ok { print $1 "," $2 "," s[$1] ",rerun" }' \
    "$work/second" "$work/results.csv" > "$work/flakes"
if [ -n "$flakes" ]; then
    awk -F, -v ok="$pass" -v pf="$flakes" '
        BEGIN {
            while ((getline l < pf) > 0)
                if (l !~ /^[[:space:]]*(#|$)/)
                    p[++np] = l
        }
        NR == FNR { s[$1] = $2; next }
        $2 !~ ok && $2 != "Missing" && !(($1 in s) && s[$1] ~ ok) {
            for (i = 1; i <= np; i++)
                if ($1 ~ p[i]) {
                    print $1 "," $2 "," (($1 in s) ? s[$1] : "-") ",named"
                    break
                }
        }' "$work/second" "$work/results.csv" >> "$work/flakes"
fi
cp "$work/flakes" "$RUNNER_OUT/flakes"
{
    echo "cases $cases of $all (every ${every}th), wall $((t1 - t0)) s, jobs $jobs"
    cut -d, -f2 "$work/results.csv" | LC_ALL=C sort | uniq -c | awk '{ print $2, $1 }'
} > "$RUNNER_OUT/summary.txt"
for f in "$logs"/*.out; do
    gzip -n -9 < "$f" > "$RUNNER_OUT/logs/${f##*/}.gz"
done
if [ "$keep" -eq 1 ]; then
    for f in "$logs"/*.qpa "$work"/info/*.qpa; do
        [ ! -f "$f" ] || gzip -n -9 < "$f" > "$RUNNER_OUT/logs/${f##*/}.gz"
    done
fi
runner_finish

count()
{
    awk -F, -v re="$1" '$2 ~ re { n++ } END { print n + 0 }' "$work/results.csv"
}
missing=$(count '^Missing$')
other=$(count '^(Pending|ResourceError|InternalError|Waiver|DeviceLost)$')
echo "deqp: result: $suite $version cases $cases of $all every $every jobs $jobs" \
    "pass $(count '^Pass$') fail $(count '^Fail$') notsupported $(count '^NotSupported$')" \
    "warning $(count '^(QualityWarning|CompatibilityWarning)$') crash $(count '^Crash$')" \
    "timeout $(count '^Timeout$') other $other missing $missing flake $(wc -l < "$work/flakes" | tr -d ' ')"
[ "$missing" -eq 0 ]
