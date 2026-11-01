# shellcheck shell=sh disable=SC2034 # fetch_agents.sh reads the variables
# shellcheck, a static analyzer for shell scripts; the project's static
# Linux build, pinned by the SHA-256 of the release asset in GitHub's API.
# The archive has no license, so the GPL-3.0-or-later text comes from the
# release's source tag, pinned by its SHA-256, and is installed beside it.
VERSION=0.11.0
LICENSE=GPL-3.0-or-later
TARBALL=shellcheck-v$VERSION.linux.x86_64.tar.xz
FILES="$TARBALL https://github.com/koalaman/shellcheck/releases/download/v$VERSION/$TARBALL sha256:8c3be12b05d5c177a04c29e3c78ce89ac86f1595681cab149b65b97c4e227198
shellcheck-$VERSION-LICENSE https://raw.githubusercontent.com/koalaman/shellcheck/v$VERSION/LICENSE sha256:3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986"
NOTICES=opt/shellcheck/LICENSE

package_stage()
{
    mkdir -p "$1/opt/shellcheck" "$1/usr/bin"
    tar -xJf "$(fetched "$TARBALL")" -C "$1/opt/shellcheck" --strip-components=1 --no-same-owner
    cp "$(fetched "shellcheck-$VERSION-LICENSE")" "$1/opt/shellcheck/LICENSE"
    chmod 0644 "$1/opt/shellcheck/LICENSE"
    ln -s ../../opt/shellcheck/shellcheck "$1/usr/bin/shellcheck"
}

# ShellCheck's version command writes no files.
package_check()
{
    out=$("$1/opt/shellcheck/shellcheck" --version | sed -n 's/^version: //p')
    if [ "$out" != "$VERSION" ]; then
        echo "shellcheck: --version gave [$out], expected $VERSION" >&2
        return 1
    fi
}
