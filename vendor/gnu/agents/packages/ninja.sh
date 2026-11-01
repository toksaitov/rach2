# shellcheck shell=sh disable=SC2034 # fetch_agents.sh reads the variables
# ninja, a build tool for generated dependency graphs; the project's
# Linux build, pinned by the SHA-256 of the release asset in GitHub's API.
# The zip has only the program; COPYING comes from its source tag, pinned
# by its SHA-256, and is installed beside the program under /opt/ninja.
VERSION=1.13.2
LICENSE=Apache-2.0
FILES="ninja-$VERSION-linux.zip https://github.com/ninja-build/ninja/releases/download/v$VERSION/ninja-linux.zip sha256:5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6
ninja-$VERSION-COPYING https://raw.githubusercontent.com/ninja-build/ninja/v$VERSION/COPYING sha256:eb7e9ab9690124c5c9f42bdc81383d886a3dede26345b6ed15bbad7caf81f7ea"
NOTICES=opt/ninja/COPYING

package_stage()
{
    mkdir -p "$1/opt/ninja" "$1/usr/bin"
    python3 -m zipfile -e "$(fetched "ninja-$VERSION-linux.zip")" "$1/opt/ninja"
    cp "$(fetched "ninja-$VERSION-COPYING")" "$1/opt/ninja/COPYING"
    chmod 0755 "$1/opt/ninja/ninja"
    chmod 0644 "$1/opt/ninja/COPYING"
    ln -s ../../opt/ninja/ninja "$1/usr/bin/ninja"
}

# Ninja's version command writes no files.
package_check()
{
    out=$("$1/opt/ninja/ninja" --version)
    if [ "$out" != "$VERSION" ]; then
        echo "ninja: --version printed [$out], expected $VERSION" >&2
        return 1
    fi
}
