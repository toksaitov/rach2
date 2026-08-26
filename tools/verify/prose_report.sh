#!/usr/bin/env sh

#
# prose_report.sh: the measured half of the prose check.
#
# `prosecheck banned` fails a build on terms that are wrong at any
# frequency. This is the other half: a watch list of ordinary phrases,
# measured and never gated, because a high multiple is a question to ask
# and not a defect. Teaching prose compares and negates more often than
# terse implementation comments do, which is most of what `which is` and
# `not a` measure.
#
# The baseline is the upstream sources this repository vendors: musl,
# busybox, dropbear, tinycc, pdpmake, and the Doom engine. They are human
# comment prose in the same domain, written by people with no connection to
# this project, which is what makes them a control. They are read from the
# PINNED TARBALLS and not from the extracted directories, because
# extraction happens during a build and a fresh clone has only the
# tarballs; a check that needed the extracted trees could not run on the
# machine this exists to serve. Each tarball is unpacked into a temporary
# directory for the reading and the directory is removed afterwards.
#
# Three populations are reported and never pooled: C and header comments,
# the comments in shell, make and Python, and Markdown prose. Only the
# first has an upstream counterpart, so only the first gets a ratio.
# Comparing our Markdown to upstream C comments would compare a tutorial to
# a register description.
#
# Usage:
#     prose_report.sh                  the report
#     prose_report.sh --baseline       compare with the committed record
#     prose_report.sh --baseline --write
#     prose_report.sh --fixture        verify the committed upstream counts
#     prose_report.sh --fixture --write
#

set -eu

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
PROSECHECK="$REPO/build/tools/verify/prosecheck"
BASELINE="$REPO/tools/verify/prose_baseline.txt"
FIXTURE="$REPO/tools/verify/prose_upstream.txt"

if [ ! -x "$PROSECHECK" ] || [ "$REPO/tools/verify/prosecheck.c" -nt "$PROSECHECK" ]; then
    mkdir -p "${PROSECHECK%/*}"
    cc -O2 -o "$PROSECHECK" "$REPO/tools/verify/prosecheck.c" || exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/rach2_prose.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT INT TERM

cd "$REPO"

WATCH="$("$PROSECHECK" watchlist)"
N_WATCH="$(printf '%s\n' "$WATCH" | wc -w | tr -d ' ')"

# The in-house roots, and the three top-level documents the Markdown
# population includes.
OURS="rach2 utils ish krffs tests tools mk ports/doom CLAUDE.md AGENTS.md Readme.md"

# `words N` and one `phrase N` per watched phrase, as one line of numbers.
counts_of() {
    # shellcheck disable=SC2086 # the roots are a deliberate word list
    "$PROSECHECK" count "$1" $2 | awk '{ printf "%s%s", sep, $2; sep = " " }
                                       END { print "" }'
}

# One upstream project's C and header comments, from its pinned tarball.
upstream_counts() {
    dir="$WORK/up.$1"
    rm -rf "$dir"
    mkdir -p "$dir"
    case "$2" in
        *.tar.gz|*.tgz) tar -xzf "$2" -C "$dir" ;;
        *.tar.bz2)      tar -xjf "$2" -C "$dir" ;;
        *.tar.xz)       tar -xJf "$2" -C "$dir" ;;
        *)              echo "prose_report: $2 is not a tarball" >&2; exit 1 ;;
    esac
    counts_of c "$dir"
    rm -rf "$dir"
}

# Every vendored project with a pinned tarball, plus the tracked Doom
# sources, in name order.
project_names() {
    for d in "$REPO"/vendor/*/; do
        name="$(basename "$d")"
        ball="$(find "$d" -maxdepth 1 \( -name '*.tar.gz' -o -name '*.tgz' \
            -o -name '*.tar.bz2' -o -name '*.tar.xz' \) | sort | head -1)"
        [ -n "$ball" ] || continue
        printf '%s %s\n' "$name" "$ball"
    done
    [ -d "$REPO/ports/doom/src" ] && printf 'doom %s\n' "$REPO/ports/doom/src"
}

# The upstream table: one line per project, `name words counts...`.
upstream_table() {
    project_names | sort | while read -r name where; do
        if [ "$name" = doom ]; then
            printf '%s %s\n' "$name" "$(counts_of c "$where")"
        else
            printf '%s %s\n' "$name" "$(upstream_counts "$name" "$where")"
        fi
    done
}

# The committed fixture's shape, for --fixture.
fixture_lines() {
    printf '%s\n' \
        "# Upstream comment prose, counted from the pinned tarballs and the" \
        "# tracked Doom sources. Regenerate deliberately, and only when a" \
        "# tarball is repinned: prose_report.sh --fixture --write" \
        "#" \
        "# project words $WATCH"
    upstream_table
}

baseline_lines() {
    printf '%s\n' \
        "# Watched phrases, per population, at the end of milestone 30." \
        "# A record, not a budget: only the banned terms fail a build." \
        "# Regenerate: prose_report.sh --baseline --write" \
        "#" \
        "# population words $WATCH"
    for pop in c hash md; do
        printf '%s %s\n' "$pop" "$(counts_of "$pop" "$OURS")"
    done
}

case "${1:-}" in
    --fixture)
        fixture_lines > "$WORK/fixture.txt"
        if [ "${2:-}" = --write ]; then
            cp "$WORK/fixture.txt" "$FIXTURE"
            echo "prose_report: wrote tools/verify/prose_upstream.txt"
            exit 0
        fi
        [ -f "$FIXTURE" ] || {
            echo "prose_report: $FIXTURE is missing" >&2; exit 1; }
        if ! diff -u "$FIXTURE" "$WORK/fixture.txt" > "$WORK/diff"; then
            echo "prose_report: the upstream counts moved from the committed fixture" >&2
            cat "$WORK/diff" >&2
            exit 1
        fi
        echo "prose_report: fixture OK ($(grep -c '^[a-z]' "$FIXTURE") projects)"
        exit 0
        ;;
    --baseline)
        baseline_lines > "$WORK/baseline.txt"
        if [ "${2:-}" = --write ]; then
            cp "$WORK/baseline.txt" "$BASELINE"
            echo "prose_report: wrote tools/verify/prose_baseline.txt"
            exit 0
        fi
        [ -f "$BASELINE" ] || { cat "$WORK/baseline.txt"; exit 0; }
        # What moved since the record was written, phrase by phrase. This
        # is a record and not a limit: only the banned terms fail a build.
        printf '%-10s %-14s %8s %8s %8s\n' population phrase 'then' now change
        awk -v names="words $WATCH" '
            FNR == NR {
                if ($0 ~ /^#/ || $0 == "") next
                for (i = 2; i <= NF; i++) then[$1, i] = $i
                next
            }
            $0 ~ /^#/ || $0 == "" { next }
            {
                split(names, name, " ")
                for (i = 2; i <= NF; i++)
                    if ((($1, i) in then) && then[$1, i] != $i)
                        printf "%-10s %-14s %8d %8d %+8d\n", $1,
                            name[i - 1], then[$1, i], $i, $i - then[$1, i]
            }' "$BASELINE" "$WORK/baseline.txt"
        echo "prose_report: compared against tools/verify/prose_baseline.txt"
        exit 0
        ;;
    "")
        ;;
    *)
        echo "usage: prose_report.sh [--baseline [--write] | --fixture [--write]]" >&2
        exit 2
        ;;
esac

# The report itself.
upstream_table > "$WORK/up.txt"
UPWORDS="$(awk '{ n += $2 } END { print n + 0 }' "$WORK/up.txt")"

echo "upstream corpus, C and header comments, from the pinned tarballs:"
awk '{ printf "  %-12s %9d words\n", $1, $2 }' "$WORK/up.txt"
printf '  %-12s %9d words\n' TOTAL "$UPWORDS"
echo

# The upstream totals per phrase, summed across the projects.
UPSUM="$(awk -v n="$N_WATCH" '
    { for (i = 3; i <= NF; i++) s[i] += $i }
    END { for (i = 3; i <= n + 2; i++) printf "%s%d", (i > 3 ? " " : ""), s[i]
          print "" }' "$WORK/up.txt")"

report_population() {
    pop="$1"
    label="$2"
    line="$(counts_of "$pop" "$OURS")"
    words="$(printf '%s' "$line" | awk '{print $1}')"

    printf '%s: %d words\n' "$label" "$words"
    if [ "$pop" = c ]; then
        printf '  %-14s %8s %10s %8s %8s\n' phrase count ours/1k up/1k ratio
    else
        printf '  %-14s %8s %10s   (no upstream counterpart)\n' \
            phrase count ours/1k
    fi
    printf '%s\n%s\n%s\n%s\n' "$WATCH" "$line" "$UPSUM" "$UPWORDS" |
        awk -v pop="$pop" -v words="$words" '
            NR == 1 { split($0, name, " "); next }
            NR == 2 { split($0, ours, " "); next }
            NR == 3 { split($0, up, " "); next }
            NR == 4 {
                uw = $1
                for (i = 1; i <= length(name); i++) {
                    phrase = name[i]
                    gsub(/_/, " ", phrase)
                    n = ours[i + 1]
                    rate = words ? n * 1000.0 / words : 0
                    if (pop == "c" && uw) {
                        urate = up[i] * 1000.0 / uw
                        ratio = urate ? sprintf("%.1fx", rate / urate) : "n/a"
                        printf "  %-14s %8d %10.2f %8.2f %8s\n", phrase, n,
                            rate, urate, ratio
                    } else {
                        printf "  %-14s %8d %10.2f\n", phrase, n, rate
                    }
                }
            }'
    echo
}

report_population c "C and header comments"
report_population hash "shell, make and Python comments"
report_population md "Markdown prose"
