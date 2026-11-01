# shellcheck shell=sh disable=SC2034 # fetch_agents.sh reads the variables
# git-lfs, Git's large file storage extension; the project's static Go
# build, pinned by the SHA-256 its release lists in sha256sums.asc. The
# archive has no license, so LICENSE.md comes from its source tag, pinned
# by its SHA-256, and is installed beside the published files.
VERSION=3.8.1
LICENSE=MIT
TARBALL=git-lfs-linux-amd64-v$VERSION.tar.gz
FILES="$TARBALL https://github.com/git-lfs/git-lfs/releases/download/v$VERSION/$TARBALL sha256:e72cf723a4a660438862282e8aecf3885fc8123e88bfcad534cf6b941a354b75
git-lfs-$VERSION-LICENSE.md https://raw.githubusercontent.com/git-lfs/git-lfs/v$VERSION/LICENSE.md sha256:4fae9062ab5cdd5fb15486b728534d8aded8b4ae9d84b6d66a956f5162c366b6"
NOTICES=opt/git-lfs/LICENSE.md

package_stage()
{
    mkdir -p "$1/opt/git-lfs" "$1/usr/bin"
    tar -xzf "$(fetched "$TARBALL")" -C "$1/opt/git-lfs" --strip-components=1 --no-same-owner
    cp "$(fetched "git-lfs-$VERSION-LICENSE.md")" "$1/opt/git-lfs/LICENSE.md"
    chmod 0644 "$1/opt/git-lfs/LICENSE.md"
    ln -s ../../opt/git-lfs/git-lfs "$1/usr/bin/git-lfs"
}

# git-lfs's version command writes no files.
package_check()
{
    out=$("$1/opt/git-lfs/git-lfs" --version)
    case $out in
        "git-lfs/$VERSION ("*) ;;
        *) echo "git-lfs: --version printed [$out], expected git-lfs/$VERSION" >&2; return 1 ;;
    esac
}
