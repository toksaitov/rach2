#!/usr/bin/env sh
#
# scripts.sh: print every in-house shell script, one path a line, relative to
# the repository's root and sorted.
#
# The style check runs ShellCheck over this list and the text check reads it,
# so the two cannot come to check different files. A script is a file whose
# name ends in `.sh`, a script the images carry, whose name ends in `.script`,
# or a file with no suffix whose first line runs a shell, such as `tests/run`,
# `utils/rach2-install` and the `muslgcc` wrappers. The vendored trees are left
# out: the upstream sources the vendor builds extract, musl's install, the Doom
# engine's sources, and termbox2 and the Dokany project under krffs/.
#
# Usage: scripts.sh
#

set -u

cd "$(dirname "$0")/../.." || exit 1
find rach2 krffs utils ish tests tools mk man ports vendor \
    -path vendor/musl/install -prune -o \
    -path 'vendor/musl/musl-*' -prune -o \
    -path 'vendor/busybox/busybox-*' -prune -o \
    -path 'vendor/tcc/tcc-*' -prune -o \
    -path 'vendor/pdpmake/pdpmake-*' -prune -o \
    -path 'vendor/dropbear/dropbear-*' -prune -o \
    -path 'vendor/e2fsprogs/e2fsprogs-*' -prune -o \
    -path ports/doom/src -prune -o \
    -path krffs/vendor -prune -o -path krffs/vs-dokany -prune -o \
    -type f \( -name '*.sh' -o -name '*.script' -o ! -name '*.*' \) -print |
while IFS= read -r f; do
    case $f in
        *.sh | *.script) echo "$f" ;;
        *) head -n 1 "$f" |
            grep -qE '^#![[:space:]]*/[^[:space:]]*/(env[[:space:]]+)?(ba|da)?sh([[:space:]]|$)' &&
            echo "$f" ;;
    esac
done | LC_ALL=C sort
