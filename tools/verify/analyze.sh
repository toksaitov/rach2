#!/usr/bin/env sh
#
# analyze.sh: the slow analysers, run by `make analyze` at a milestone's close
# and not by the style suite, which stays about a minute.
#
#   1. GCC's -fanalyzer over the kernel and the guest programs, under
#      -Werror, with -Wanalyzer-too-complex so a path the analyser gives up
#      on is reported and not silently green: those are counted and listed
#      by program point, not held against the build, since a dispatcher's
#      call graph exceeds the analyser's budget by design. The allocators
#      carry the malloc attributes and k_panic is noreturn (sys/lib/kdef.h),
#      which is what lets it follow a block from kmalloc to kfree or to a
#      leak.
#   2. cppcheck at --check-level=exhaustive over the same two lists the style
#      suite checks at the normal level, with the same suppressions.
#   3. clang-tidy's bugprone-* and misc-* checks and the cognitive
#      complexity report over the kernel, advisory: the count and the log,
#      never a verdict, since the checks overlap the two above and the
#      complexity numbers are read by a person.
#   4. The linker's dead-code report: the kernel linked with function and
#      data sections and --gc-sections, every section the linker dropped
#      printed. The linker honours assembly references, which cppcheck's
#      unusedFunction does not. Advisory: the count and the list.
#
# Each analyser reads the compilation databases the build phase generates,
# or builds the component again under its own COMPONENT name, so the
# ordinary objects are never touched. Usage: analyze.sh [--fast]; --fast
# skips the exhaustive cppcheck.
set -u
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO" || exit 1
VDIR=/tmp/rach2_verify
mkdir -p "$VDIR"
fail=0
fast=0
[ "${1:-}" = "--fast" ] && fast=1
hr() { echo "---- $1 ----"; }

hr "1/4 -fanalyzer over the kernel and the guest programs"
ANALYZE="-fanalyzer -Wanalyzer-too-complex -Wno-error=analyzer-too-complex"
if make -C rach2 -B -k COMPONENT=rach2-analyze KEXTRA="$ANALYZE" >"$VDIR/analyze_gcc.log" 2>&1 &&
   make -C tests -B -k COMPONENT=tests-analyze EXTRA="$ANALYZE -Werror" >>"$VDIR/analyze_gcc.log" 2>&1 &&
   make -C ish -B -k COMPONENT=ish-analyze EXTRA="$ANALYZE -Werror" >>"$VDIR/analyze_gcc.log" 2>&1 &&
   make -C utils -B -k COMPONENT=utils-analyze EXTRA="$ANALYZE -Werror" >>"$VDIR/analyze_gcc.log" 2>&1; then
    echo "  PASS (0 findings)"
else
    echo "  FAIL: $(grep 'warning:\|error:' "$VDIR/analyze_gcc.log" | grep -vc 'too-complex') finding(s); see $VDIR/analyze_gcc.log"
    grep "warning:\|error:" "$VDIR/analyze_gcc.log" | grep -v "too-complex" | head -5 | sed 's/^/    /'
    fail=1
fi
complex=$(grep -c "analyzer-too-complex" "$VDIR/analyze_gcc.log")
echo "  advisory: $complex program point(s) past the analyser's budget"
grep "analyzer-too-complex" "$VDIR/analyze_gcc.log" | grep -o "^[a-z/._0-9]*:[0-9]*" | sort | uniq -c | sort -rn | head -6 | sed 's/^/    /'

hr "2/4 cppcheck at the exhaustive level"
if [ "$fast" -eq 1 ]; then
    echo "  SKIP (--fast)"
else
    find "$REPO/rach2/sys" -name '*.c' >"$VDIR/_asrcs.txt"
    echo "$REPO/rach2/kernel.c" >>"$VDIR/_asrcs.txt"
    {
        find "$REPO/tools" -name '*.c' -o -name '*.h'
        find "$REPO/tests/system" "$REPO/krffs" -path '*/vendor' -prune -o -path '*/vs-dokany' -prune \
             -o \( -name '*.c' -o -name '*.h' \) -print
    } 2>/dev/null | sort >"$VDIR/_ahostsrcs.txt"
    if cppcheck --std=c89 --platform=unix32 --enable=warning,style,performance,portability \
          --check-level=exhaustive --error-exitcode=2 \
          --suppress=missingInclude --suppress=missingIncludeSystem \
          --suppressions-list="$REPO/tools/verify/cppcheck.suppress" \
          --file-list="$VDIR/_asrcs.txt" >"$VDIR/analyze_cppcheck.log" 2>&1 &&
       cppcheck --std=c99 --platform=unix64 --enable=warning,style,performance,portability \
          --check-level=exhaustive --error-exitcode=2 \
          --suppress=missingInclude --suppress=missingIncludeSystem \
          --suppressions-list="$REPO/tools/verify/cppcheck.suppress" \
          --file-list="$VDIR/_ahostsrcs.txt" >>"$VDIR/analyze_cppcheck.log" 2>&1; then
        echo "  PASS (0 findings)"
    else
        echo "  FAIL: see $VDIR/analyze_cppcheck.log"
        grep "\[[a-zA-Z]*\]$" "$VDIR/analyze_cppcheck.log" | head -5 | sed 's/^/    /'
        fail=1
    fi
fi

hr "3/4 clang-tidy over the kernel (advisory)"
if ! command -v clang-tidy >/dev/null 2>&1; then
    echo "  SKIP (clang-tidy is absent here; the testing host judges)"
elif ! sh "$REPO/tools/dev/gen_compile_commands.sh" >"$VDIR/analyze_db.log" 2>&1; then
    echo "  FAIL: the compilation databases were not generated; see $VDIR/analyze_db.log"; fail=1
else
    res=$(clang -print-resource-dir 2>/dev/null)
    : >"$VDIR/analyze_tidy.log"
    for f in $(make -s -C "$REPO/rach2" print-c-src); do
        clang-tidy -p "$REPO/build/compile_commands/rach2.json" \
            --checks='-*,bugprone-*,misc-*,readability-function-cognitive-complexity' \
            --extra-arg=--target=i686-elf --extra-arg=-nostdinc --extra-arg="-isystem$res/include" \
            "$REPO/rach2/$f" >>"$VDIR/analyze_tidy.log" 2>&1
    done
    echo "  advisory: $(grep -c 'warning:' "$VDIR/analyze_tidy.log") finding(s); see $VDIR/analyze_tidy.log"
    grep -o '\[[a-z-]*\.[a-zA-Z-]*\]$' "$VDIR/analyze_tidy.log" | sort | uniq -c | sort -rn | head -8 | sed 's/^/    /'
fi

hr "4/4 the linker's dead-code report (advisory)"
if make -C rach2 -B COMPONENT=rach2-gc KEXTRA="-ffunction-sections -fdata-sections" \
        LDEXTRA="--gc-sections --print-gc-sections" >"$VDIR/analyze_gc.log" 2>&1; then
    n=$(grep -c "removing unused section" "$VDIR/analyze_gc.log")
    echo "  advisory: $n section(s) the linker dropped; see $VDIR/analyze_gc.log"
    grep "removing unused section" "$VDIR/analyze_gc.log" | sed "s/.*'\(\.[a-z]*\.[^']*\)'.*/\1/" | head -12 | sed 's/^/    /'
else
    echo "  FAIL: the sectioned link failed; see $VDIR/analyze_gc.log"; fail=1
fi

hr "result"
if [ "$fail" -eq 0 ]; then echo "analyze: PASS"; else echo "analyze: FAIL"; fi
exit "$fail"
