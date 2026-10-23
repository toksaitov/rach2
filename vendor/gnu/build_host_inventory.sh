#!/bin/sh
# Prepare GNU-only host checks and immutable source inputs, without running one.
set -eu
REPO=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
. "$REPO/vendor/gnu/arch.sh"
[ "$RACH2_ARCH" = x86_64 ] || {
    echo 'host inventory: prepare the fixed x86-64 GNU helper' >&2
    exit 1
}
OUT=$GNU_OUT/host-checks
TREE=$OUT/tree
CC=$GNU_OUT/cross/bin/$GNU_TARGET-gcc
SAN=$GNU_OUT/sanitizers
rm -rf "$OUT"
mkdir -p "$TREE"
cd "$REPO"
# Only tracked source and the extracted musl inputs enter this small tree.
if git rev-parse --git-dir > /dev/null 2>&1; then
    git ls-files -z -- Makefile tests tools mk utils krffs rach2 ish ports vendor > "$OUT/sources.list"
else
    cp build/gnu-source-files "$OUT/sources.list"
fi
python3 - "$REPO" "$TREE" "$OUT/sources.list" <<'PY'
from pathlib import Path
import shutil, sys
repo, tree = map(Path, sys.argv[1:3])
for name in Path(sys.argv[3]).read_bytes().split(b'\0'):
    if not name:
        continue
    relative = Path(name.decode())
    source = repo / relative
    if not source.is_file() or source.suffix in ('.xz', '.gz', '.bz2', '.zst', '.wad', '.img'):
        continue
    output = tree / relative
    output.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, output)
for base in ('vendor/musl/musl-1.2.5', 'build/x86_64/vendor-work/musl/musl-1.2.5'):
    source = repo / base
    if source.is_dir():
        shutil.copytree(source, tree / base, dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns('*.o', '*.a', '*.so', 'obj', 'lib'))
PY
mkdir -p "$TREE/build/tools/etude" "$TREE/build/tests/cases/host"
cp "$GNU_OUT/host-products/etude" "$TREE/build/tools/etude/etude"
cp "$GNU_OUT/host-products/host" "$TREE/build/tests/cases/host/host"
# These programs already belong to the helper's GNU root. They never become
# dependencies of the desktop running the collection.
TOOLS=$OUT/tool-inputs
mkdir -p "$TOOLS"
for name in tar mdir mcopy mke2fs debugfs dumpe2fs e2fsck python3; do
    source=
    for dir in "$GNU_OUT/root/usr/bin" "$GNU_OUT/root/usr/sbin" "$GNU_OUT/root/bin" \
        "$GNU_OUT/root/sbin" "$GNU_OUT/host-products"; do
        if [ -f "$dir/$name" ]; then source=$dir/$name; break; fi
    done
    [ -n "$source" ] || { echo "host inventory: GNU helper lacks $name" >&2; exit 1; }
    cp -L "$source" "$TOOLS/$name"
done
# Independent exFAT bytes were produced once by the build's external writer.
[ -f "$REPO/build/tests/host/exfat_writer" ] || {
    echo 'host inventory: prepare the independent exFAT fixture first' >&2
    exit 1
}
mkdir -p "$TREE/build/tests/host" "$TREE/build/tests/host-suite/tools"
cp "$REPO/build/tests/host/exfat_writer" "$TREE/build/tests/host/"
cp -R "$REPO/build/tests/host-suite/tools/exfat_external" "$TREE/build/tests/host-suite/tools/"
cd "$TREE"
export HOST_TEST_PLATFORM=posix
export HOST_TEST_PREPARED_TOOLS=$TOOLS
export CC="$CC --sysroot=$GNU_OUT/root -B$SAN/compiler/ -L$SAN/root/usr/lib"
for arch in i686 x86_64; do
    case $arch in i686) base=build ;; *) base=build/x86_64 ;; esac
    python3 -B tests/cases/host/prepare.py --gnu --arch "$arch" tests "$base/tests/host-suite/tests.inventory.json"
done
for group in etude checkers fuzz; do
    python3 -B tests/cases/host/prepare.py --gnu "$group" "build/tests/host-suite/$group.inventory.json"
done
"$GNU_OUT/cross/bin/$GNU_TARGET-gcc" --sysroot="$GNU_OUT/root" -std=c99 -D_DEFAULT_SOURCE \
    -Wall -Wextra -Werror "$REPO/tests/cases/host/gnu_driver.c" "$REPO/tests/cases/host/fixture.c" -o "$OUT/driver"
chmod -R a+rX "$TREE"
rm -rf "$TOOLS" "$OUT/sources.list"
