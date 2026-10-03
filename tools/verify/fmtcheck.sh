#!/usr/bin/env sh
#
# fmtcheck.sh: refuse a kernel message the kernel's formatter cannot print.
#
# `k_vprintf` (rach2/sys/lib/kfmt.c) walks its arguments off the stack one word
# per conversion. A conversion it does not know is written as its letter alone
# and consumes no argument, so every field after it in the same message is read
# one slot early: the fault is wrong values, not only wrong letters. The machine
# on the desk printed `audio: not driven: 8086:a0c at 02x:02x.0` from a format
# of `%x:%x at %02x:%02x.%d`, before the `0` flag and the width were understood.
#
# What the formatter takes: `%d %b %o %x %X %p %s %c %%`, each with an
# optional `0` flag and one decimal digit of width before the letter, and `l`
# or `z` before the letter of a numeric conversion. Anything else, `%u`,
# `%lu`, `%llx` and the rest, is a finding here.
#
# Usage: fmtcheck.sh
set -u

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO" || exit 1

findings=0
for f in $(find rach2 -name '*.c' -o -name '*.h' | sort); do
    # Only the text inside the quotes of a message call is a format string; a
    # `%` anywhere else on the line is the remainder operator.
    awk -v file="$f" '
        /klog[ ]*\(|klog_v[ ]*\(|console_printf[ ]*\(|k_printf[ ]*\(/ { inCall = 1 }
        inCall {
            line = $0
            text = ""
            while (match(line, /"([^"\\]|\\.)*"/)) {
                lit = substr(line, RSTART + 1, RLENGTH - 2)
                text = text lit
                line = substr(line, RSTART + RLENGTH)
            }
            while (match(text, /%/)) {
                rest = substr(text, RSTART + 1)
                if (rest != "" && rest !~ /^0?[1-9]?[lz]?[dboxX]/ && rest !~ /^0?[1-9]?[psc%]/) {
                    printf "fmtcheck: %s:%d: %%%s is not a conversion this formatter has\n", \
                        file, NR, substr(rest, 1, 2)
                    bad++
                }
                if (rest ~ /^%/) {
                    text = substr(rest, 2)
                } else {
                    text = rest
                }
            }
            if (/\);/) { inCall = 0 }
        }
        END { exit (bad > 0) }
    ' "$f" || findings=$((findings + 1))
done

if [ "$findings" -gt 0 ]; then
    echo "fmtcheck: $findings file(s) with a conversion the formatter cannot print" >&2
    exit 1
fi
echo "fmtcheck: ok: every kernel message uses a conversion the formatter has"
