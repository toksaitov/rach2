#!/usr/bin/env sh

#
# check.sh: Rach2 lightweight quality gate (`make verify-style`).
#
# Over IN-HOUSE code only (the kernel, the new probes/seam tests, the build
# scripts; never the vendored musl/ports trees), runs:
#
#   0. a tool-presence check (a missing tool is a hard FAILURE, never a silent skip)
#   1. the kernel build with -Werror (the zero-warning policy as a hard gate)
#   2. cppcheck (c89, warning+style) with tools/cppcheck.suppress (--error-exitcode)
#   3. shellcheck on the in-house shell scripts
#   4. the convention grep (no // comments, ASCII only, _NAME_H_ guards)
#   5. clang-format: HARD on new-code dirs (tools/ libc/ tests/host, *probe.c)
#      and on any files named as arguments; advisory across the legacy tree
#   6. a readelf ABI check on a freshly linked musl binary (entry + load range)
#   7. the kernel source-list guard (every rach2 .c on disk is in C_SRC)
#
# Usage:
#     ./check.sh [file.c ...]      # extra files to hard clang-format check
#
# Exits non-zero if any hard check fails.
#

set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 1
VDIR=/tmp/rach2_verify
mkdir -p "$VDIR"
LIBC="$REPO/libc/install"
fail=0

hr() { printf -- '---- %s ----\n' "$*"; }

# In-house C/H: the kernel, the new tools/libc C (if any), and the host/probe
# tests. Excludes the vendored musl tree, ports/doom, and the legacy utils/ish.
inhouse_ch()
{
    find rach2 \( -name '*.c' -o -name '*.h' \) 2>/dev/null
    # libc/ holds only the vendored musl tree (install/, musl-*/) plus any future
    # in-house shim; prune the vendored parts so they are never checked.
    find libc -path '*/install' -prune -o -path '*/musl-*' -prune -o \
         \( -name '*.c' -o -name '*.h' \) -print 2>/dev/null
    ls tests/host/*.c tests/host/*.h tests/*probe.c 2>/dev/null
}

# New code the port introduces; must be clang-format clean (unlike the legacy
# tree, which we never reformat).
newcode_ch()
{
    find libc -path '*/install' -prune -o -path '*/musl-*' -prune -o \
         \( -name '*.c' -o -name '*.h' \) -print 2>/dev/null
    ls tests/host/*.c tests/host/*.h tests/*probe.c 2>/dev/null
}

# 0. required tools
hr "0/7 tools present"
miss=0
for t in i686-elf-gcc i686-elf-readelf cppcheck shellcheck clang-format perl; do
    command -v "$t" >/dev/null 2>&1 || { echo "  MISSING: $t"; miss=1; }
done
if [ "$miss" -eq 0 ]; then echo "  PASS"; else echo "  FAIL"; fail=1; fi

# 1. kernel build (-Werror)
hr "1/7 kernel build (-Werror)"
if make -C rach2 >"$VDIR/verify_build.log" 2>&1; then
    echo "  PASS (0 warnings/errors)"
else
    echo "  FAIL: see $VDIR/verify_build.log"; fail=1
fi

# 2. cppcheck: --error-exitcode makes a finding a real non-zero exit, so a
#    crash or a finding both fail (a silent 0-count cannot mask either).
hr "2/7 cppcheck (c89, warning+style)"
find "$REPO/rach2/sys" -name '*.c' >"$VDIR/_srcs.txt"
echo "$REPO/rach2/kernel.c" >>"$VDIR/_srcs.txt"
ls "$REPO"/tests/*probe.c >>"$VDIR/_srcs.txt" 2>/dev/null || true
if cppcheck --std=c89 --platform=unix32 --enable=warning,style --error-exitcode=2 \
      --suppress=missingInclude --suppress=missingIncludeSystem \
      --suppressions-list="$REPO/tools/cppcheck.suppress" \
      --file-list="$VDIR/_srcs.txt" >"$VDIR/verify_cppcheck.log" 2>&1; then
    echo "  PASS (0 findings)"
else
    echo "  FAIL: see $VDIR/verify_cppcheck.log"; fail=1
fi

# 3. shellcheck. New in-house scripts (tools/, libc/) are STRICT; SC2086 stays
#    active so an accidental unquoted expansion is caught. The legacy QEMU
#    run/build scripts waive SC2086 (option word-splitting is intentional) and
#    SC2034 (vars shared across sourced fragments); SC1091 covers the dynamic
#    source path in both.
hr "3/7 shellcheck"
sc=0
# shellcheck disable=SC2046
shellcheck -x -e SC1091 $(ls tools/*.sh libc/*.sh 2>/dev/null) \
    >"$VDIR/verify_shellcheck.log" 2>&1 || sc=1
# shellcheck disable=SC2046
shellcheck -x -e SC1091 -e SC2034 -e SC2086 \
    $(ls rach2/*.sh krffs/build_*.sh krffs/create_*.sh 2>/dev/null) \
    >>"$VDIR/verify_shellcheck.log" 2>&1 || sc=1
if [ "$sc" -eq 0 ]; then echo "  PASS"; else echo "  FAIL: see $VDIR/verify_shellcheck.log"; fail=1; fi

# 4. convention grep (no //, ASCII only, _NAME_H_ guards)
hr "4/7 conventions (no //, ASCII, header guards)"
cv=0
if inhouse_ch | xargs grep -nE '(^|[^:"])//' 2>/dev/null | grep -vE 'https?://|://' | grep -q .; then
    echo "  // comment found:"; inhouse_ch | xargs grep -nE '(^|[^:"])//' 2>/dev/null | grep -vE 'https?://|://' | head -3 | sed 's/^/    /'; cv=1
fi
na=$(inhouse_ch | while read -r f; do perl -ne 'print "$ARGV\n" if /[^\x09\x0a\x0d\x20-\x7e]/' "$f"; done | sort -u)
[ -n "$na" ] && { echo "  non-ASCII bytes in:"; echo "$na" | sed 's/^/    /'; cv=1; }
# shellcheck disable=SC2046
for h in $(inhouse_ch | grep '\.h$'); do
    grep -qE '^[[:space:]]*#[[:space:]]*ifndef[[:space:]]+[A-Za-z0-9_]+' "$h" \
      || grep -qE '^[[:space:]]*#[[:space:]]*pragma[[:space:]]+once' "$h" \
      || { echo "  no header guard: $h"; cv=1; }
done
if [ "$cv" -eq 0 ]; then echo "  PASS"; else fail=1; fi

# 5. clang-format. HARD on new-code dirs + named args; advisory on the legacy
#    tree. Uses --Werror so a real run failure is a real failure (not a 0 count).
hr "5/7 clang-format"
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
hr "6/7 readelf ABI (fresh musl link)"
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
    echo "  FAIL: libc not built; run libc/build_libc.sh first (the ABI gate"
    echo "          cannot verify the musl ABI without it, so it does not pass)"
    fail=1
fi

# 7. kernel source-list guard: every rach2 .c on disk must be listed in C_SRC, so a
#    newly added kernel source cannot be silently dropped from the build.
hr "7/7 kernel source list (C_SRC vs disk)"
if "$REPO/tools/check_sources.sh" >"$VDIR/verify_sources.log" 2>&1; then
    echo "  PASS"
else
    echo "  FAIL: see $VDIR/verify_sources.log"; fail=1
fi

# Text style (advisory): the mechanical natural-text checks (tools/textcheck.sh). It
# does not gate the build yet; a later phase makes it hard once the tree is clean.
hr "text style (advisory)"
if "$REPO/tools/textcheck.sh" >"$VDIR/verify_text.log" 2>&1; then
    echo "  PASS"
else
    tc=$(sed -n 's/^textcheck: \([0-9]*\) finding.*/\1/p' "$VDIR/verify_text.log")
    [ -n "$tc" ] || tc='?'
    echo "  advisory: $tc text-style finding(s), not yet gating; see $VDIR/verify_text.log"
fi

hr "result"
if [ "$fail" -eq 0 ]; then echo "verify-style: PASS"; else echo "verify-style: FAIL"; fi
exit "$fail"
