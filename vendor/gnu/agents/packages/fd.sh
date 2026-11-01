# shellcheck shell=sh disable=SC2034 # fetch_agents.sh reads the variables
# fd, a search for file names in a tree; the author's static musl build,
# pinned by the SHA-256 of the release asset in GitHub's API. The archive
# includes both licenses, MIT and Apache-2.0, and is kept under /opt/fd.
VERSION=10.5.0
LICENSE="MIT OR Apache-2.0"
TARBALL=fd-v$VERSION-x86_64-unknown-linux-musl.tar.gz
FILES="$TARBALL https://github.com/sharkdp/fd/releases/download/v$VERSION/$TARBALL sha256:761c72dc8e120d85b22292063be8a796e2eeb20eb3e4f38b8fa2343ccf3514a7"
NOTICES="opt/fd/LICENSE-MIT opt/fd/LICENSE-APACHE"

package_stage()
{
    mkdir -p "$1/opt/fd" "$1/usr/bin"
    tar -xzf "$(fetched "$TARBALL")" -C "$1/opt/fd" --strip-components=1 --no-same-owner
    ln -s ../../opt/fd/fd "$1/usr/bin/fd"
}

# fd's version command writes no files.
package_check()
{
    out=$("$1/opt/fd/fd" --version)
    if [ "$out" != "fd $VERSION" ]; then
        echo "fd: --version printed [$out], expected fd $VERSION" >&2
        return 1
    fi
}
