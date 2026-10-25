#!/bin/sh
# Build patched Windows QEMU in one private directory under build/tests.
set -eu
if test "$#" -lt 1 || test "$#" -gt 2; then
    echo 'usage: build.sh PRIVATE_ROOT [--resume-source|--resume-configure]' >&2
    exit 2
fi
base=$(cd "$1" && pwd -P)
input=$base/inputs
support=$(cd "$(dirname "$0")" && pwd -P)
resume=${2-}
case $resume in ''|--resume-source|--resume-configure) ;; *) echo 'invalid resume mode' >&2; exit 2;; esac
case $base in */build/tests/*) ;; *) echo 'private root must be under build/tests' >&2; exit 2;; esac
if test -n "$resume"; then
    test -d "$base/deps/ucrt64"
    test -d "$base/qemu-11.1.2"
    if test "$resume" = --resume-configure; then
        test -d "$base/build/pyvenv"
        test ! -e "$base/build/build.ninja"
        outputs="install"
    else
        outputs="build install"
    fi
else
    outputs="deps qemu-11.1.2 build install"
fi
for name in $outputs; do
    test ! -e "$base/$name" || { echo "output already exists: $base/$name" >&2; exit 1; }
done
/usr/bin/python3 - "$input" "$support" <<'PY'
import hashlib,json,pathlib,sys
p=pathlib.Path(sys.argv[1]);support=pathlib.Path(sys.argv[2]);d=json.loads((support/'pins.json').read_text())
checks=[(p/d['source']['filename'],d['source']['sha256'],d['source']['size_bytes']),
        (p/'whpx-live-xsave-size.patch',d['patch_sha256'],None)]
b=json.loads((p/'dependencies.json').read_text())
checks.append((p/b['filename'],b['sha256'],b['size_bytes']))
checks.extend((support/x['filename'],x['sha256'],x['size_bytes']) for x in d['python_wheels'])
for f,expected,size in checks:
 h=hashlib.sha256()
 with f.open('rb') as stream:
  for data in iter(lambda:stream.read(1048576),b''):h.update(data)
 assert h.hexdigest()==expected and (size is None or f.stat().st_size==size),str(f)
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
native_base=$(cygpath -am "$base")
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
PKG_CONFIG=$native_dep/bin/pkgconf.exe
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
/usr/bin/python3 - "$source" "$support" "$resume" <<'PY'
from pathlib import Path
import sys,json,hashlib,shutil
p=Path(sys.argv[1]);support=Path(sys.argv[2]);d=json.loads((support/'pins.json').read_text())
assert (p/'VERSION').read_text().strip()==d['version']
key='patched_whpx_sha256' if sys.argv[3]=='--resume-configure' else 'source_whpx_sha256'
assert hashlib.sha256((p/'target/i386/whpx/whpx-all.c').read_bytes()).hexdigest()==d[key]
for item in d['python_wheels']:
 target=p/'python/wheels'/item['filename']
 if target.exists():assert hashlib.sha256(target.read_bytes()).hexdigest()==item['sha256']
 else:shutil.copyfile(support/item['filename'],target)
PY
if test "$resume" != --resume-configure; then
    patch -p1 -d "$source" < "$input/whpx-live-xsave-size.patch"
    mkdir "$base/build"
fi
cd "$base/build"
CC=$(cygpath -am /ucrt64/bin/gcc.exe)
CXX=$(cygpath -am /ucrt64/bin/g++.exe)
export CC CXX
"$dep/bin/python.exe" - "$CC" "$CXX" "$PKG_CONFIG" \
    "$native_dep/bin/ninja.exe" "$native_base" <<'PY'
from pathlib import Path
import json,subprocess,sys
assert Path(sys.argv[5]).is_dir(),sys.argv[5]
rows=[];failed=False
for path in [sys.executable]+sys.argv[1:5]:
 try:
  r=subprocess.run([path,'--version'],capture_output=True,text=True)
  rows.append({'argv':[path,'--version'],'status':r.returncode,
               'stdout':r.stdout,'stderr':r.stderr})
  failed=failed or r.returncode!=0
 except OSError as e:
  rows.append({'argv':[path,'--version'],'error':str(e)});failed=True
print(json.dumps({'native_tool_preflight':rows},indent=2),flush=True)
assert not failed,'a native build tool could not execute'
PY
"$source/configure" --prefix="$native_base/install" \
    --python="$native_dep/bin/python.exe" --ninja="$native_dep/bin/ninja.exe" \
    --target-list=i386-softmmu,x86_64-softmmu --without-default-features \
    --enable-system --disable-user --enable-whpx --enable-slirp \
    --enable-pixman --enable-tools --disable-docs --disable-guest-agent \
    --disable-rust --disable-download --with-pkgversion=rach2-whpx-d0
"$dep/bin/ninja.exe" -j8 qemu-system-i386.exe qemu-system-x86_64.exe qemu-img.exe
./qemu-system-i386.exe --version
./qemu-system-x86_64.exe --version
./qemu-img.exe --version
sha256sum qemu-system-i386.exe qemu-system-x86_64.exe qemu-img.exe
