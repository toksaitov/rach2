# shellcheck shell=sh disable=SC2034 # fetch_agents.sh reads the variables
# opencode, OpenCode, a coding agent for the terminal; one executable, the
# project's linux-x64-baseline build, a program for Bun that links against
# glibc and needs no AVX2. The project signs no file of the release: the pin
# is the SHA-256 that the release's SHA256SUMS lists for the tarball. The
# license is LICENSE of the repository at the release's tag, which the
# tarball does not have, pinned by its SHA-256. The program is installed
# unmodified as /opt/opencode/bin/opencode, with the license beside it and a
# link in /usr/bin, and /etc/profile.d/opencode.sh sets
# OPENCODE_DISABLE_AUTOUPDATE, which turns off its update check.
VERSION=2.2.5
LICENSE=MIT
TAG=v$VERSION
TARBALL=opencode-linux-x64-baseline.tar.gz
FILES="opencode-$VERSION-linux-x64-baseline.tar.gz https://github.com/anomalyco/opencode/releases/download/$TAG/$TARBALL sha256:09f44302c10fd036912f59c81fd1aef5df45dd06f649197f9dfc9bbe2a79614e
opencode-$VERSION-LICENSE https://raw.githubusercontent.com/anomalyco/opencode/$TAG/LICENSE sha256:625f0f619133f89bbbb2abe37369613dfa1885eba1e50d02170deb62bb42cb6b"
NOTICES=opt/opencode/LICENSE

package_stage()
{
    tarball=$(fetched "opencode-$VERSION-linux-x64-baseline.tar.gz")
    mkdir -p "$1/opt/opencode/bin" "$1/usr/bin" "$1/etc/profile.d"
    tar -xzf "$tarball" -C "$1/opt/opencode/bin" --no-same-owner opencode
    chmod 0755 "$1/opt/opencode/bin/opencode"
    cp "$(fetched "opencode-$VERSION-LICENSE")" "$1/opt/opencode/LICENSE"
    chmod 0644 "$1/opt/opencode/LICENSE"
    ln -s ../../opt/opencode/bin/opencode "$1/usr/bin/opencode"
    cat > "$1/etc/profile.d/opencode.sh" <<'EOF'
# OpenCode runs the version the root installed: its update check is off.
export OPENCODE_DISABLE_AUTOUPDATE=1
EOF
}

# OpenCode writes its log under its home and unpacks a library into TMPDIR
# at every start, so it runs with a home and a TMPDIR of its own. The
# program is Bun's, whose JavaScriptCore aborts at its start under QEMU's
# user-mode emulator, so a build host that is not x86, as etude's tooling
# machine on a Mac is not, does not run it; the agents suite runs it on Rach2.
package_check()
{
    case "$(uname -m)" in
        x86_64 | i?86) ;;
        *)
            echo "opencode: --version is not run on this $(uname -m) host, whose emulator of x86-64 cannot run Bun"
            return 0
            ;;
    esac
    out=$(HOME="$SCRATCH" TMPDIR="$SCRATCH" XDG_CONFIG_HOME="$SCRATCH/config" \
        XDG_DATA_HOME="$SCRATCH/data" XDG_STATE_HOME="$SCRATCH/state" XDG_CACHE_HOME="$SCRATCH/cache" \
        OPENCODE_DISABLE_AUTOUPDATE=1 "$1/opt/opencode/bin/opencode" --version)
    if [ "$out" != "opencode v$VERSION" ]; then
        echo "opencode: --version printed [$out], not opencode v$VERSION" >&2
        return 1
    fi
}
