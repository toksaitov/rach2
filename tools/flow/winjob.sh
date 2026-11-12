#!/bin/sh
# winjob.sh: print the lines for the testing host's MSYS2 bash that build
# winjob.c, beside this script, into Windows' output directory once and set
# winjob to the program.
#
# Usage: winjob.sh OUT
#
# The program is OUT/winjob-SUM.exe, SUM being cksum's checksum of the
# source, so that a changed source builds a new program and a program in use
# is never replaced. release.sh, stop.sh and sweep.sh put these lines before
# each script for Windows that runs or stops a release part; a script that
# finds no program after them cannot say whether a part runs.

[ "$#" -eq 1 ] || { echo 'usage: winjob.sh OUT' >&2; exit 2; }
src=$(dirname "$0")/winjob.c
sum=$(cksum < "$src" | cut -d ' ' -f 1) || exit 1
[ -n "$sum" ] || exit 1
echo "winjob=$1/winjob-$sum.exe"
# shellcheck disable=SC2016 # the remote shell expands these
echo 'if [ ! -x "$winjob" ]; then'
# shellcheck disable=SC2016 # the remote shell expands these
echo '    cat > "$winjob.$$.c" << '"'END_OF_WINJOB'"
cat "$src"
echo 'END_OF_WINJOB'
# A concurrent build may have put the program in place first.
cat << 'END'
    C:/msys64/ucrt64/bin/gcc.exe -O2 -static -o "$winjob.$$.exe" "$winjob.$$.c" > /dev/null 2>&1 &&
        { mv "$winjob.$$.exe" "$winjob" 2> /dev/null || [ -x "$winjob" ]; } ||
        echo "winjob: $winjob did not build" >&2
    rm -f "$winjob.$$.c" "$winjob.$$.exe"
fi
END
