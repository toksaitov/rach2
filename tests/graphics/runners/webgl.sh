#!/bin/sh
# webgl.sh: the WebGL conformance suite in chunks, each in a new browser,
# inside a graphics guest, Rach2's or Debian's.
#
#   webgl.sh [-b BROWSER] [-v VERSION] [-k SIZE] [-s SUITE] [-o OUT] CHUNK|all
#
# Runs chunk CHUNK, counted from 0, of SIZE pages (250) of the suite's list
# for VERSION (2.0.1; or 1.0.4), or every chunk one after another with
# `all`, in BROWSER (chromium; or firefox) under a headless Weston, the
# suite served from SUITE (/usr/share/webgl/sdk/tests) on the loopback
# address. Each chunk starts a new browser with a new profile, which
# webgl.html drives through the suite's own harness. A browser that ends
# before its chunk does, as Chromium did once in a whole 2.0.1 run of the
# discovery run and failed 163 pages after it, is replaced by a new one for
# the pages left, and the page it was running is `crash`. Each page that
# does not pass is then run once more in a new browser, and its second
# result is its verdict; the first is kept beside it.
#
# The harness ends a page after 20 s without a result, which webgl.html
# reports as `timeout`, a status like any other, never a verdict on the run.
# A browser that posts nothing for 120 s, six times the harness's limit, has
# stopped: it is ended, and the page it was running is `hung`.
#
# OUT (.) gets the record and, for each chunk, BROWSER-VERSION-NN.pages, one
# line a page in the list's order: `INDEX STATUS PASSED FAILED SKIPPED
# TIMEOUTS FIRST URL`, STATUS being pass, fail, timeout, skip, empty (no
# result at all), crash, hung or notrun, and FIRST the status of the first
# run. Each chunk prints one line:
#
#   webgl: result: BROWSER VERSION chunk N of M pages P pass A fail F
#       timeout T crash C hung H skip S empty E notrun R rerun X
#       subtests passed SP failed SF
#
# The status is 0 when every chunk ran and 1 otherwise; a page's failure is
# a result, which judge.py compares with the reference's.
set -eu

RUNNER_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=common.sh
. "$RUNNER_DIR/common.sh"

browser=chromium version=2.0.1 size=250 suite=/usr/share/webgl/sdk/tests out=.
silence=120
while getopts b:v:k:s:o: o; do
    case $o in
        b) browser=$OPTARG ;;
        v) version=$OPTARG ;;
        k) size=$OPTARG ;;
        s) suite=$OPTARG ;;
        o) out=$OPTARG ;;
        *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 1 ] || { echo "usage: webgl.sh [-b BROWSER] [-v VERSION] [-k SIZE] [-s SUITE] [-o OUT] CHUNK|all" >&2; exit 2; }
which=$1
case $browser in chromium|firefox) ;; *) echo "webgl: no browser $browser" >&2; exit 2 ;; esac
case $version in 1.0.4|2.0.1) ;; *) echo "webgl: no version $version; 1.0.4 or 2.0.1" >&2; exit 2 ;; esac
[ -f "$suite/00_test_list.txt" ] || { echo "webgl: no suite at $suite" >&2; exit 1; }

runner_init webgl "$out"
runner_start
runner_record "case webgl $browser $version chunk $which of $size pages"
runner_record "warmup 0"
runner_record "repetitions 1, and each page that does not pass once more"
runner_record "suite $suite"
runner_browser_record "$browser"
runner_weston
runner_server "$suite" "webgl-runner.html=$RUNNER_DIR/webgl.html"
events=$RUNNER_WORK/events
lines=0
count=

# run_pages INDEX...: run the pages in new browsers until each has a result,
# and leave their lines in $RUNNER_WORK/attempt.
run_pages()
{
    : > "$RUNNER_WORK/attempt"
    todo=$*
    empty=0
    while [ -n "$todo" ]; do
        # shellcheck disable=SC2086 # indexes are words
        only=$(echo $todo | tr ' ' ,)
        runner_browser "$browser" "http://127.0.0.1:$RUNNER_PORT/webgl-runner.html?version=$version&only=$only" \
            "$RUNNER_OUT/browser.log"
        # shellcheck disable=SC2046 # two words
        set -- $(python3 -I "$RUNNER_DIR/events.py" wait "$events" "$lines" "$RUNNER_BROWSER" "$silence")
        state=$1
        runner_stop
        before=$(wc -l < "$RUNNER_WORK/attempt")
        python3 -I "$RUNNER_DIR/events.py" pages "$events" "$lines" "$2" "$state" >> "$RUNNER_WORK/attempt"
        lines=$2
        echo "webgl: browser: $state after $(($(wc -l < "$RUNNER_WORK/attempt") - before)) pages" >&2
        if [ -z "$count" ]; then
            # shellcheck disable=SC2046 # two words
            set -- $(python3 -I "$RUNNER_DIR/events.py" list "$events")
            if [ "$1" -gt 0 ]; then
                count=$1
                runner_record "list $version $1 pages sha256 $2"
                python3 -I "$RUNNER_DIR/events.py" renderer "$events" >> "$RUNNER_OUT/record"
            fi
        fi
        left=
        for i in $todo; do
            [ -z "$count" ] || [ "$i" -lt "$count" ] || continue
            awk -v i="$i" '$1 == i { found = 1 } END { exit !found }' "$RUNNER_WORK/attempt" || left="$left $i"
        done
        if [ "$(wc -l < "$RUNNER_WORK/attempt")" -eq "$before" ]; then
            empty=$((empty + 1))
        else
            empty=0
        fi
        if [ "$state" = "done" ] || [ "$empty" -ge 2 ]; then
            # The harness ended, or two browsers in a row ran nothing.
            for i in $left; do
                echo "$i notrun 0 0 0 0 -" >> "$RUNNER_WORK/attempt"
            done
            left=
        fi
        todo=$left
    done
}

# chunk N: run chunk N and write its pages file and line; fail when the
# chunk is past the list's end.
chunk()
{
    n=$1
    first=$((n * size))
    run_pages $(seq "$first" $((first + size - 1)))
    [ -n "$count" ] || runner_die "the harness gave no list"
    [ "$first" -lt "$count" ] || runner_die "chunk $n starts past the list's $count pages"
    chunks=$(((count + size - 1) / size))
    sort -n -k1,1 "$RUNNER_WORK/attempt" > "$RUNNER_WORK/first"
    again=$(awk '$2 != "pass" && $2 != "skip" && $2 != "notrun" { print $1 }' "$RUNNER_WORK/first")
    : > "$RUNNER_WORK/again"
    if [ -n "$again" ]; then
        # shellcheck disable=SC2086 # indexes are words
        run_pages $again
        sort -n -k1,1 "$RUNNER_WORK/attempt" > "$RUNNER_WORK/again"
    fi
    name=$browser-$version-$(printf '%02d' "$n")
    awk 'FILENAME == ARGV[1] { again[$1] = $0; next }
         { first = $2; line = ($1 in again) ? again[$1] : $0; split(line, f, " ")
           print f[1], f[2], f[3], f[4], f[5], f[6], first, $7 }' \
        "$RUNNER_WORK/again" "$RUNNER_WORK/first" > "$RUNNER_OUT/$name.pages"
    awk -v b="$browser" -v v="$version" -v n="$n" -v m="$chunks" '
        { s[$2]++; p += $3; f += $4; if ($7 != "pass" && $7 != "skip" && $7 != "notrun") x++ }
        END {
            printf "webgl: result: %s %s chunk %d of %d pages %d pass %d fail %d timeout %d crash %d hung %d skip %d empty %d notrun %d rerun %d subtests passed %d failed %d\n",
                b, v, n, m, NR, s["pass"], s["fail"], s["timeout"], s["crash"], s["hung"],
                s["skip"], s["empty"], s["notrun"], x, p, f
        }' "$RUNNER_OUT/$name.pages" | tee -a "$RUNNER_OUT/results"
    ! grep -q ' notrun ' "$RUNNER_OUT/$name.pages"
}

status=0
if [ "$which" = all ]; then
    chunk 0 || status=1
    n=1
    while [ "$n" -lt "$chunks" ]; do
        chunk "$n" || status=1
        n=$((n + 1))
    done
else
    chunk "$which" || status=1
fi
runner_finish
exit "$status"
