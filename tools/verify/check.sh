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
#   2. cppcheck (c89, warning+style) with tools/verify/cppcheck.suppress, and
#      the host programs (tools/, tests/system/, krffs/) at c99
#   (--error-exitcode) 3. shellcheck on the in-house shell scripts
#   4. the convention grep (no // comments, ASCII only, _NAME_H_ guards)
#   5. clang-format: HARD on new-code dirs (tools/ libc/ tests/host, *probe.c)
#      and on any files named as arguments; advisory across the legacy tree
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
#
# Usage:
#     ./check.sh [file.c ...]      # extra files to hard clang-format check
#
# Exits non-zero if any hard check fails.
#

set -u

REPO="$(cd "$(dirname "$0")/../.." && pwd)"

# The byte scanner beside this script, compiled when missing or older than
# its source: the shell text tools disagree across platforms about NUL and
# high bytes, and C does not.
ASCIISCAN="$REPO/build/tools/verify/asciiscan"
if [ ! -x "$ASCIISCAN" ] || [ "$REPO/tools/verify/asciiscan.c" -nt "$ASCIISCAN" ]; then
    mkdir -p "${ASCIISCAN%/*}"
    cc -O2 -o "$ASCIISCAN" "$REPO/tools/verify/asciiscan.c" || exit 1
fi
# The probe runner is a test tool, and tests/system/suites.mk owns how the test
# tools are built. Asked for rather than compiled here, so there is one recipe
# for it and this check cannot disagree with the one the suites use.
RUNTESTS="$REPO/build/tests/system/runtests"
make -C "$REPO" --no-print-directory build/tests/system/runtests >/dev/null || exit 1
GUESTCHECK="$REPO/build/tools/verify/guestcheck"
if [ ! -x "$GUESTCHECK" ] || [ "$REPO/tools/verify/guestcheck.c" -nt "$GUESTCHECK" ]; then
    mkdir -p "${GUESTCHECK%/*}"
    cc -O2 -o "$GUESTCHECK" "$REPO/tools/verify/guestcheck.c" || exit 1
fi
PROSECHECK="$REPO/build/tools/verify/prosecheck"
if [ ! -x "$PROSECHECK" ] || [ "$REPO/tools/verify/prosecheck.c" -nt "$PROSECHECK" ]; then
    mkdir -p "${PROSECHECK%/*}"
    cc -O2 -o "$PROSECHECK" "$REPO/tools/verify/prosecheck.c" || exit 1
fi
cd "$REPO" || exit 1
VDIR=/tmp/rach2_verify
mkdir -p "$VDIR"
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
    # vendor/musl/ has only the vendored musl tree (install/, musl-*/) plus
    # any future in-house shim; prune the vendored parts so they are never
    # checked.
    find vendor/musl -path '*/install' -prune -o -path '*/musl-*' -prune -o \
         \( -name '*.c' -o -name '*.h' \) -print 2>/dev/null
    # The other hand-written programs: the krffs host tools (minus the vendored
    # termbox2 and vs-dokany), the utils, the ish shell, and every test (the
    # probes, the vm family, the musl integration tests, and the host
    # tests).
    find krffs utils ish tests -path '*/vendor' -prune -o -path '*/vs-dokany' -prune \
         -o \( -name '*.c' -o -name '*.h' \) -print 2>/dev/null
    # The host programs under tools/: the image builders, the inspectors,
    # the setup checks and the verifiers.
    find tools \( -name '*.c' -o -name '*.h' \) 2>/dev/null
}

# New code the port introduces; must be clang-format clean (unlike the legacy
# tree, which we never reformat).
#
# The globs do not reach tests/musl/ or utils/, and must not be widened to:
# those are legacy trees this check deliberately leaves unformatted, and a
# wholesale extension fails today on files nobody is asking to reformat. So a
# new source under either is listed here BY NAME, in the commit that creates it.
# A file added without being listed here is never format-checked, which is the
# failure this comment exists to prevent.
newcode_ch()
{
    find vendor/musl -path '*/install' -prune -o -path '*/musl-*' -prune -o \
         \( -name '*.c' -o -name '*.h' \) -print 2>/dev/null
    ls tests/host/*.c tests/host/*.h tests/*probe.c 2>/dev/null
    # By name, per the rule above: fdstale is a probe in everything but its
    # name, idreport is the helper the credential probes drive, and the
    # *probe.c glob reaches neither.
    ls tests/fdstale.c tests/idreport.c 2>/dev/null
    # By name, per the rule above: the credential library is new code under
    # utils/, and its two guest tests are under tests/musl/; no glob here
    # reaches either tree. The host test's compat headers are a directory down
    # from tests/host, which the glob above does not descend into.
    ls utils/pwauth.c utils/pwauth.h utils/passwd.c utils/adduser.c utils/login.c 2>/dev/null
    ls tests/musl/pwauthmusl.c tests/musl/saltnoent.c tests/musl/creddisk.c \
       tests/musl/passwdmusl.c tests/musl/addusermusl.c \
       tests/musl/loginmusl.c tests/musl/readvmusl.c tests/musl/vmamusl.c \
       tests/musl/clockmusl.c tests/musl/statmusl.c \
       tests/musl/stampmusl.c \
       tests/musl/execargmusl.c \
       tests/musl/spawnmusl.c 2>/dev/null
    ls tests/host/compat/crypt.h tests/host/compat/shadow.h \
       tests/host/compat/sys/random.h 2>/dev/null
    # By name, per the rule above: the extractor and its path walk are new
    # code in the otherwise legacy krffs tree, which no glob here reaches.
    ls krffs/krffs_reader.c krffs/krffs_reader.h krffs/readfile.krffs.c 2>/dev/null
}

# 0. required tools
hr "preflight: tools present"
miss=0
for t in i686-elf-gcc i686-elf-readelf cppcheck shellcheck clang-format; do
    command -v "$t" >/dev/null 2>&1 || { echo "  MISSING: $t"; miss=1; }
done
if [ "$miss" -eq 0 ]; then echo "  PASS"; else echo "  FAIL"; fail=1; fi

# 1. kernel build (-Werror)
hr "1/18 kernel build (-Werror)"
if make -C rach2 >"$VDIR/verify_build.log" 2>&1; then
    echo "  PASS (0 warnings/errors)"
else
    echo "  FAIL: see $VDIR/verify_build.log"; fail=1
fi

# 2. cppcheck: --error-exitcode makes a finding a real non-zero exit, so a
#    crash or a finding both fail (a silent 0-count cannot mask either).
hr "2/18 cppcheck (c89, warning+style)"
find "$REPO/rach2/sys" -name '*.c' >"$VDIR/_srcs.txt"
echo "$REPO/rach2/kernel.c" >>"$VDIR/_srcs.txt"
ls "$REPO"/tests/*probe.c >>"$VDIR/_srcs.txt" 2>/dev/null || true
ls "$REPO"/tests/fdstale.c "$REPO"/tests/idreport.c >>"$VDIR/_srcs.txt" 2>/dev/null || true
ls "$REPO"/utils/pwauth.c "$REPO"/utils/pwauth.h "$REPO"/utils/passwd.c \
   "$REPO"/utils/adduser.c "$REPO"/utils/login.c >>"$VDIR/_srcs.txt" 2>/dev/null || true
ls "$REPO"/tests/host/pwauth_hosttest.c "$REPO"/tests/musl/pwauthmusl.c \
   "$REPO"/tests/musl/passwdmusl.c \
   "$REPO"/tests/musl/addusermusl.c \
   "$REPO"/tests/musl/loginmusl.c \
   "$REPO"/tests/musl/readvmusl.c "$REPO"/tests/musl/vmamusl.c \
   "$REPO"/tests/musl/clockmusl.c "$REPO"/tests/musl/statmusl.c \
   "$REPO"/tests/musl/stampmusl.c "$REPO"/tests/musl/execargmusl.c "$REPO"/tests/musl/spawnmusl.c \
   "$REPO"/tests/musl/saltnoent.c "$REPO"/tests/musl/creddisk.c >>"$VDIR/_srcs.txt" 2>/dev/null || true
ls "$REPO"/krffs/krffs_reader.c "$REPO"/krffs/krffs_reader.h \
   "$REPO"/krffs/readfile.krffs.c >>"$VDIR/_srcs.txt" 2>/dev/null || true
if cppcheck --std=c89 --platform=unix32 --enable=warning,style --error-exitcode=2 \
      --suppress=missingInclude --suppress=missingIncludeSystem \
      --suppressions-list="$REPO/tools/verify/cppcheck.suppress" \
      --file-list="$VDIR/_srcs.txt" >"$VDIR/verify_cppcheck.log" 2>&1; then
    echo "  PASS (0 findings)"
else
    echo "  FAIL: see $VDIR/verify_cppcheck.log"; fail=1
fi
# The host programs, in their own dialect and on a 64-bit host: the tools,
# the runner, the checkers and the fixtures under tests/system, and the KRFFS
# tools without their vendored terminal library.
{
    find "$REPO/tools" -name '*.c' -o -name '*.h'
    find "$REPO/tests/system" "$REPO/krffs" -path '*/vendor' -prune -o -path '*/vs-dokany' -prune \
         -o \( -name '*.c' -o -name '*.h' \) -print
} 2>/dev/null | sort >"$VDIR/_hostsrcs.txt"
if cppcheck --std=c99 --platform=unix64 --enable=warning,style --error-exitcode=2 \
      --suppress=missingInclude --suppress=missingIncludeSystem \
      --suppressions-list="$REPO/tools/verify/cppcheck.suppress" \
      --file-list="$VDIR/_hostsrcs.txt" >"$VDIR/verify_cppcheck_host.log" 2>&1; then
    echo "  PASS (host programs: 0 findings)"
else
    echo "  FAIL: see $VDIR/verify_cppcheck_host.log"; fail=1
fi

# 3. shellcheck. New in-house scripts (tools/, vendor/*/build_*.sh) are STRICT;
#    SC2086 stays active so an accidental unquoted expansion is caught. The
#    legacy QEMU run/build scripts waive SC2086 (option word-splitting is
#    intentional) and SC2034 (vars shared across sourced fragments); SC1091
#    covers the dynamic source path in both.
hr "3/18 shellcheck"
sc=0
# shellcheck disable=SC2046
shellcheck -x -e SC1091 $(ls tests/run tests/system/suites/*.sh tests/system/accept/*.sh tests/system/grow_machine.sh tools/*/*.sh vendor/musl/*.sh vendor/*/build_*.sh 2>/dev/null) \
    >"$VDIR/verify_shellcheck.log" 2>&1 || sc=1
# shellcheck disable=SC2046
shellcheck -x -e SC1091 -e SC2034 -e SC2086 \
    $(ls rach2/*.sh krffs/*.sh 2>/dev/null) \
    >>"$VDIR/verify_shellcheck.log" 2>&1 || sc=1
if [ "$sc" -eq 0 ]; then echo "  PASS"; else echo "  FAIL: see $VDIR/verify_shellcheck.log"; fail=1; fi

# 4. convention grep (no //, ASCII only, _NAME_H_ guards)
hr "4/18 conventions (no //, ASCII, header guards, marked idle wait)"
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
# Every halt in the kernel goes through the marked idle wait in
# sys/proc/sched.c, which is what lets the timer tell an idle tick from a busy
# one: there is no idle task, so a blocked process stays current and nothing
# else says which was which. A site written with a bare `sti; hlt` somewhere
# else would report its idle time as busy and no test would notice, so the
# spelling is refused outside the helper. Comments and the header naming it are
# not code; the asm statement is what is looked for.
idle_bad=$(inhouse_ch | grep -vE 'sys/proc/sched\.c$' \
    | xargs grep -nE '__asm__[^;]*"[^"]*sti;[[:space:]]*hlt' 2>/dev/null || true)
if [ -n "$idle_bad" ]; then
    echo "  bare sti;hlt outside idle_wait_irq_off/on:"
    echo "$idle_bad" | head -3 | sed 's/^/    /'
    cv=1
fi
if [ "$cv" -eq 0 ]; then echo "  PASS"; else fail=1; fi

# 5. clang-format. HARD on new-code dirs + named args; advisory on the legacy
# tree. Uses --Werror so a real run failure is a real failure (not a 0 count).
hr "5/18 clang-format"
strict_fail=0
# shellcheck disable=SC2046
for f in $(newcode_ch) "$@"; do
    [ -f "$f" ] || continue
    if clang-format --style=file --dry-run --Werror "$f" >/dev/null 2>&1; then :; else
        echo "  FAIL (strict): $f"; strict_fail=1
    fi
done
[ "$strict_fail" -eq 0 ] && echo "  PASS (new code clean)" || fail=1
drift=0
# shellcheck disable=SC2046
for f in $(inhouse_ch); do
    clang-format --style=file --dry-run --Werror "$f" >/dev/null 2>&1 || drift=$((drift + 1))
done
echo "  advisory: $drift in-house files differ from .clang-format (legacy not reformatted)"

# 6. readelf ABI on a freshly linked musl binary (no stale-artifact dependency).
hr "6/18 readelf ABI (fresh musl link)"
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
hr "7/18 kernel source list (C_SRC vs disk) and the guest build lists"
if "$REPO/tools/verify/check_sources.sh" >"$VDIR/verify_sources.log" 2>&1 &&
   "$GUESTCHECK" "$REPO" >>"$VDIR/verify_sources.log" 2>&1; then
    echo "  PASS"
else
    echo "  FAIL: see $VDIR/verify_sources.log"; fail=1
fi
# No recipe under tests/ makes a product: the images, disks, ISOs and sticks
# the machine or a stick carries are the build's (the root Makefile and mk/),
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
hr "8/18 text style"
if "$REPO/tools/verify/textcheck.sh" >"$VDIR/verify_text.log" 2>&1; then
    echo "  PASS (textcheck)"
else
    tc=$(sed -n 's/^textcheck: \([0-9]*\) finding.*/\1/p' "$VDIR/verify_text.log")
    [ -n "$tc" ] || tc='?'
    echo "  FAIL: $tc text-style finding(s); see $VDIR/verify_text.log"; fail=1
fi

# Comment width. Written for milestone 26 and left unwired until the tree
# reached zero, because a check that fails on the day it lands teaches everyone
# to ignore it.
if "$PROSECHECK" columns >"$VDIR/verify_cols.log" 2>&1; then
    echo "  PASS (comments at 80 columns)"
else
    cc=$(sed -n 's/^prosecheck: \([0-9]*\) comment.*/\1/p' "$VDIR/verify_cols.log")
    [ -n "$cc" ] || cc='?'
    echo "  FAIL: $cc over-width comment line(s); see $VDIR/verify_cols.log"; fail=1
fi

# Vocabulary. Only the half of the register rule a script can decide: terms with
# no legitimate use in this tree, in comments AND in the strings a person reads.
# The other half, and the repetition report, are for review; see CLAUDE.md.
if "$PROSECHECK" --selftest >"$VDIR/verify_prose.log" 2>&1 &&
   "$PROSECHECK" banned >>"$VDIR/verify_prose.log" 2>&1; then
    echo "  PASS (banned vocabulary)"
else
    pc=$(sed -n 's/^prosecheck: \([0-9]*\) banned.*/\1/p' "$VDIR/verify_prose.log")
    [ -n "$pc" ] || pc='?'
    echo "  FAIL: $pc banned term use(s); see $VDIR/verify_prose.log"; fail=1
fi

# The two runner rules that decide a result without a boot: the scoring rule,
# which turns serial text into every probe's PASS or FAIL, and the harness's
# escape decoder, which turns a step's text into the bytes a guest is given. A
# regression in either is invisible in the place that would otherwise catch it,
# because both are what that place reads its answer from.
# Message formats. The kernel's own formatter takes a fixed set of
# conversions, and one it does not know is written out verbatim while
# consuming no argument, so every field after it in that message is read one
# slot early. That printed `at 02x:02x.0` on the machine and, worse, the wrong
# numbers beside it.
hr "9/18 message formats"
if "$REPO/tools/verify/fmtcheck.sh" >"$VDIR/verify_fmt.log" 2>&1; then
    echo "  PASS (fmtcheck)"
else
    fc=$(sed -n 's/^fmtcheck: \([0-9]*\) file.*/\1/p' "$VDIR/verify_fmt.log")
    [ -n "$fc" ] || fc='?'
    echo "  FAIL: $fc file(s) with a message the formatter cannot print; see $VDIR/verify_fmt.log"
    fail=1
fi

hr "10/18 runner rules (runtests and harness selftests)"
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
hr "11/18 set-id guard (selftest + manifest sweep)"
setid=0
# The self-test first: it covers the failure cases no current manifest
# exercises, so without it a regression in, say, the image walk or the path
# decoding would leave every real manifest passing and this check silent.
SETIDCHECK="$REPO/build/tools/inspect/setidcheck"
if [ ! -x "$SETIDCHECK" ] || [ "$REPO/tools/inspect/setidcheck.c" -nt "$SETIDCHECK" ]; then
    mkdir -p "${SETIDCHECK%/*}"
    cc -O2 -o "$SETIDCHECK" "$REPO/tools/inspect/setidcheck.c" || setid=1
fi
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
hr "12/18 image reproducibility (one tree, one image)"
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
#     refuse to publish an invalid database, and the suite checker must refuse
#     each form of a guessed duration and accept each marked interval.
hr "13/18 checker selftests"
ELFCHECK="$REPO/build/tools/inspect/elfcheck"
if [ ! -x "$ELFCHECK" ] || [ "$REPO/tools/inspect/elfcheck.c" -nt "$ELFCHECK" ]; then
    mkdir -p "${ELFCHECK%/*}"
    cc -O2 -o "$ELFCHECK" "$REPO/tools/inspect/elfcheck.c" || exit 1
fi
CFGCHECK="$REPO/build/tools/inspect/cfgcheck"
if [ ! -x "$CFGCHECK" ] || [ "$REPO/tools/inspect/cfgcheck.c" -nt "$CFGCHECK" ]; then
    mkdir -p "${CFGCHECK%/*}"
    cc -O2 -o "$CFGCHECK" "$REPO/tools/inspect/cfgcheck.c" || exit 1
fi
if "$ELFCHECK" --selftest >"$VDIR/verify_kernelck.log" 2>&1 &&
   "$CFGCHECK" --selftest >>"$VDIR/verify_kernelck.log" 2>&1 &&
   sh "$REPO/tools/dev/gen_compile_commands.sh" --selftest >>"$VDIR/verify_kernelck.log" 2>&1 &&
   sh "$REPO/tools/verify/suitecheck.sh" --selftest >>"$VDIR/verify_kernelck.log" 2>&1; then
    echo "  PASS"
else
    echo "  FAIL: see $VDIR/verify_kernelck.log"; fail=1
fi
# 13. the dev-log index against the directories it indexes. AGENTS.md tells a
#     reader that the last entry in dev-log/Readme.md is where the tree stands,
#     so every finished milestone must have an entry, the entries must be in the
#     directories' numeric order, and a directory holding only a plan must have
#     none. The selftest proves the rules reject; the run proves this index.
hr "14/18 dev-log index"
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
hr "15/18 test registry against tests/system/suites.mk"
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
hr "16/18 run- targets' products against prepare-all"
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
    # prepare-all reaches its two halves through its recipe, not its
    # prerequisites; only the prepare- targets that recipe names count.
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
hr "17/18 suites' waits and criteria (suitecheck)"
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
hr "18/18 the manual against the machine (mancheck)"
if make -s -C "$REPO" mancheck >"$VDIR/verify_man.log" 2>&1; then
    echo "  PASS"
else
    echo "  FAIL: a program, applet or call has no page, or a page is faulty; see $VDIR/verify_man.log:"
    grep "^mancheck:" "$VDIR/verify_man.log" | head -5 | sed 's/^/    /'
    fail=1
fi

hr "result"
if [ "$fail" -eq 0 ]; then echo "verify-style: PASS"; else echo "verify-style: FAIL"; fi
exit "$fail"
