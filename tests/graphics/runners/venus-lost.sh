#!/bin/sh
# venus-lost.sh: what Vulkan programs on Venus see when the host's render
# server dies under them, in a graphics guest, Rach2's or Debian's, and the
# host's half that kills it.
#
#   venus-lost.sh start [-l LIST] [-c CTS] [-o OUT]
#   venus-lost.sh after [-o OUT]
#   venus-lost.sh run [-l LIST] [-c CTS] [-o OUT]
#   venus-lost.sh kill [WORKSPACE]
#
# `start` runs four deqp-vk processes of CTS (/opt/vk-gl-cts-1.4.1.1) on
# Venus's ICD alone, in a session of their own, each on every fourth of the
# first 8000 cases of LIST (/tmp/graphics/deqp-vk-lavapipe.txt.gz) that are
# not dEQP-VK.info cases, and returns once each has finished a case, so
# that all four are inside Venus when the render server dies. `after`, run
# once the host has killed it, waits for the four to end, runs
# `vulkaninfo --summary` again and writes to OUT/lost.txt, and prints:
#
#   deqp N exit STATUS
#   vulkaninfo exit STATUS
#   vulkaninfo: ERROR LINE
#
# for N from 0 to 3, STATUS being the shell's, 134 for SIGABRT, and the
# error line vulkaninfo's first line that names an error, without its
# source location, or `vulkaninfo: none`. OUT (/tmp/graphics/out) keeps
# each process's output and .qpa log. `run` is `start` and then `after` in
# one command, for a guest whose commands may leave no process behind, as
# etude-guest reaps every process a command started when it ends.
#
# `kill`, on the host, finds the QEMU whose command line names WORKSPACE, a
# case's own directory, by default the one above the script's, as etude
# copies a case's artifacts into its bin/, and kills with SIGKILL its
# virgl_render_server processes, the server QEMU started and the one it
# forked for each context, and prints each, as the Debian reference's run
# killed its own.
set -eu

lost_usage()
{
    echo "usage: venus-lost.sh start|run [-l LIST] [-c CTS] [-o OUT] | after [-o OUT] | kill [WORKSPACE]" >&2
    exit 2
}

lost_die()
{
    echo "venus-lost: $*" >&2
    exit 1
}

# lost_tree PID: PID and every process below it, one a line.
lost_tree()
{
    echo "$1"
    for c in $(pgrep -P "$1" || true); do
        lost_tree "$c"
    done
}

lost_kill()
{
    [ $# -le 1 ] || lost_usage
    ws=$(cd "${1:-$(dirname "$0")/..}" && pwd -P) || lost_die "no workspace ${1:-}"
    q=
    for p in $(pgrep -u "$(id -u)" qemu-system || true); do
        if tr '\0' '\n' < "/proc/$p/cmdline" 2>/dev/null | grep -qF "$ws/"; then
            [ -z "$q" ] || lost_die "more than one QEMU names $ws"
            q=$p
        fi
    done
    [ -n "$q" ] || lost_die "no QEMU names $ws"
    pids=
    for r in $(pgrep -P "$q" virgl_render_s || true); do
        pids="$pids $(lost_tree "$r" | tr '\n' ' ')"
    done
    [ -n "$pids" ] || lost_die "QEMU $q has no render server"
    for p in $pids; do
        echo "venus-lost: kill: $(ps -o pid=,ppid=,args= -p "$p")"
    done
    # shellcheck disable=SC2086
    kill -KILL $pids
    echo "venus-lost: result: killed$pids of QEMU $q"
}

lost_start()
{
    list=/tmp/graphics/deqp-vk-lavapipe.txt.gz
    cts=/opt/vk-gl-cts-1.4.1.1
    out=/tmp/graphics/out
    while getopts l:c:o: o; do
        case $o in
        l) list=$OPTARG ;;
        c) cts=$OPTARG ;;
        o) out=$OPTARG ;;
        *) lost_usage ;;
        esac
    done
    [ -r "$list" ] || lost_die "no list $list"
    [ -x "$cts/vulkan/deqp-vk" ] || lost_die "no deqp-vk in $cts"
    mkdir -p "$out"
    rm -f "$out"/lost.*
    gzip -dc "$list" | grep -v -e '^#' -e '\.info\.' | head -n 8000 > "$out/lost.all"
    for j in 0 1 2 3; do
        awk -v j=$j 'NR % 4 == j' "$out/lost.all" > "$out/lost.$j.txt"
        # The processes outlive this command, in a session of their own.
        # shellcheck disable=SC2016
        setsid sh -c 'cd "$1/vulkan" && ./deqp-vk --deqp-caselist-file="$2/lost.$3.txt" \
            --deqp-log-filename="$2/lost.$3.qpa" --deqp-log-images=disable \
            --deqp-log-shader-sources=disable > "$2/lost.$3.out" 2>&1
            echo "exit $?" > "$2/lost.$3.exit"' sh "$cts" "$out" $j < /dev/null > /dev/null 2>&1 &
    done
    for j in 0 1 2 3; do
        until grep -q '^#endTestCaseResult' "$out/lost.$j.qpa" 2>/dev/null; do
            [ ! -f "$out/lost.$j.exit" ] || lost_die "deqp $j ended before its first case: $(cat "$out/lost.$j.exit")"
            sleep 1
        done
    done
    echo "venus-lost: result: 4 deqp-vk processes running"
}

lost_after()
{
    out=/tmp/graphics/out
    while getopts o: o; do
        case $o in
        o) out=$OPTARG ;;
        *) lost_usage ;;
        esac
    done
    for j in 0 1 2 3; do
        until [ -f "$out/lost.$j.exit" ]; do
            sleep 1
        done
    done
    {
        for j in 0 1 2 3; do
            echo "deqp $j $(cat "$out/lost.$j.exit")"
        done
        status=0
        vulkaninfo --summary > "$out/lost.vulkaninfo" 2>&1 || status=$?
        echo "vulkaninfo exit $status"
        line=$(grep -m 1 -i 'error' "$out/lost.vulkaninfo" | sed 's/^ERROR at [^:]*:[0-9]*:/ERROR:/' || true)
        echo "vulkaninfo: ${line:-none}"
    } > "$out/lost.txt"
    cat "$out/lost.txt"
}

[ $# -ge 1 ] || lost_usage
verb=$1
shift
case $verb in
start)
    export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/venus_icd.x86_64.json
    lost_start "$@"
    ;;
after)
    export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/venus_icd.x86_64.json
    lost_after "$@"
    ;;
run)
    export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/venus_icd.x86_64.json
    lost_start "$@"
    # lost_start leaves its output directory in out.
    OPTIND=1
    lost_after -o "$out"
    ;;
kill) lost_kill "$@" ;;
*) lost_usage ;;
esac
