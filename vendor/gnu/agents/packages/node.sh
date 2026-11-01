# shellcheck shell=sh disable=SC2034 # fetch_agents.sh reads the variables
# node, Node.js, the JavaScript runtime with npm and npx; the project's
# x86-64 Linux LTS build, pinned by its release's SHASUMS256.txt. The archive
# keeps Node's MIT license with its bundled notices and npm's Artistic-2.0
# license and dependency notices. Its bin directory keeps npm and npx's
# relative links into lib/node_modules, under /opt/node as published.
VERSION=26.13.0
LICENSE="MIT AND Artistic-2.0 AND bundled licenses"
TARBALL=node-v$VERSION-linux-x64.tar.xz
FILES="$TARBALL https://nodejs.org/dist/v$VERSION/$TARBALL sha256:d28a96af6c38596f6d9d97c0ee531c13d238d757742387d1928de822d959e997"
NOTICES="opt/node/LICENSE opt/node/lib/node_modules/npm/LICENSE"

package_stage()
{
    mkdir -p "$1/opt/node" "$1/usr/bin"
    tar -xJf "$(fetched "$TARBALL")" -C "$1/opt/node" --strip-components=1 --no-same-owner
    for program in node npm npx; do
        ln -s ../../opt/node/bin/$program "$1/usr/bin/$program"
    done
}

# npm and npx use this package's node and write only in their scratch home.
package_check()
{
    out=$("$1/opt/node/bin/node" --version)
    if [ "$out" != "v$VERSION" ]; then
        echo "node: --version printed [$out], expected v$VERSION" >&2
        return 1
    fi
    for program in npm npx; do
        out=$(HOME="$SCRATCH" TMPDIR="$SCRATCH" npm_config_cache="$SCRATCH/cache" \
            "$1/opt/node/bin/node" "$1/opt/node/lib/node_modules/npm/bin/$program-cli.js" --version)
        if [ "$out" != 11.20.0 ]; then
            echo "node: $program --version printed [$out], expected 11.20.0" >&2
            return 1
        fi
    done
}
