#!/usr/bin/env sh
#
# tools/bootstrap_mac.sh: install what a fresh Mac needs, from the manifest.
#
# Reads tools/mac-formulae.txt so the list lives in one place a person can read
# and a script can act on. Installs nothing that is already there, prints what
# it would do under --dry-run, and leaves the judgement of whether the machine
# is ready to `make doctor`, which is the thing that actually knows.
#
# Xcode's command line tools (make, cc, python3, perl) are not Homebrew's to
# install; `xcode-select --install` is, and this says so rather than pretending.
set -eu

REPO="$(cd "$(dirname "$0")/.." && pwd)"
MANIFEST="$REPO/tools/mac-formulae.txt"
DRY=0
[ "${1:-}" = "--dry-run" ] && DRY=1

[ "$(uname -s)" = "Darwin" ] || {
    echo "bootstrap_mac: this is for macOS; on Linux see the hints in make doctor" >&2
    exit 1
}
command -v brew >/dev/null 2>&1 || {
    echo "bootstrap_mac: Homebrew is not installed; see https://brew.sh" >&2
    exit 1
}
command -v cc >/dev/null 2>&1 || echo "bootstrap_mac: run 'xcode-select --install' too"

while read -r kind name _rest; do
    case "$kind" in ''|'#'*) continue ;; esac
    case "$kind" in
    formula) args="$name" ;;
    cask)    args="--cask $name" ;;
    *)       echo "bootstrap_mac: $MANIFEST: unknown kind: $kind" >&2; exit 1 ;;
    esac
    # shellcheck disable=SC2086
    if brew list $args >/dev/null 2>&1; then
        echo "bootstrap_mac: $name: already installed"
        continue
    fi
    if [ "$DRY" = 1 ]; then
        echo "bootstrap_mac: would install: brew install $args"
    else
        echo "bootstrap_mac: installing $name"
        # shellcheck disable=SC2086
        brew install $args
    fi
done < "$MANIFEST"

echo "bootstrap_mac: done; now run 'make doctor'"
