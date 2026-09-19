#!/usr/bin/env sh
#
# preflight.sh: check the build's own tools before compilation, cheaply. The
# root `make` and `tests/run` invoke it at each entry; `clean` and `distclean`
# do not, so cleanup works with a broken toolchain.
#
# The check is cached in build/.preflight. The cache records the host OS and
# architecture, this script's own identity, and each required tool's resolved
# path with its size and modification time. A matching cache returns after a
# few stats, with no tool executed. Any difference (a tool missing, moved,
# reinstalled, or this script changed) repeats the full probes, which compile
# a host and a cross test program, and rewrites the cache. A missing tool
# fails with its name and an installation hint before any compilation starts.
#
# `--report` runs the full probes, prints one line per tool, and writes no
# cache. `tools/doctor.sh` remains the wider machine-setup report covering
# QEMU, style, and optional tools.
#
set -u

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
STAMP="$REPO/build/.preflight"
SELF="$REPO/tools/setup/preflight.sh"

# The tools the default build needs. Suites declare their own runtime tools
# in tests/registry; QEMU is a suite tool, not a build tool. The three
# e2fsprogs programs build and check the ext2 root image: mke2fs writes it,
# debugfs sets the owners a build that is not root cannot, e2fsck checks it.
TOOLS="make cc i686-elf-gcc i686-elf-ld i686-elf-ar i686-elf-strip \
       i686-elf-objcopy awk sed tar gzip bzip2 patch ssh-keygen \
       mke2fs debugfs e2fsck mandoc"

# Where e2fsprogs is found when it is not on the path: the Homebrew keg, which
# does not link it, and the administrator's directories Debian keeps it in.
# tools/image/mkext2.sh looks in the same places.
. "$(dirname "$0")/e2fsprogs_path.sh"

hint() {
    case "$1" in
        i686-elf-*) echo "brew install i686-elf-gcc i686-elf-binutils, or see tools/Readme.md" ;;
        cc)         echo "install Xcode Command Line Tools or a C compiler package" ;;
        ssh-keygen) echo "install the OpenSSH client package" ;;
        mke2fs|debugfs|e2fsck) echo "brew install e2fsprogs, or apt install e2fsprogs" ;;
        mandoc)     echo "brew install mandoc, or apt install mandoc; the images carry the manual pages it renders" ;;
        *)          echo "install it with your package manager" ;;
    esac
}

# One cache line per tool: name|path|size|mtime, from stat without executing
# the tool. A reinstalled tool changes size or mtime at the same path.
fingerprint() {
    printf 'format|2\n'
    printf 'os|%s %s\n' "$(uname -s)" "$(uname -m)"
    for t in self $TOOLS; do
        if [ "$t" = self ]; then
            p=$SELF
        else
            p=$(command -v "$t" 2>/dev/null) || { printf '%s|missing\n' "$t"; continue; }
        fi
        # shellcheck disable=SC2012,SC2046 # stat formats differ; ls -ln is
        # portable, and the word split is the point
        set -- $(ls -lnT "$p" 2>/dev/null || ls -ln "$p" 2>/dev/null)
        printf '%s|%s|%s|%s %s %s %s\n' "$t" "$p" "${5-}" "${6-}" "${7-}" "${8-}" "${9-}"
    done
}

# The full probes: every tool present, the host compiler compiles and links,
# the cross compiler compiles a freestanding i386 object. Under --report each
# tool gets one line; otherwise the first failure is fatal.
probe() {
    report=${1-}
    ok=0
    for t in $TOOLS; do
        if p=$(command -v "$t" 2>/dev/null); then
            [ -n "$report" ] && printf 'preflight: %-16s %s\n' "$t" "$p"
        else
            echo "preflight: missing $t: $(hint "$t")" >&2
            [ -n "$report" ] || return 1
            ok=1
        fi
    done
    work=$(mktemp -d) || return 1
    trap 'rm -rf "$work"' EXIT INT TERM
    printf 'int main(void) { return 0; }\n' >"$work/t.c"
    if ! cc -o "$work/t" "$work/t.c" 2>"$work/err"; then
        echo "preflight: the host C compiler cannot build a program:" >&2
        sed 's/^/  /' "$work/err" >&2
        return 1
    fi
    if ! i686-elf-gcc -m32 -ffreestanding -c -o "$work/t.o" "$work/t.c" 2>"$work/err"; then
        echo "preflight: i686-elf-gcc cannot compile a freestanding object:" >&2
        sed 's/^/  /' "$work/err" >&2
        return 1
    fi
    # mke2fs must take the options mkext2.sh gives it: a source directory and
    # a named feature set. An e2fsprogs old enough to lack -d would build no
    # image and fail later with less to say.
    if ! mke2fs 2>&1 | grep -q -- '-d root-directory'; then
        echo "preflight: mke2fs does not take -d; e2fsprogs 1.43 or later is needed" >&2
        return 1
    fi
    [ -n "$report" ] && echo "preflight: result: host and cross compile probes passed"
    return $ok
}

case "${1-}" in
    --report)
        probe report
        exit $?
        ;;
    '') ;;
    *)
        echo "usage: preflight.sh [--report]" >&2
        exit 2
        ;;
esac

current=$(fingerprint)
if [ -f "$STAMP" ] && [ "$current" = "$(cat "$STAMP")" ]; then
    case "$current" in
        *'|missing'*) ;;
        *) exit 0 ;;
    esac
fi

[ -f "$STAMP" ] && echo "preflight: environment changed; rechecking" >&2
probe '' || exit 1
mkdir -p "$REPO/build"
printf '%s\n' "$current" >"$STAMP"
exit 0
