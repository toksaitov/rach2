#!/bin/sh
# Build patched Windows QEMU in one private directory under build/tests.
set -eu
if test "$#" -lt 1 || test "$#" -gt 2; then
    echo 'usage: build.sh PRIVATE_ROOT [--resume-source]' >&2
    exit 2
fi
base=$(cd "$1" && pwd -P)
input=$base/inputs
resume=${2-}
case $resume in ''|--resume-source) ;; *) echo 'invalid resume mode' >&2; exit 2;; esac
case $base in */build/tests/*) ;; *) echo 'private root must be under build/tests' >&2; exit 2;; esac
if test -n "$resume"; then
    test -d "$base/deps/ucrt64"
    test -d "$base/qemu-11.1.2"
    outputs="build install"
else
    outputs="deps qemu-11.1.2 build install"
fi
for name in $outputs; do
    test ! -e "$base/$name" || { echo "output already exists: $base/$name" >&2; exit 1; }
done
/usr/bin/python3 - "$input" <<'PY'
import hashlib,json,pathlib,sys
p=pathlib.Path(sys.argv[1]);d=json.loads((p/'pins.json').read_text())
checks=[(d['source']['filename'],d['source']['sha256'],d['source']['size_bytes']),
        ('whpx-live-xsave-size.patch',d['patch_sha256'],None)]
b=json.loads((p/'dependencies.json').read_text())
checks.append((b['filename'],b['sha256'],b['size_bytes']))
for name,expected,size in checks:
 f=p/name;h=hashlib.sha256()
 with f.open('rb') as stream:
  for data in iter(lambda:stream.read(1048576),b''):h.update(data)
 assert h.hexdigest()==expected and (size is None or f.stat().st_size==size),name
print('pinned source, patch and private dependency bundle verified')
PY
if test -z "$resume"; then
    mkdir "$base/deps"
    tar -xzf "$input/qemu-ucrt-dependencies.tar.gz" -C "$base/deps"
fi
dep=$base/deps/ucrt64
PATH=$dep/bin:/ucrt64/bin:/usr/bin
export PATH
unset MSYS2_ARG_CONV_EXCL MSYS_NO_PATHCONV MSYS2_ENV_CONV_EXCL
unset PYTHONHOME PYTHONPATH
PYTHONNOUSERSITE=1
export PYTHONNOUSERSITE
native_dep=$(cygpath -am "$dep")
"$dep/bin/python.exe" - "$native_dep" "$resume" <<'PY'
from pathlib import Path
import sys
root=Path(sys.argv[1]);assert Path(sys.prefix).resolve()==root.resolve()
if not sys.argv[2]:
 for p in (root/'lib/pkgconfig').glob('*.pc'):
  text=p.read_text();assert '/mingw64' not in text,p
  if '/ucrt64' in text:p.write_text(text.replace('/ucrt64',root.as_posix()),newline='\n')
print('private Python prefix:',sys.prefix)
PY
PKG_CONFIG=$dep/bin/pkgconf.exe
PKG_CONFIG_PATH=$native_dep/lib/pkgconfig
PKG_CONFIG_LIBDIR=$PKG_CONFIG_PATH
export PKG_CONFIG PKG_CONFIG_PATH PKG_CONFIG_LIBDIR
for package in glib-2.0 gthread-2.0 pixman-1 slirp zlib; do
    "$PKG_CONFIG" --modversion "$package"
    "$PKG_CONFIG" --variable=prefix "$package"
done
if test -z "$resume"; then
    tar -xJf "$input/qemu-11.1.2.tar.xz" -C "$base" \
        --exclude=qemu-11.1.2/roms --exclude=qemu-11.1.2/tests/lcitool
fi
source=$base/qemu-11.1.2
/usr/bin/python3 - "$source" "$input/pins.json" <<'PY'
from pathlib import Path
import sys,json,hashlib
p=Path(sys.argv[1]);d=json.loads(Path(sys.argv[2]).read_text())
assert (p/'VERSION').read_text().strip()==d['version']
assert hashlib.sha256((p/'target/i386/whpx/whpx-all.c').read_bytes()).hexdigest()==d['source_whpx_sha256']
PY
patch -p1 -d "$source" < "$input/whpx-live-xsave-size.patch"
mkdir "$base/build"
cd "$base/build"
CC=/ucrt64/bin/gcc.exe
CXX=/ucrt64/bin/g++.exe
export CC CXX
"$CC" --version
"$dep/bin/python.exe" --version
"$dep/bin/ninja.exe" --version
"$source/configure" --prefix="$base/install" \
    --python="$dep/bin/python.exe" --ninja="$dep/bin/ninja.exe" \
    --target-list=i386-softmmu,x86_64-softmmu --without-default-features \
    --enable-system --disable-user --enable-whpx --enable-slirp \
    --enable-pixman --disable-tools --disable-docs --disable-guest-agent \
    --disable-rust --disable-download --with-pkgversion=rach2-whpx-d0
"$dep/bin/ninja.exe" -j8 qemu-system-i386.exe qemu-system-x86_64.exe
./qemu-system-i386.exe --version
./qemu-system-x86_64.exe --version
sha256sum qemu-system-i386.exe qemu-system-x86_64.exe
