#!/usr/bin/env sh
#
# fetch_wad.sh: check that Doom's game data, an IWAD, is at
# ports/doom/doom1.wad, and say whether it is the shareware Doom 1.9
# doom1.wad, by its SHA-256 digest. It downloads nothing, and exits with
# status 1 when there is no file.
#
# The tree has no game data, and ports/doom/.gitignore keeps a WAD out of
# Git. Without one the game exits with status 255 after "IWAD file
# '/demo/doom1.wad' not found!". The manifests that stage the game stage
# this file as /demo/doom1.wad.
#
# Usage: sh ports/doom/fetch_wad.sh   (before make builds the images)
#
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR" || exit 1

# The SHA-256 of the shareware Doom 1.9 doom1.wad, which id Software lets
# anyone copy. Another IWAD that src/d_iwad.c knows, such as the retail
# doom.wad or Freedoom's freedoom1.wad, also plays when saved at this path,
# and the script prints its digest.
WAD="doom1.wad"
SHAREWARE_SHA256="1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771"

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

if [ -f "$WAD" ]; then
    got="$(sha256_of "$WAD")"
    if [ "$got" = "$SHAREWARE_SHA256" ]; then
        echo "doom: $WAD present and verified (id Software Doom shareware v1.9)"
    else
        echo "doom: $WAD present (sha256 $got): not the pinned shareware WAD, but"
        echo "doom: any Doom IWAD the port recognizes is fine"
    fi
    exit 0
fi

echo "doom: no IWAD at $SCRIPT_DIR/$WAD; the Doom capstone cannot start without one" >&2
echo "  Option A (id Software Doom shareware, freely redistributable):" >&2
echo "    place doom1.wad here; its sha256 must be" >&2
echo "    $SHAREWARE_SHA256" >&2
echo "  Option B (license-clean): download freedoom1.wad from" >&2
echo "    https://freedoom.github.io and save it here as doom1.wad" >&2
echo "  Then rebuild the os-real image: make os-real-image" >&2
exit 1
