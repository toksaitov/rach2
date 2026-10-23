#!/bin/sh
# build_openssl.sh: prepare static host libraries from the GNU layer's pin.
# The C services need no OpenSSL executable, modules or shared libraries.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
output=$repo/build/vendor/host-openssl
compiler=${FAKEAPI_CC:-cc}
archive=$(sh "$repo/vendor/gnu/fetch.sh" --path openssl)
case ${OS:-}:$(uname -s):$(uname -m) in
    Windows_NT:*|*:MINGW*:*|*:MSYS*:*) target=mingw64 ;;
    *:Darwin:arm64) target=darwin64-arm64-cc ;;
    *:Darwin:x86_64) target=darwin64-x86_64-cc ;;
    *:Linux:x86_64) target=linux-x86_64 ;;
    *:Linux:aarch64) target=linux-aarch64 ;;
    *) echo 'fakeapi: no static OpenSSL build target for this host' >&2; exit 2 ;;
esac
source_dir=$output/source-${archive##*/}
mkdir -p "$output"
# Only this rule writes the directory, under make's ordinary target ownership.
# Reconfiguration keeps compilation incremental when the pin is unchanged.
if [ ! -f "$source_dir/Configure" ]; then
    mkdir -p "$source_dir"
    tar -xf "$archive" --strip-components=1 -C "$source_dir"
fi
cd "$source_dir"
CC="$compiler" perl ./Configure "$target" no-shared no-module no-dso no-tests \
    no-zlib no-legacy --prefix="$output/install" --libdir=lib
make build_libs
mkdir -p "$output/lib" "$output/include"
cp libssl.a libcrypto.a "$output/lib/"
cp -R include/openssl "$output/include/"
touch "$output/ready"
