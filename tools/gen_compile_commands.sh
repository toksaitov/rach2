#!/usr/bin/env sh
#
# gen_compile_commands.sh: generate compile_commands.json, the LSP compilation
# database clangd reads for per-file flags (run via `make compile-commands`).
#
# GCC has no clang-style -MJ, so the database is generated FROM the build's own
# commands: each in-house component is dry-run (`make -Bn`), and every i686-elf cross
# GCC invocation that compiles a C source becomes one entry (directory + file + the
# verbatim command). This covers the FREESTANDING / musl in-house code: the kernel
# (rach2) and the guest programs (tests, ish, utils). The host krffs FUSE tools (built
# with the host `cc` and dynamic `pkg-config` flags), and the vendored musl/busybox/
# dropbear and Doom trees, are intentionally excluded: clangd-on-the-kernel is the goal
# and those carry flags that do not belong in a portable database.
#
# The output (compile_commands.json at the repo root) is git-ignored and regenerable;
# rerun this after adding/removing a source or changing flags.
#
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$REPO/compile_commands.json"

entries=$(mktemp)
dump=$(mktemp)
out_tmp=$(mktemp)
rawlist=$(mktemp)
expected=$(mktemp)
missing=$(mktemp)
trap 'rm -f "$entries" "$dump" "$out_tmp" "$rawlist" "$expected" "$missing"' EXIT INT TERM

# Escape a string for a JSON double-quoted value: backslashes first, then quotes.
json_escape() {
    printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

# In-house, cross-compiled components (host krffs tools + vendored/Doom trees excluded).
for comp in rach2 tests ish utils; do
    dir="$REPO/$comp"
    [ -d "$dir" ] || continue
    # Dry-run an always-make build. Capture it to a file (a pipe would lose the exit
    # status), so a toolchain/Make failure is a hard error, not a silent empty DB.
    if ! ( cd "$dir" && make -Bn ) >"$dump" 2>/dev/null; then
        echo "error: 'make -Bn' failed in $comp/ -- cannot generate the database" >&2
        echo "       (is the i686-elf cross toolchain installed? try 'make doctor')" >&2
        exit 1
    fi
    dirj=$(json_escape "$dir")
    while IFS= read -r line; do
        # Only the i686-elf cross GCC (skips the host cc, i686-elf-ld links, asm, ...).
        case "$line" in
            *i686-elf-*gcc*) ;;
            *) continue ;;
        esac
        cmdj=$(json_escape "$line")
        # One entry per C source operand. The kernel compiles name exactly one source;
        # the tests/utils compile+link recipes (ping, nc, dnstest) name two, and the
        # command is stored verbatim; clangd keys on "file" and resolves just that
        # TU's flags (verified with clangd --check), so the extra inputs are harmless.
        printf '%s\n' "$line" | tr ' ' '\n' | grep -E '\.c$' | while IFS= read -r src; do
            filej=$(json_escape "$src")
            printf '{"directory": "%s", "file": "%s", "command": "%s"}\n' \
                "$dirj" "$filej" "$cmdj" >>"$entries"
        done
    done <"$dump"
done

# Build the JSON array into a TEMP file (comma after all but the last; [] if empty). It
# is promoted to compile_commands.json only after coverage passes, so a failure never
# leaves a bad/partial DB in place.
{
    echo '['
    awk 'NR>1 { print prev "," } { prev = $0 } END { if (NR > 0) print prev }' "$entries"
    echo ']'
} >"$out_tmp"

# Coverage assertion: every kernel source listed in C_SRC must have made it into the
# database. Capture the list to a file FIRST (a pipe would lose print-c-src's exit
# status and an empty list would vacuously "pass"), then require it to have succeeded
# and be non-empty before checking each source is present.
if ! make --no-print-directory -C "$REPO/rach2" print-c-src >"$rawlist" 2>/dev/null; then
    echo "error: 'make -C rach2 print-c-src' failed -- cannot verify kernel coverage" >&2
    exit 1
fi
tr ' ' '\n' <"$rawlist" | sed '/^[[:space:]]*$/d' | sort -u >"$expected"
if [ ! -s "$expected" ]; then
    echo "error: kernel source list (C_SRC) is empty -- is rach2's print-c-src intact?" >&2
    exit 1
fi
while IFS= read -r src; do
    grep -qF "\"file\": \"$src\"" "$out_tmp" || echo "$src" >>"$missing"
done <"$expected"
if [ -s "$missing" ]; then
    echo "error: kernel sources missing from compile_commands.json (generator bug):" >&2
    sed 's/^/  /' "$missing" >&2
    exit 1
fi

# Coverage passed; promote the temp DB into place (guarded: the script does not run
# under set -e, so an unchecked mv could fall through and falsely report success).
if ! mv "$out_tmp" "$OUT"; then
    echo "error: failed to write $OUT" >&2
    exit 1
fi

n=$(grep -c '"file"' "$OUT" 2>/dev/null || echo 0)
echo "compile_commands.json: $n entries -> $OUT"
