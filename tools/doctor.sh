#!/usr/bin/env sh
#
# tools/doctor.sh: Rach2 build/run toolchain preflight (run via `make doctor`).
#
# Reports each tool as PASS / MISSING / OPTIONAL with an install hint, and exits
# non-zero if a REQUIRED tool is absent (or present but broken), so a student or a
# fresh CI box sees a clear checklist instead of a cryptic failure mid-build.
#
# ASCII-only output: the convention gate (tools/check.sh) forbids non-ASCII bytes in
# in-house files, so no checkmarks/Unicode here.
#
set -u

case "$(uname -s)" in
    Darwin) MAC=1 ;;
    *)      MAC=0 ;;
esac

# hint KEY: an OS-appropriate install suggestion. The i686-elf cross toolchain is
# NOT a stock apt package on Linux, so its hint is spelled out rather than assuming
# a package manager.
hint() {
    case "$1" in
    cross)    [ "$MAC" = 1 ] && echo "brew install i686-elf-gcc" \
                             || echo "build the OSDev i686-elf cross GCC, or 'brew install i686-elf-gcc' via Homebrew on Linux" ;;
    binutils) [ "$MAC" = 1 ] && echo "brew install i686-elf-binutils" \
                             || echo "part of the i686-elf cross toolchain (OSDev / Homebrew on Linux)" ;;
    qemu)     [ "$MAC" = 1 ] && echo "brew install qemu" || echo "apt install qemu-system-x86" ;;
    pkgconf)  [ "$MAC" = 1 ] && echo "brew install pkg-config" || echo "apt install pkg-config" ;;
    make)     [ "$MAC" = 1 ] && echo "xcode-select --install" || echo "apt install make" ;;
    cc)       [ "$MAC" = 1 ] && echo "xcode-select --install" || echo "apt install build-essential" ;;
    fuse)     [ "$MAC" = 1 ] && echo "brew install --cask fuse-t (or --cask macfuse)" \
                             || echo "apt install libfuse-dev" ;;
    openssh)  [ "$MAC" = 1 ] && echo "preinstalled with macOS (or brew install openssh)" \
                             || echo "apt install openssh-client" ;;
    *)        [ "$MAC" = 1 ] && echo "brew install $1" || echo "apt install $1" ;;
    esac
}

missing=0

# row STATUS NAME DETAIL: one aligned line.
row() { printf '  %-10s %-18s %s\n' "$1" "$2" "$3"; }

# req NAME HINTKEY: a tool the build/run needs. It must both EXIST and RUN (a bare
# `command -v` would PASS a broken Xcode shim that has no real Command Line Tools),
# so it is probed with --version. Absence/breakage is a failure.
req() {
    if command -v "$1" >/dev/null 2>&1 && "$1" --version >/dev/null 2>&1; then
        row "[PASS]" "$1" "$(command -v "$1")"
    else
        row "[MISSING]" "$1" "-- $(hint "$2")"
        missing=$((missing + 1))
    fi
}

# opt NAME WHAT HINTKEY: a tool only some tasks need; absence is a note, not a
# failure (these are dev/quality tools, so a plain presence check is enough).
opt() {
    if command -v "$1" >/dev/null 2>&1; then
        row "[PASS]" "$1" "$(command -v "$1")"
    else
        row "[OPTIONAL]" "$1" "($2) -- $(hint "$3")"
    fi
}

echo "Rach2 toolchain check (make doctor)"
echo
echo "Required (build + run the OS):"
req i686-elf-gcc     cross
req i686-elf-ld      binutils
req i686-elf-strip   binutils
req qemu-system-i386 qemu
req pkg-config       pkgconf
req make             make
req cc               cc

# FUSE for the krffs host tools (fuse-t on macOS, fuse on Linux), checked via
# pkg-config. If pkg-config is missing OR broken (same probe req() uses for it),
# FUSE is uncheckable; report that without counting it a second time (pkg-config
# is already the failure above).
if ! pkg-config --version >/dev/null 2>&1; then
    row "[SKIP]" "FUSE" "-- cannot check until pkg-config works"
elif pkg-config --exists fuse 2>/dev/null; then
    row "[PASS]" "FUSE" "fuse ($(pkg-config --modversion fuse 2>/dev/null))"
elif pkg-config --exists fuse-t 2>/dev/null; then
    row "[PASS]" "FUSE" "fuse-t ($(pkg-config --modversion fuse-t 2>/dev/null))"
else
    row "[MISSING]" "FUSE" "-- $(hint fuse)"
    missing=$((missing + 1))
fi

echo
echo "Optional (only some tasks need these):"
opt cppcheck     "make verify-style" cppcheck
opt shellcheck   "make verify-style" shellcheck
opt clang-format "make verify-style" clang-format
opt ssh-keygen   "the SSH capstone demo key" openssh

echo
if [ "$missing" -eq 0 ]; then
    echo "Result: all required tools present."
    exit 0
fi
echo "Result: $missing required tool(s) missing -- install them (hints above), then re-run 'make doctor'."
exit 1
