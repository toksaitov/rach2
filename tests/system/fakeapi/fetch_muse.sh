#!/usr/bin/env sh

#
# fetch_muse.sh: Muse Code's Linux x86-64 binary at the version the museapi
# suite pins, as Meta publishes it, checked against the digest Meta lists.
#
# Usage: fetch_muse.sh
#
# Meta's installer reads the channel manifest at
# https://api.meta.ai/muse-code/channels/muse-stable, which names the current
# version and its release manifest; the release manifest lists each
# platform's file with its URL, size and SHA-256. VERSION, URL and SHA256
# below are the x86_linux entry of the release manifest of VERSION. The file
# is downloaded once into build/downloads/muse/ and checked against SHA256 on
# every call; a file with another digest is removed and refused. Prints the
# path of the checked program.
#

set -e

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
DL="$REPO/build/downloads/muse"
SHA="$REPO/build/tools/verify/sha256"
VERSION=1.4.10-R6427.1
URL="https://lookaside.facebook.com/lookaside/muse/download/?channel=muse&version=$VERSION&file=muse-x86-linux"
SHA256=decefc0fb33c48ee43419c3e72ac2dbc781743868611074c7292807f5960dfcf
FILE="$DL/muse-x86-linux-$VERSION"

sha256_of()
{
    make -s -C "$REPO" build/tools/verify/sha256 || return 1
    "$SHA" "$1" | awk '{print $1}'
}

# download URL FILE: write URL to FILE, trying up to three times.
download()
{
    tries=1
    while :; do
        curl -sSfL --proto '=https' --proto-redir '=https' --max-redirs 3 \
            -o "$2" "$1" && return 0
        [ "$tries" -lt 3 ] || return 1
        tries=$((tries + 1))
        echo "fetch_muse: retrying: $1" >&2
        sleep 10
    done
}

mkdir -p "$DL"
if [ ! -f "$FILE" ]; then
    echo "fetch_muse: downloading Muse Code $VERSION" >&2
    download "$URL" "$FILE.part" || {
        rm -f "$FILE.part"
        echo "fetch_muse: download failed: $URL" >&2
        exit 1
    }
    mv "$FILE.part" "$FILE"
fi
got="$(sha256_of "$FILE")"
if [ "$got" != "$SHA256" ]; then
    rm -f "$FILE"
    echo "fetch_muse: refused: sha256 $got, pinned $SHA256" >&2
    exit 1
fi
chmod 0755 "$FILE"
echo "$FILE"
