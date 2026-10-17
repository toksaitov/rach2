# shellcheck shell=sh disable=SC2034 # fetch_agents.sh reads the variables
# gh, GitHub's command-line program for repositories, issues, pull requests,
# releases and the API; a static Go program, GitHub's own build. GitHub
# signs no file of the release: the pin is the SHA-256 that the release's
# checksums file, gh_VERSION_checksums.txt, lists for the tarball. The
# tarball is unpacked as published into /opt/gh, its license beside the
# program, and /usr/bin/gh links to the program.
VERSION=2.102.0
LICENSE=MIT
FILES="gh_${VERSION}_linux_amd64.tar.gz https://github.com/cli/cli/releases/download/v$VERSION/gh_${VERSION}_linux_amd64.tar.gz sha256:bb766f710eef8ede859c18578c72c327597cd4c8a85b06001b1f3843c6019386"
NOTICES=opt/gh/LICENSE

package_stage()
{
    tarball=$(fetched "gh_${VERSION}_linux_amd64.tar.gz")
    mkdir -p "$1/opt/gh" "$1/usr/bin"
    tar -xzf "$tarball" -C "$1/opt/gh" --strip-components=1 --no-same-owner
    ln -s ../../opt/gh/bin/gh "$1/usr/bin/gh"
}

# gh writes a device identifier under its state directory when it starts,
# so it runs with a home of its own.
package_check()
{
    out=$(HOME="$SCRATCH" XDG_CONFIG_HOME="$SCRATCH/config" XDG_STATE_HOME="$SCRATCH/state" \
        XDG_DATA_HOME="$SCRATCH/data" XDG_CACHE_HOME="$SCRATCH/cache" \
        "$1/opt/gh/bin/gh" --version | sed -n 1p)
    case $out in
        "gh version $VERSION "*) ;;
        *) echo "gh: --version printed [$out], not version $VERSION" >&2; return 1 ;;
    esac
}
