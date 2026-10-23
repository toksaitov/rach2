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
# Tracked sources and the extracted sources the tests compile enter this tree.
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
# These are the only extracted musl bytes the inventories compile. Both
# paths remain distinct because architecture metadata selects between them.
for prefix in ('vendor', 'build/x86_64/vendor-work'):
    relative = Path(prefix) / 'musl/musl-1.2.5/src/crypt/crypt_sha512.c'
    source = repo / relative
    if not source.is_file():
        raise ValueError(f'host inventory: missing extracted source: {source}')
    output = tree / relative
    output.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, output)
    library = Path(prefix) / 'e2fsprogs/e2fsprogs-1.47.4/lib'
    if not (repo / library / 'ext2fs/unix_io.c').is_file():
        raise ValueError(f'host inventory: missing patched e2fsprogs library: {library}')
    for source in (repo / library).rglob('*'):
        if source.is_symlink():
            raise ValueError(f'host inventory: extracted source must be regular: {source}')
        if source.is_file() and (source.suffix == '.h' or source.name == 'unix_io.c'):
            output = tree / source.relative_to(repo)
            output.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, output)
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
export HOST_TEST_PREPARED_TOOLS="$TOOLS"
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
