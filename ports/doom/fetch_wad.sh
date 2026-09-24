#!/usr/bin/env sh
#
# fetch_wad.sh: check that the Doom game data (an IWAD) the Doom capstone needs
# is in place, and verify it against a known-good hash.
#
# The WAD is operator-supplied: it is game data, not built, and never committed
# (shareware or commercial content, git-ignored via ports/doom/.gitignore). So a
# fresh clone has the doom binary but no game data, and doom then starts and
# exits with status 255 after "IWAD file '/demo/doom1.wad' not found!". Put a
# Doom IWAD at ports/doom/doom1.wad; the os-real manifest stages that file to
# /demo/doom1.wad in the image.
#
# Usage: ./fetch_wad.sh   (run it before `make os-real-image` on a fresh clone)
#
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR" || exit 1

# The id Software Doom shareware WAD (v1.9) is freely redistributable; this is
# its canonical sha256. Any other IWAD the port recognizes (retail doom.wad, the
# BSD-licensed freedoom1.wad, and the rest in src/d_iwad.c) also works, placed
# at this same path; it is simply not hash-pinned here.
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
