#!/usr/bin/env sh

#
# check.sh: the Rach2 source checks (`tests/run style`).
#
# Over IN-HOUSE code only (the kernel, the krffs host tools, the utils, the ish
# shell, the tests, and the build scripts; never the vendored musl/ports trees),
# runs:
#
#   0. a tool-presence check (a missing tool is a hard FAILURE, never a silent
#   skip) 1. the kernel build with -Werror (the zero-warning policy, enforced)
#   2. cppcheck (c89; warning, style, performance, portability) with
#      tools/verify/cppcheck.suppress, and the host programs (tools/,
#      tests/system/, krffs/) at c99
#      (--error-exitcode; a differing cppcheck version reports its findings and
#      decides nothing, but a cppcheck that fails to run fails the step)
#   3. shellcheck on every in-house shell script (tools/verify/scripts.sh)
#   4. the convention grep (no // comments, ASCII only, _NAME_H_ guards)
#   5. clang-format on every in-house file and any named as arguments, judged
#      by the version tools/setup/judges records
#   6. a readelf ABI check on a freshly linked musl binary (entry + load range)
#   7. the kernel source-list guard (every rach2 .c on disk is in C_SRC), no
#      product rule under tests/, and every source in a compilation database
#   8. text style, in three parts: textcheck.sh for non-ASCII and dashes,
#      prosecheck for the 80-column comment rule and for the vocabulary a
#      script can decide (both beside this file).
#   9. the probe runner's scoring rule (runtests --selftest)
#  10. the set-id manifest sweep (tools/inspect/setidcheck.c)
#  11. image reproducibility (tools/verify/check_repro.sh): one tree, one image
#  12. the checker selftests (elfcheck, cfgcheck, and
#      the compile-commands generator)
#  13. the dev-log index (prosecheck devlog): it names every finished
#      milestone, and names the newest last
#  14. the test registry against the run- targets in tests/system/suites.mk
#  16. the suites' waits and criteria (tools/verify/suitecheck.sh): no sleep
#      for a number of seconds, no wait= step, no upper bound on a duration
#      in a probe, except an interval marked with a reason the file
#      tests/system/intervals lists
#  18. the manual against the machine (tools/verify/mancheck.c): a page or
#      an alias for every staged program, applet and registered call, every
#      page lint-clean, the recovery pages staged
#  19. clang's positive list over the kernel, judged by the recorded version
#  20. sparse over the kernel with the address-space annotations checked
#  22. the Markdown links (tools/verify/linkcheck.sh): every link names a file
#      that is there and every anchor a heading that is in it
#  21. the size limits (tools/verify/sizecheck.c) over every in-house C file:
#      100 lines, 6 parameters, nesting 4, and cyclomatic complexity 25 by
#      pmccabe, against tools/verify/sizes.reasons
#
# Usage:
#     ./check.sh [file.c ...]      # extra files to hard clang-format check
#
# Exits non-zero if any hard check fails.
#

set -u

REPO="$(cd "$(dirname "$0")/../.." && pwd)"

# Make builds the checkers this script runs, one recipe each under mk/host.mk's
# -Werror. A hand compile here would write a binary at the same path that make
# then reads as current, so the warning build would be skipped until another
# dependency changed.
ASCIISCAN="$REPO/build/tools/verify/asciiscan"
RUNTESTS="$REPO/build/tests/system/runtests"
GUESTCHECK="$REPO/build/tools/verify/guestcheck"
PROSECHECK="$REPO/build/tools/verify/prosecheck"
SIZECHECK="$REPO/build/tools/verify/sizecheck"
SETIDCHECK="$REPO/build/tools/inspect/setidcheck"
make -C "$REPO" --no-print-directory build/tools/verify/asciiscan \
    build/tests/system/runtests build/tools/verify/guestcheck \
    build/tools/verify/prosecheck build/tools/verify/sizecheck \
    build/tools/inspect/setidcheck >/dev/null || exit 1
cd "$REPO" || exit 1
VDIR=$(umask 077; mktemp -d "${TMPDIR:-/tmp}/rach2_verify.XXXXXX") || {
    echo "verify-style: cannot create a temporary directory" >&2
    exit 1
}
printf 'verify-style: logs: %s\n' "$VDIR"
LIBC="$REPO/vendor/musl/install"
fail=0

hr() { printf -- '---- %s ----\n' "$*"; }

# In-house C/H: the kernel, any in-house libc shim, the krffs host tools, the
# utils, the ish shell, and every test. Excludes the vendored trees (the musl
# source and install, the termbox2 under krffs/vendor, and krffs/vs-dokany);
# ports/doom is not a root here.
inhouse_ch()
{
    find rach2 \( -name '*.c' -o -name '*.h' \) 2>/dev/null
    find vendor/musl -path '*/install' -prune -o -path '*/musl-*' -prune -o \
         \( -name '*.c' -o -name '*.h' \) -print 2>/dev/null
    find krffs utils ish tests -path '*/vendor' -prune -o -path '*/vs-dokany' -prune \
         -o \( -name '*.c' -o -name '*.h' \) -print 2>/dev/null
    find tools \( -name '*.c' -o -name '*.h' \) 2>/dev/null
}


if ! sh tests/system/verify_temp_test.sh ||
   ! sh tools/verify/probecheck.sh ||
   ! sh tests/system/probecheck_test.sh; then
    fail=1
fi

hr "preflight: tools present"
miss=0
for t in i686-elf-gcc i686-elf-readelf cppcheck shellcheck clang-format; do
    command -v "$t" >/dev/null 2>&1 || { echo "  MISSING: $t"; miss=1; }
done
if [ "$miss" -eq 0 ]; then echo "  PASS"; else echo "  FAIL"; fail=1; fi

hr "1/22 kernel build (-Werror)"
if make -C rach2 >"$VDIR/verify_build.log" 2>&1; then
    echo "  PASS (0 warnings/errors)"
else
    echo "  FAIL: see $VDIR/verify_build.log"; fail=1
fi

# 2. cppcheck: --error-exitcode=2 makes a finding exit 2, so a silent 0-count
#    cannot mask one. Any other non-zero status is cppcheck itself failing to
#    run, which fails the step under every version; only the findings are the
#    recorded version's to judge.
hr "2/22 cppcheck (c89; warning, style, performance, portability)"
# cppcheck's findings change between majors in both directions: 2.19 reports
# a modulo of one in a mutation hook and an uninitialised word 2.21 does not,
# and 2.21 reports a shadowed argument and an unused cast 2.19 does not. So
# this is judged by the version tools/setup/judges records and nowhere else;
# another version reports what it found and decides nothing, as step 5 does
# for clang-format.
cpp_recorded=$(sed -n 's/^cppcheck: //p' "$REPO/tools/setup/judges")
cpp_found=$(cppcheck --version 2>/dev/null | head -1)
. "$REPO/tools/verify/srclists.sh"
srclists "$REPO" "$VDIR" _
cpp_rc=0
cppcheck --std=c89 --platform=unix32 --enable=warning,style,performance,portability --error-exitcode=2 \
      --suppress=missingInclude --suppress=missingIncludeSystem \
      --suppressions-list="$REPO/tools/verify/cppcheck.suppress" \
      --file-list="$VDIR/_srcs.txt" >"$VDIR/verify_cppcheck.log" 2>&1 || cpp_rc=$?
if [ "$cpp_rc" -eq 0 ]; then
    echo "  PASS (0 findings)"
elif [ "$cpp_rc" -ne 2 ]; then
    echo "  FAIL: cppcheck exited $cpp_rc, see $VDIR/verify_cppcheck.log"; fail=1
elif [ "$cpp_found" != "$cpp_recorded" ]; then
    echo "  SKIP ($(grep -c 'error:\|warning:\|style:\|performance:\|portability:' "$VDIR/verify_cppcheck.log") finding(s) under $cpp_found; the record says $cpp_recorded, and the testing host judges)"
else
    echo "  FAIL: see $VDIR/verify_cppcheck.log"; fail=1
fi
cpp_rc=0
cppcheck --std=c99 --platform=unix64 --enable=warning,style,performance,portability --error-exitcode=2 \
      --suppress=missingInclude --suppress=missingIncludeSystem \
      --suppressions-list="$REPO/tools/verify/cppcheck.suppress" \
      --file-list="$VDIR/_hostsrcs.txt" >"$VDIR/verify_cppcheck_host.log" 2>&1 || cpp_rc=$?
if [ "$cpp_rc" -eq 0 ]; then
    echo "  PASS (host programs: 0 findings)"
elif [ "$cpp_rc" -ne 2 ]; then
    echo "  FAIL: cppcheck exited $cpp_rc, see $VDIR/verify_cppcheck_host.log"; fail=1
elif [ "$cpp_found" != "$cpp_recorded" ]; then
    echo "  SKIP (host programs: $(grep -c 'error:\|warning:\|style:\|performance:\|portability:' "$VDIR/verify_cppcheck_host.log") finding(s) under $cpp_found; the testing host judges)"
else
    echo "  FAIL: see $VDIR/verify_cppcheck_host.log"; fail=1
fi

# 3. shellcheck over every in-house shell script, the list
#    tools/verify/scripts.sh prints, so a script added anywhere in the tree is
#    checked without being named here. SC1091 is off throughout, since a file
#    sourced through a variable cannot be followed. The QEMU run scripts under
#    rach2/ and the build scripts at krffs/*.sh also waive SC2086, since they
#    split option strings into words on purpose, and SC2034, since their
#    variables are read by the files that source them. An empty list fails
#    the step, since it means the list was not made.
hr "3/22 shellcheck"
sc=0
sh "$REPO/tools/verify/scripts.sh" >"$VDIR/_scripts.txt" || sc=1
legacy='^(rach2|krffs)/[^/]*\.sh$'
grep -vE "$legacy" "$VDIR/_scripts.txt" >"$VDIR/_scripts_strict.txt"
grep -E "$legacy" "$VDIR/_scripts.txt" >"$VDIR/_scripts_legacy.txt"
xargs shellcheck -x -e SC1091 <"$VDIR/_scripts_strict.txt" \
    >"$VDIR/verify_shellcheck.log" 2>&1 || sc=1
xargs shellcheck -x -e SC1091 -e SC2034 -e SC2086 <"$VDIR/_scripts_legacy.txt" \
    >>"$VDIR/verify_shellcheck.log" 2>&1 || sc=1
nscripts=$(grep -c . "$VDIR/_scripts.txt")
[ "$nscripts" -gt 0 ] || sc=1
if [ "$sc" -eq 0 ]; then
    echo "  PASS ($nscripts scripts)"
else
    echo "  FAIL: see $VDIR/verify_shellcheck.log"; fail=1
fi

# 4. convention grep (no //, ASCII only, _NAME_H_ guards)
hr "4/22 conventions (no //, ASCII, header guards, marked idle wait, group kills, exit flags)"
cv=0
if inhouse_ch | xargs grep -nE '(^|[^:"])//' 2>/dev/null | grep -vE 'https?://|://' | grep -q .; then
    echo "  // comment found:"; inhouse_ch | xargs grep -nE '(^|[^:"])//' 2>/dev/null | grep -vE 'https?://|://' | head -3 | sed 's/^/    /'; cv=1
fi
na=$(inhouse_ch | while read -r f; do "$ASCIISCAN" "$f" >/dev/null || echo "$f"; done | sort -u)
[ -n "$na" ] && { echo "  non-ASCII bytes in:"; echo "$na" | sed 's/^/    /'; cv=1; }
# shellcheck disable=SC2046
for h in $(inhouse_ch | grep '\.h$'); do
    grep -qE '^[[:space:]]*#[[:space:]]*ifndef[[:space:]]+[A-Za-z0-9_]+' "$h" \
      || grep -qE '^[[:space:]]*#[[:space:]]*pragma[[:space:]]+once' "$h" \
      || { echo "  no header guard: $h"; cv=1; }
done
# An idle wait, a halt with interrupts on that expects to resume, goes through
# the helper in sys/proc/sched.c, which is what lets the timer tell an idle
# tick from a busy one: there is no idle task, so a blocked process stays
# current and nothing else says which was which. A wait written as `sti; hlt`
# somewhere else would report its idle time as busy and no test would notice,
# so that spelling is refused outside the helper. A `cli; hlt` that stops the
# machine for good and a halt in early boot, before the scheduler accounts
# anything, are not idle waits and are left alone. Comments and the header
# naming the helper are not code; the asm statement is what is looked for.
idle_bad=$(inhouse_ch | grep -vE 'sys/proc/sched\.c$' \
    | xargs grep -nE '__asm__[^;]*"[^"]*sti;[[:space:]]*hlt' 2>/dev/null || true)
if [ -n "$idle_bad" ]; then
    echo "  bare sti;hlt outside idle_wait_irq_off/on:"
    echo "$idle_bad" | head -3 | sed 's/^/    /'
    cv=1
fi
# dash, the testing host's /bin/sh, refuses `--` in its kill builtin with
# "Illegal number: -", so `kill -TERM -- "-$child"` signals nothing, and a
# trap that stops a test's process group that way waits for the group to end
# by itself. After a signal option a negative pid is already a pid, so the
# form is `kill -TERM "-$child"`. The scripts read are the ones step 3 checks,
# every in-house shell script tools/verify/scripts.sh lists.
kill_bad=$(sh "$REPO/tools/verify/scripts.sh" \
    | xargs grep -nE 'kill[^|;&#]*[[:space:]]--[[:space:]]+"?-' 2>/dev/null \
    | grep -vE '^[^:]+:[0-9]+:[[:space:]]*#' || true)
if [ -n "$kill_bad" ]; then
    echo "  kill with an end-of-options marker before a process group, which dash refuses:"
    echo "$kill_bad" | head -3 | sed 's/^/    /'
    cv=1
fi
# bash 3.2, the Mac's /bin/sh and /bin/bash, gives an EXIT trap a status of
# 0 when `set -u` stops a script that also has `set -e`, and a trap that ends
# in success lets the script exit 0; dash, bash 5 and BusyBox's ash report
# the failure. So a script in the scope below that sets both and traps EXIT
# keeps a `finished` flag, set where it succeeds, and its trap fails a run
# that never set it (entry 222 of milestone 50's list). The scope is the
# scripts under tools/ and krffs/, most of which the Mac runs, and the machine
# probes and the acceptance's su command. The flags are read from a `set` at
# the start of a line, after any other options: `-eu`, `-Ceu`, `-e -u`, the
# two on separate lines, or `-o errexit` and `-o nounset`. A flag on the `#!`
# line or in a second `set` after a `;` is not read. The trap is read from a
# `trap` at the start of a line, and may span lines, its first leaving its
# quote open and its last ending in the quote and its signals, EXIT or 0 among
# them.
opts='^[[:space:]]*set([[:space:]]+([-+][A-Za-z]+|[-+]o[[:space:]]+[a-z]+))*[[:space:]]+'
sigs='([[:space:]]+[A-Z0-9]+)*[[:space:]]+(EXIT|0)([[:space:]]+[A-Z0-9]+)*[[:space:]]*$'
# shellcheck disable=SC2016
guard_bad=$(sh "$REPO/tools/verify/scripts.sh" \
    | grep -E '^(tools|krffs)/|^tests/system/(manual_probes|accept/su_command)\.sh$' \
    | while IFS= read -r f; do
        if grep -qE "${opts}(-[A-Za-z]*e[A-Za-z]*|-o[[:space:]]+errexit)([[:space:];]|\$)" "$f" &&
            grep -qE "${opts}(-[A-Za-z]*u[A-Za-z]*|-o[[:space:]]+nounset)([[:space:];]|\$)" "$f" &&
            { grep -qE "^[[:space:]]*trap .*[[:space:]](EXIT|0)([[:space:];]|\$)" "$f" ||
                { grep -qE "^[[:space:]]*trap[[:space:]]+(--[[:space:]]+)?('[^']*|\"[^\"]*)\$" "$f" &&
                    grep -qE "['\"]${sigs}" "$f"; }; } &&
            ! grep -qF '[ -n "$finished" ]' "$f"; then
            echo "$f"
        fi
    done)
if [ -n "$guard_bad" ]; then
    echo "  set -eu and an EXIT trap with no finished flag, which bash 3.2 lets exit 0:"
    echo "$guard_bad" | head -3 | sed 's/^/    /'
    cv=1
fi
if [ "$cv" -eq 0 ]; then echo "  PASS"; else fail=1; fi

# 5. clang-format over every in-house file, with --Werror so a real run
#    failure is a real failure (not a 0 count). The verdict depends on the
#    tool's version: two majors format four files of this tree differently.
#    So the check is judged by the version tools/setup/judges records, the
#    testing host's; another version reports the count and decides nothing.
hr "5/22 clang-format"
recorded=$(sed -n 's/^clang-format: //p' "$REPO/tools/setup/judges")
found=$(clang-format --version </dev/null 2>/dev/null | head -1)
drift=0
# shellcheck disable=SC2046
for f in $(inhouse_ch) "$@"; do
    [ -f "$f" ] || continue
    if clang-format --style=file --dry-run --Werror "$f" >/dev/null 2>&1; then :; else
        [ "$found" = "$recorded" ] && echo "  FAIL: $f"
        drift=$((drift + 1))
    fi
done
if [ "$found" != "$recorded" ]; then
    echo "  SKIP ($drift file(s) differ under $found; the record says $recorded, and the testing host judges)"
elif [ "$drift" -eq 0 ]; then
    echo "  PASS (every in-house file)"
else
    fail=1
fi

# 6. readelf ABI on a freshly linked musl binary (no stale-artifact dependency).
if ! sh tests/system/build_inputs_test.sh >"$VDIR/build-inputs.log" 2>&1; then
    echo "verify-style: link dependency test failed: $VDIR/build-inputs.log" >&2
    fail=1
fi

hr "6/22 readelf ABI (fresh musl link)"
if [ -f "$LIBC/lib/libc.a" ]; then
    printf 'int main(void){return 0;}\n' >"$VDIR/_abi.c"
    if i686-elf-gcc -static -nostdlib -nostartfiles -isystem "$LIBC/include" \
         -o "$VDIR/_abi" "$LIBC/lib/crt1.o" "$LIBC/lib/crti.o" "$VDIR/_abi.c" \
         "$LIBC/lib/libc.a" "$(i686-elf-gcc -print-libgcc-file-name)" \
         "$LIBC/lib/crtn.o" -Wl,-z,noexecstack 2>"$VDIR/_abi.log"; then
        h=$(i686-elf-readelf -h "$VDIR/_abi" 2>/dev/null)
        l=$(i686-elf-readelf -l "$VDIR/_abi" 2>/dev/null)
        entry=$(echo "$h" | sed -n 's/.*Entry point address: *//p')
        if echo "$h" | grep -q 'ELF32' \
           && echo "$h" | grep -qE 'Type: *EXEC' \
           && echo "$h" | grep -q 'Intel 80386' \
           && ! echo "$l" | grep -q 'INTERP' \
           && echo "$l" | grep -qE 'LOAD +0x[0-9a-f]+ +0x08048000' \
           && case "$entry" in 0x804[0-9a-f]*) true ;; *) false ;; esac; then
            echo "  PASS (ELF32 EXEC i386, static, LOAD@0x08048000, entry $entry)"
        else
            echo "  FAIL: off-ABI (entry=$entry); see $VDIR/_abi.log"; fail=1
        fi
    else
        echo "  FAIL: musl link failed; see $VDIR/_abi.log"; fail=1
    fi
else
    echo "  FAIL: libc not built; run vendor/musl/build_libc.sh first (this"
    echo "          check cannot verify the musl ABI without it)"
    fail=1
fi

# 7. kernel source-list guard: every rach2 .c on disk must be listed in C_SRC,
#    so a newly added kernel source cannot be silently dropped from the build.
hr "7/22 kernel source list (C_SRC vs disk) and the guest build lists"
if "$REPO/tools/verify/check_sources.sh" >"$VDIR/verify_sources.log" 2>&1 &&
   "$GUESTCHECK" "$REPO" >>"$VDIR/verify_sources.log" 2>&1; then
    echo "  PASS"
else
    echo "  FAIL: see $VDIR/verify_sources.log"; fail=1
fi
# No recipe under tests/ makes a product: the images, disks, ISOs and sticks a
# run boots are the build's (the root Makefile and mk/),
# and a suite names one only as a prerequisite. A product is a file under
# build/images named rach2-*, or one of the sealed and recovery names.
# The dollar is grep's, a literal $(NAME) target.
# shellcheck disable=SC2016
products=$(grep -n '^\(build/images/rach2-[a-z0-9-]*\.[a-z0-9]*\|\$(SEALED_DISK)\|\$(SEALED_EXT2)\|\$(RECOVERY_ISO)\|\$(RECOVERY_IMAGE)\|\$(METAL_IMAGE)\|\$(STICK_IMAGE)\|\$(INSTALL_STICK)\|\$(OS_REAL_IMAGE)\|\$(OS_SIMPLE_IMAGE)\|\$(TEST_IMAGE)\) *:' "$REPO/tests/system/suites.mk" || true)
if [ -z "$products" ]; then
    echo "  PASS (no product rule under tests/)"
else
    echo "  FAIL: a rule under tests/ makes a product:"
    echo "$products" | sed 's/^/    tests\/system\/suites.mk:/'
    fail=1
fi
# Every in-house source is compiled by something: the generator writes one
# compilation database per unit from the builds' dry runs, and dbcheck.sh
# finds a source no unit, includer or test fixture script names.
if sh "$REPO/tools/dev/gen_compile_commands.sh" >"$VDIR/verify_dbcheck.log" 2>&1 &&
   sh "$REPO/tools/verify/dbcheck.sh" >>"$VDIR/verify_dbcheck.log" 2>&1; then
    echo "  PASS ($(tail -1 "$VDIR/verify_dbcheck.log"))"
else
    echo "  FAIL: see $VDIR/verify_dbcheck.log"; fail=1
fi

# Text style: the mechanical natural-text checks (tools/verify/textcheck.sh). It
# catches non-ASCII bytes, faked em-dashes, and `-` list bullets in the in-house
# tree.
hr "8/22 text style"
if "$REPO/tools/verify/textcheck.sh" >"$VDIR/verify_text.log" 2>&1; then
    echo "  PASS (textcheck)"
else
    tc=$(sed -n 's/^textcheck: \([0-9]*\) finding.*/\1/p' "$VDIR/verify_text.log")
    [ -n "$tc" ] || tc='?'
    echo "  FAIL: $tc text-style finding(s); see $VDIR/verify_text.log"; fail=1
fi

# Comment width: every comment line in in-house code at 80 columns.
if "$PROSECHECK" columns >"$VDIR/verify_cols.log" 2>&1; then
    echo "  PASS (comments at 80 columns)"
else
    cc=$(sed -n 's/^prosecheck: \([0-9]*\) comment.*/\1/p' "$VDIR/verify_cols.log")
    [ -n "$cc" ] || cc='?'
    echo "  FAIL: $cc over-width comment line(s); see $VDIR/verify_cols.log"; fail=1
fi

# Vocabulary. Only the half of the register rule a script can decide: terms with
# no legitimate use in this tree, in comments AND in the strings a person reads.
# The other half, and the repetition report, are for review; see AGENTS.md.
if "$PROSECHECK" --selftest >"$VDIR/verify_prose.log" 2>&1 &&
   "$PROSECHECK" banned >>"$VDIR/verify_prose.log" 2>&1; then
    echo "  PASS (banned vocabulary)"
else
    pc=$(sed -n 's/^prosecheck: \([0-9]*\) banned.*/\1/p' "$VDIR/verify_prose.log")
    [ -n "$pc" ] || pc='?'
    echo "  FAIL: $pc banned term use(s); see $VDIR/verify_prose.log"; fail=1
fi

# Message formats. The kernel's own formatter takes a fixed set of
# conversions, and one it does not know is written as its letter alone while
# consuming no argument, so every field after it in that message is read one
# slot early. That printed `at 02x:02x.0` on the machine and, worse, the wrong
# numbers beside it.
hr "9/22 message formats"
if "$REPO/tools/verify/fmtcheck.sh" >"$VDIR/verify_fmt.log" 2>&1; then
    echo "  PASS (fmtcheck)"
else
    fc=$(sed -n 's/^fmtcheck: \([0-9]*\) file.*/\1/p' "$VDIR/verify_fmt.log")
    [ -n "$fc" ] || fc='?'
    echo "  FAIL: $fc file(s) with a message the formatter cannot print; see $VDIR/verify_fmt.log"
    fail=1
fi

# The two runner rules that decide a result without a boot: the scoring rule,
# which turns serial text into every probe's PASS or FAIL, and the harness's
# escape decoder, which turns a step's text into the bytes a guest is given. A
# regression in either is invisible in the place that would otherwise catch it,
# because both are what that place reads its answer from.
hr "10/22 runner rules (runtests and harness selftests)"
HARNESSBIN="$REPO/build/tests/system/harness"
make -C "$REPO" --no-print-directory build/tests/system/harness >/dev/null || exit 1
if "$RUNTESTS" --selftest >"$VDIR/verify_score.log" 2>&1 &&
   "$HARNESSBIN" --selftest >>"$VDIR/verify_score.log" 2>&1; then
    echo "  PASS"
else
    echo "  FAIL: see $VDIR/verify_score.log"; fail=1
fi

# Every set-id line in every image manifest, against the per-image allowlist.
# This half reads sources only, so it belongs with the source checks; the image
# inventory that reads a built artifact runs from mkimage.sh and mkdisk.sh
# instead.
hr "11/22 set-id guard (selftest + manifest sweep)"
setid=0
# The self-test first: it covers the failure cases no current manifest
# exercises, so without it a regression in, say, the image walk or the path
# decoding would leave every real manifest passing and this check silent.
"$SETIDCHECK" --selftest >"$VDIR/verify_setid.log" 2>&1 || setid=1
"$SETIDCHECK" >>"$VDIR/verify_setid.log" 2>&1 || setid=1
if [ "$setid" -eq 0 ]; then
    echo "  PASS"
else
    sed 's/^/  /' "$VDIR/verify_setid.log"
    fail=1
fi


# 11. image reproducibility: one tree, one image. It stages a scratch manifest
#     twice with the real host tools, so those are BUILT here first, the way
#     step 1 builds the kernel it checks: a check that tested whatever binary
#     happened to be lying in build/ would report on another day's source.
hr "12/22 image reproducibility (one tree, one image)"
if ! make -C "$REPO/krffs" >"$VDIR/verify_repro_build.log" 2>&1; then
    echo "  FAIL: the krffs host tools did not build; see $VDIR/verify_repro_build.log"
    fail=1
elif sh "$REPO/tools/verify/check_repro.sh" >"$VDIR/verify_repro.log" 2>&1; then
    echo "  PASS"
else
    echo "  FAIL: see $VDIR/verify_repro.log"; fail=1
    sed -n '1,10p' "$VDIR/verify_repro.log" | sed 's/^/  /'
fi


# 12. the checkers' own selftests. Each runs on every build of the thing it
#     guards, so what is verified here is the checker itself: the kernel one
#     must accept a well-formed kernel and refuse each malformed variant a
#     loader would misload, the GRUB one must accept the configuration a core
#     image has and refuse every way of losing the recovery path, the
#     sealed-network one must refuse every way the menu and the recovery script
#     can come to describe two different machines, the compilation-database
#     generator must join wrapped recipes, select only compile commands, and
#     refuse to publish an invalid database, the suite checker must refuse
#     each form of a guessed duration and accept each marked interval, the
#     build rules that tests/system/build_rules_test.sh checks must hold in a
#     private tree: the source-list guard names a source missing from its list
#     and one missing from the disk, a clean needs no cross compiler, the GRUB
#     disk has a rule, the disk and stick rules have their tools and names, a
#     keyed tree names its disks for the key, and a release is refused in one,
#     and the script
#     list of step 3 and the text check of step 8 must read each kind of
#     file they claim and find a fault planted in each
#     (tests/system/style_scope_test.sh).
hr "13/22 checker selftests"
ELFCHECK="$REPO/build/tools/inspect/elfcheck"
CFGCHECK="$REPO/build/tools/inspect/cfgcheck"
make -C "$REPO" --no-print-directory build/tools/inspect/elfcheck build/tools/inspect/cfgcheck \
    >/dev/null || exit 1
if "$ELFCHECK" --selftest >"$VDIR/verify_kernelck.log" 2>&1 &&
   "$CFGCHECK" --selftest >>"$VDIR/verify_kernelck.log" 2>&1 &&
   sh "$REPO/tools/dev/gen_compile_commands.sh" --selftest >>"$VDIR/verify_kernelck.log" 2>&1 &&
   sh "$REPO/tools/verify/suitecheck.sh" --selftest >>"$VDIR/verify_kernelck.log" 2>&1 &&
   sh "$REPO/tests/system/recovery_key_test.sh" >>"$VDIR/verify_kernelck.log" 2>&1 &&
   sh "$REPO/tests/system/dryrun_test.sh" >>"$VDIR/verify_kernelck.log" 2>&1 &&
   sh "$REPO/tests/system/build_rules_test.sh" >>"$VDIR/verify_kernelck.log" 2>&1 &&
   sh "$REPO/tests/system/style_scope_test.sh" >>"$VDIR/verify_kernelck.log" 2>&1; then
    echo "  PASS"
else
    echo "  FAIL: see $VDIR/verify_kernelck.log"; fail=1
fi
# 13. the dev-log index against the directories it indexes. AGENTS.md tells a
#     reader that the last entry in dev-log/Readme.md is where the tree stands,
#     so every finished milestone must have an entry, the entries must be in the
#     directories' numeric order, and a directory holding only a plan must have
#     none. The selftest proves the rules reject; the run proves this index.
hr "14/22 dev-log index"
if "$PROSECHECK" devlog >"$VDIR/verify_devlog.log" 2>&1; then
    echo "  PASS"
else
    echo "  FAIL: see $VDIR/verify_devlog.log"; fail=1
fi
# 14. the test registry against the suites it names. tests/run reads the
#     registry and calls make with `run-` and the name, so a row whose target
#     does not exist is a suite that cannot run, and a target with no row is a
#     suite nothing runs. Neither shows up anywhere else: make accepts an
#     unknown goal only by failing, which happens on the day someone asks for
#     that suite, and a target no one names is silent forever.
hr "15/22 test registry against tests/system/suites.mk"
{
    awk -F'|' '!/^#/ && NF > 1 { print $1 }' "$REPO/tests/registry" | sort > "$VDIR/_reg.txt"
    grep -o '^run-[a-z0-9-]*' "$REPO/tests/system/suites.mk" | sed 's/^run-//' \
        | sort -u > "$VDIR/_tgt.txt"
} 2>"$VDIR/verify_registry.log"
regfail=0
if [ "$(wc -l < "$VDIR/_reg.txt")" != "$(sort -u "$VDIR/_reg.txt" | wc -l)" ]; then
    echo "  FAIL: a suite is named twice in tests/registry"
    sort "$VDIR/_reg.txt" | uniq -d | sed 's/^/    /'
    regfail=1
fi
missing=$(comm -23 "$VDIR/_reg.txt" "$VDIR/_tgt.txt")
extra=$(comm -13 "$VDIR/_reg.txt" "$VDIR/_tgt.txt")
if [ -n "$missing" ]; then
    echo "  FAIL: in tests/registry with no run- target:"
    echo "$missing" | sed 's/^/    /'
    regfail=1
fi
if [ -n "$extra" ]; then
    echo "  FAIL: a run- target no row in tests/registry names:"
    echo "$extra" | sed 's/^/    /'
    regfail=1
fi
# The generated inventory: tests/system/suites.md is `tests/run inventory`,
# and a sentence naming every suite exists in suites.mk for it to read.
sh "$REPO/tests/run" inventory > "$VDIR/_inventory.md" 2>"$VDIR/verify_inventory.log"
if ! cmp -s "$VDIR/_inventory.md" "$REPO/tests/system/suites.md"; then
    echo "  FAIL: tests/system/suites.md is not \`tests/run inventory\`; regenerate it"
    diff "$REPO/tests/system/suites.md" "$VDIR/_inventory.md" | head -5 | sed 's/^/    /'
    regfail=1
fi
if grep -q 'no sentence in suites.mk' "$VDIR/_inventory.md"; then
    echo "  FAIL: a suite has no \`tests/run NAME\` sentence in suites.mk:"
    grep 'no sentence' "$VDIR/_inventory.md" | sed 's/:.*//; s/^/    /'
    regfail=1
fi
if [ "$regfail" -eq 0 ]; then
    echo "  PASS ($(wc -l < "$VDIR/_reg.txt" | tr -d ' ') suites, each with its target)"
else
    fail=1
fi

# 15. every product a `run-` target names is built by `prepare-all`, so that a
#     set's pool never has two suites building one at once. Both sides come
#     from make's own database, prerequisites expanded, and the closure is
#     transitive, so a product reached through an image counts as prepared.
#     `prepare-all` builds its halves from its recipe, so the recipe is read
#     for the targets it names; a half dropped from it is found.
hr "16/22 run- targets' products against prepare-all"
make -C "$REPO" -pn prepare-all > "$VDIR/_db.txt" 2>/dev/null || { echo "  FAIL: make -pn prepare-all failed"; fail=1; }
unprepared=$(awk '
    /^#/ || /^$/ { next }
    /^[^ \t:=]+:[^=]/ {
        t = $1; sub(/:$/, "", t)
        n = split($0, f, /[ \t]+/)
        for (i = 2; i <= n; i++) if (f[i] != "" && f[i] != "|") deps[t] = deps[t] " " f[i]
        if (t ~ /^run-/) runs[t] = 1
        cur = t; next
    }
    /^\t/ && cur == "prepare-all" {
        n = split($0, f, /[ \t]+/)
        for (i = 1; i <= n; i++) if (f[i] ~ /^prepare-/) deps[cur] = deps[cur] " " f[i]
        next
    }
    /^\t/ { next }
    END {
        queue[1] = "prepare-all"; head = 1; tail = 1; seen["prepare-all"] = 1
        while (head <= tail) {
            t = queue[head++]
            m = split(deps[t], d, " ")
            for (i = 1; i <= m; i++) if (d[i] != "" && !(d[i] in seen)) { seen[d[i]] = 1; queue[++tail] = d[i] }
        }
        for (r in runs) {
            m = split(deps[r], d, " ")
            for (i = 1; i <= m; i++) if (d[i] ~ /^build\// && !(d[i] in seen)) print r ": " d[i]
        }
    }' "$VDIR/_db.txt" | sort)
if [ -z "$unprepared" ]; then
    echo "  PASS"
else
    echo "  FAIL: a run- target names a product prepare-all does not build:"
    echo "$unprepared" | sed 's/^/    /'
    fail=1
fi

# 17. the suites wait for tokens and assert on counts. A `sleep N` in a
#     suite, a `wait=N` harness step and an upper bound on a duration in a
#     probe are guesses at a machine's speed; each is refused unless it is
#     an interval marked with a reason that tests/system/intervals lists.
hr "17/22 suites' waits and criteria (suitecheck)"
if sh "$REPO/tools/verify/suitecheck.sh" >"$VDIR/verify_suites.log" 2>&1; then
    echo "  PASS"
else
    echo "  FAIL: a suite guesses a duration; see $VDIR/verify_suites.log:"
    head -5 "$VDIR/verify_suites.log" | sed 's/^/    /'
    fail=1
fi

# 18. the manual covers the machine (tools/verify/mancheck.c): every program
#     the manifests stage, every BusyBox applet and every registered system
#     call has a page or an alias, every alias names a page, every page passes
#     mandoc's lint, and the recovery image stages the pages man/recovery lists.
hr "18/22 the manual against the machine (mancheck)"
if make -s -C "$REPO" mancheck >"$VDIR/verify_man.log" 2>&1; then
    echo "  PASS"
else
    echo "  FAIL: a program, applet or call has no page, or a page is faulty; see $VDIR/verify_man.log:"
    grep "^mancheck:" "$VDIR/verify_man.log" | head -5 | sed 's/^/    /'
    fail=1
fi

# 19. clang as a second front end over the kernel, with a short positive
#     list: the flags -Weverything showed to have signal here. Judged by the
#     clang version tools/setup/judges records; another version reports the
#     count and decides nothing, as the format check does.
hr "19/22 clang's positive list over the kernel"
recorded=$(sed -n 's/^clang: //p' "$REPO/tools/setup/judges")
found=$(clang --version </dev/null 2>/dev/null | head -1)
CLANG_WARN="-Wcast-align -Wconditional-uninitialized -Wshadow-all -Wtypedef-redefinition -Wcomma"
: >"$VDIR/verify_clang.log"
res=$(clang -print-resource-dir 2>/dev/null)
clang_bad=0
for f in $(make -s -C "$REPO/rach2" print-c-src); do
    # shellcheck disable=SC2086
    (cd "$REPO/rach2" && clang --target=i686-elf -m32 -ansi -ffreestanding -nostdinc -isystem "$res/include" \
        -fsyntax-only $CLANG_WARN "$f") >>"$VDIR/verify_clang.log" 2>&1 || clang_bad=$((clang_bad + 1))
done
clang_warnings=$(grep -c "warning:" "$VDIR/verify_clang.log")
if [ "$found" != "$recorded" ]; then
    echo "  SKIP ($clang_warnings warning(s) under $found; the record says $recorded, and the testing host judges)"
elif [ "$clang_bad" -eq 0 ] && [ "$clang_warnings" -eq 0 ]; then
    echo "  PASS (0 warnings over $(make -s -C "$REPO/rach2" print-c-src | wc -w | tr -d ' ') files)"
else
    echo "  FAIL: $clang_warnings warning(s); see $VDIR/verify_clang.log"; fail=1
fi

# 20. sparse with __CHECKER__ defined, so the kernel's __user annotations are
#     checked: a user pointer dereferenced directly, or passed
#     where a kernel pointer is expected, is a finding. -Wno-non-pointer-null
#     is the one category left out; tools/verify/exceptions.md says why.
hr "20/22 sparse over the kernel's address spaces"
if ! command -v sparse >/dev/null 2>&1; then
    echo "  SKIP (sparse is absent here; the testing host judges)"
else
    gccinc=$(i686-elf-gcc -print-file-name=include)
    : >"$VDIR/verify_sparse.log"
    sparse_bad=0
    for f in $(make -s -C "$REPO/rach2" print-c-src); do
        (cd "$REPO/rach2" && sparse -D__CHECKER__ -m32 -ansi -ffreestanding -nostdinc -I"$gccinc" \
            -Wno-decl -Wno-non-pointer-null "$f") >>"$VDIR/verify_sparse.log" 2>&1 ||
            sparse_bad=$((sparse_bad + 1))
    done
    sparse_n=$(grep -c "warning:\|error:" "$VDIR/verify_sparse.log")
    if [ "$sparse_bad" -ne 0 ]; then
        echo "  FAIL: sparse did not run over $sparse_bad file(s); see $VDIR/verify_sparse.log"; fail=1
    elif [ "$sparse_n" -eq 0 ]; then
        echo "  PASS (0 findings)"
    else
        echo "  FAIL: $sparse_n finding(s); see $VDIR/verify_sparse.log"; fail=1
    fi
fi

# 21. The size limits over every in-house C file. pmccabe's table gives the
#     complexity; without pmccabe the three measures the checker takes itself
#     are judged and the complexity waits for the testing host. The reasons
#     file records every excess allowed and the value it may not grow past.
hr "21/22 function sizes"
inhouse_c=$(inhouse_ch | grep '\.c$' | sort)
if command -v pmccabe >/dev/null 2>&1; then
    # shellcheck disable=SC2086
    pmccabe $inhouse_c >"$VDIR/verify_pmccabe.txt" 2>/dev/null
    size_table="--pmccabe $VDIR/verify_pmccabe.txt"
    size_note=""
else
    size_table=""
    size_note="; complexity not judged, pmccabe is absent here and the testing host judges"
fi
# shellcheck disable=SC2086
if "$SIZECHECK" $size_table "$REPO/tools/verify/sizes.reasons" $inhouse_c >"$VDIR/verify_sizes.log" 2>&1; then
    echo "  PASS ($(sed -n 's/^sizecheck: ok: //p' "$VDIR/verify_sizes.log")$size_note)"
else
    sn=$(sed -n 's/^sizecheck: \([0-9]*\) finding.*/\1/p' "$VDIR/verify_sizes.log")
    [ -n "$sn" ] || sn='?'
    echo "  FAIL: $sn size finding(s); see $VDIR/verify_sizes.log"; fail=1
fi

# 22. The links between the documents. A Readme that points at a file a commit
#     moved is worse than one that says nothing: a reader follows it and
#     learns that the tree cannot be trusted.
hr "22/22 the documents' links"
# The milestone checklists under dev-log/ are lists of links by their nature,
# so they are checked by name although the records around them are not.
checklists=$(cd "$REPO" && find dev-log -name checklist.md | sort | tr '\n' ' ')
# shellcheck disable=SC2086 # one path a word, listed above
if sh "$REPO/tools/verify/linkcheck.sh" >"$VDIR/verify_links.log" 2>&1 &&
   sh "$REPO/tools/verify/linkcheck.sh" $checklists >>"$VDIR/verify_links.log" 2>&1; then
    echo "  PASS ($(sed -n 's/^linkcheck: ok: //p' "$VDIR/verify_links.log" | tr '\n' ';' | sed 's/;$//; s/;/, and /'))"
else
    echo "  FAIL: $(sed -n 's/^linkcheck: \([0-9]*\) broken.*/\1/p' "$VDIR/verify_links.log") broken link(s); see $VDIR/verify_links.log"
    grep -m3 'no such file\|has no heading' "$VDIR/verify_links.log" | sed 's/^/    /'
    fail=1
fi

hr "result"
if [ "$fail" -eq 0 ]; then echo "verify-style: PASS"; else echo "verify-style: FAIL"; fi
exit "$fail"
