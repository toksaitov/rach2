# shellcheck shell=sh disable=SC2034 # fetch_agents.sh reads the variables
# rg, ripgrep, which searches a tree of files for lines that match a regular
# expression and skips what .gitignore names; a static Rust program built for
# musl, its author's build. The release signs no file: the pin is the SHA-256
# that the tarball's own file, NAME.sha256, lists. The tarball is unpacked as
# published into /opt/ripgrep, and /usr/bin/rg links to the program. Its
# COPYING, LICENSE-MIT and UNLICENSE are the files of the tag 15.2.0 that
# codex.sh pins for the copy of ripgrep in Codex's package, byte for byte.
VERSION=15.2.0
LICENSE="Unlicense OR MIT"
TARBALL=ripgrep-$VERSION-x86_64-unknown-linux-musl.tar.gz
FILES="$TARBALL https://github.com/BurntSushi/ripgrep/releases/download/$VERSION/$TARBALL sha256:33e15bcf1624b25cdd2a55813a47a2f95dbe126268203e76aa6a585d1e7b149c"
NOTICES="opt/ripgrep/COPYING opt/ripgrep/LICENSE-MIT opt/ripgrep/UNLICENSE"

package_stage()
{
    mkdir -p "$1/opt/ripgrep" "$1/usr/bin"
    tar -xzf "$(fetched "$TARBALL")" -C "$1/opt/ripgrep" --strip-components=1 --no-same-owner
    ln -s ../../opt/ripgrep/rg "$1/usr/bin/rg"
}

# rg writes no file and needs no home for --version.
package_check()
{
    out=$("$1/opt/ripgrep/rg" --version | sed -n 1p)
    case $out in
        "ripgrep $VERSION "*) ;;
        *) echo "ripgrep: --version printed [$out], not ripgrep $VERSION" >&2; return 1 ;;
    esac
}
