#!/bin/sh
# build_openssl.sh: prepare static host libraries from the GNU layer's pin.
# The C services need no OpenSSL executable, modules or shared libraries.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../../.." && pwd)
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
digest=$(awk '$1 == "openssl" {print $4; exit}' "$repo/vendor/gnu/sources.txt" "$repo/vendor/gnu/sources-extra.txt")
[ -n "$digest" ] || exit 2
source_dir=$output/source-$digest
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
# Publish fresh copies so read-only headers from the pinned archive are never
# opened for replacement. Keep the preceding generation until both moves end.
stage=
finished=
published=
cleanup_publish() {
    status=$?
    trap - EXIT HUP INT TERM
    [ -n "$finished" ] || [ "$status" -ne 0 ] || status=1
    if [ -n "$stage" ]; then
        if [ -z "$finished" ]; then
            for name in $published; do rm -rf "${output:?}/${name:?}"; done
            for name in lib include; do
                if [ -d "$stage/previous-$name" ]; then
                    mv "$stage/previous-$name" "$output/$name"
                fi
            done
        fi
        rm -rf "$stage"
    fi
    exit "$status"
}
trap cleanup_publish EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
stage=$(mktemp -d "$output/publish.XXXXXX")
mkdir "$stage/lib" "$stage/include"
cp libssl.a libcrypto.a "$stage/lib/"
cp -R include/openssl "$stage/include/"
for name in lib include; do
    if [ -e "$output/$name" ]; then
        mv "$output/$name" "$stage/previous-$name"
    fi
    # Track the destination before the move so a signal cannot omit rollback.
    published="$name $published"
    mv "$stage/$name" "$output/$name"
done
touch "$output/ready"
finished=1
