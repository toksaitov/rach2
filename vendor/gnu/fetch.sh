#!/usr/bin/env sh

#
# fetch.sh: download the pinned tarballs of the GNU layer and check them.
#
# Usage: fetch.sh [NAME...]
#        fetch.sh --path NAME
#
# sources.txt gives each tarball's version, URL and SHA-256. A tarball is
# downloaded into downloads/ once and checked against its digest on every
# call; a file with another digest is removed and refused. With no NAME every
# tarball is fetched. --path fetches one and prints its path.
#

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
DL="$DIR/downloads"
SHA="$REPO/build/tools/verify/sha256"

sha256_of()
{
    if [ ! -x "$SHA" ]; then
        mkdir -p "$(dirname "$SHA")"
        cc -O2 -o "$SHA" "$REPO/tools/verify/sha256.c" || return 1
    fi
    "$SHA" "$1" | awk '{print $1}'
}

download()
{
    if command -v curl >/dev/null 2>&1; then
        curl -sSfL -o "$2" "$1"
    else
        wget -q -O "$2" "$1"
    fi
}

# fetch_one NAME: print the checked tarball's path, fetching it if needed.
fetch_one()
{
    line="$(awk -v n="$1" '$1 == n { print; exit }' "$DIR/sources.txt")"
    if [ -z "$line" ]; then
        echo "fetch: $1: not in sources.txt" >&2
        return 1
    fi
    set -- $line
    url="$3"
    want="$4"
    file="$DL/${url##*/}"
    mkdir -p "$DL"
    if [ ! -f "$file" ]; then
        echo "fetch: $1 $2: downloading ${url##*/}" >&2
        download "$url" "$file.part" || {
            rm -f "$file.part"
            echo "fetch: $1: download failed: $url" >&2
            return 1
        }
        mv "$file.part" "$file"
    fi
    got="$(sha256_of "$file")"
    if [ "$got" != "$want" ]; then
        rm -f "$file"
        echo "fetch: $1: refused: sha256 $got, pinned $want" >&2
        return 1
    fi
    echo "$file"
}

if [ "$1" = "--path" ]; then
    fetch_one "$2"
    exit
fi
if [ $# -eq 0 ]; then
    set -- $(awk '$1 !~ /^#/ && NF { print $1 }' "$DIR/sources.txt")
fi
for name in "$@"; do
    fetch_one "$name" >/dev/null
done
