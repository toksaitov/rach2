# shellcheck shell=sh disable=SC2034 # fetch_agents.sh reads the variables
# agy, Google's Antigravity CLI, a coding agent for the terminal; a
# dynamically linked Go program for glibc, Google's own build. Google's
# installer reads manifests/linux_amd64.json from its download host, which
# gives the version, the tarball's URL and its SHA-512, and installs the one
# file of the tarball, antigravity, as agy. The pin is that URL and that
# SHA-512. The program is copied unmodified into /opt/agy/bin/agy, and
# /usr/bin/agy links to it. Google publishes no license file: the program is
# used under the Google Terms of Service, the Google Antigravity Additional
# Terms of Service and the Google Privacy Policy, which the stage's
# /opt/agy/TERMS names. /etc/profile.d/agy.sh sets AGY_CLI_DISABLE_AUTO_UPDATE,
# the setting Google documents to turn off the program's updater.
VERSION=1.4.0
LICENSE="Google Antigravity Terms of Service"
FILES="cli_linux_x64.tar.gz https://storage.googleapis.com/antigravity-public/antigravity-cli/$VERSION-5281842794987520/linux-x64/cli_linux_x64.tar.gz sha512:6c4b298c2a69b6bd0f58567031cc56d8097a32be54c4b633aa80ad395871b389618ab3860d4ba0a12599831b8226b51b03e258ee2ec7fae105a45b8e0fce2188"
NOTICES=opt/agy/TERMS

package_stage()
{
    tarball=$(fetched cli_linux_x64.tar.gz)
    mkdir -p "$1/opt/agy/bin" "$1/usr/bin" "$1/etc/profile.d"
    tar -xzf "$tarball" -C "$1/opt/agy/bin" --no-same-owner antigravity
    mv "$1/opt/agy/bin/antigravity" "$1/opt/agy/bin/agy"
    ln -s ../../opt/agy/bin/agy "$1/usr/bin/agy"
    cat > "$1/opt/agy/TERMS" <<EOF
Antigravity CLI $VERSION, /opt/agy/bin/agy, is Google's software, installed
unmodified from Google's build. Its use is governed by the Google Terms of
Service, the Google Antigravity Additional Terms of Service and the Google
Privacy Policy:

    https://policies.google.com/terms
    https://antigravity.google/terms
    https://policies.google.com/privacy
EOF
    cat > "$1/etc/profile.d/agy.sh" <<'EOF'
# Antigravity CLI runs the version the root installed: its updater is off.
export AGY_CLI_DISABLE_AUTO_UPDATE=true
EOF
}

# The program writes nothing for --version, but runs with a home of its own
# all the same. It needs glibc's x86-64 loader, which QEMU's user-mode
# emulator lacks on a build host that is not x86, as etude's tooling machine
# on a Mac is not, so such a host does not run it; the agents suite runs it
# on Rach2.
package_check()
{
    case "$(uname -m)" in
        x86_64 | i?86) ;;
        *)
            echo "agy: --version is not run on this $(uname -m) host, which has no x86-64 glibc loader"
            return 0
            ;;
    esac
    out=$(HOME="$SCRATCH" AGY_CLI_DISABLE_AUTO_UPDATE=true "$1/opt/agy/bin/agy" --version)
    if [ "$out" != "$VERSION" ]; then
        echo "agy: --version printed [$out], not $VERSION" >&2
        return 1
    fi
}
