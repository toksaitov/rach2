# shellcheck shell=sh disable=SC2034 # fetch_agents.sh reads the variables
# codex, OpenAI's Codex, a coding agent for the terminal; a static Rust
# program with its helpers, OpenAI's build. The release's package for
# x86_64-unknown-linux-musl has every file of the npm package
# @openai/codex-linux-x64 in the same layout: bin/codex,
# bin/codex-code-mode-host, codex-path/rg (ripgrep), codex-resources/bwrap
# (bubblewrap), codex-resources/zsh, the voice host with its libraries and
# their licenses, and codex-package.json. The pin is the SHA-256 that the
# release's codex-package_SHA256SUMS lists, and package_verify checks the
# release's Sigstore signature of the package with Sigstore's cosign, itself
# pinned by the SHA-256 its own release lists. The package is unpacked as
# published into /opt/codex, with OpenAI's license and notice and the
# licenses of ripgrep, bubblewrap and zsh, which the package does not have,
# taken from the sources that the tag's build scripts name. /usr/bin/codex
# links to the program, and /etc/codex/config.toml turns off the update
# check and the background server. The sandbox is left as shipped, the
# bundled bubblewrap.
VERSION=0.162.1
LICENSE=Apache-2.0
COSIGN_VERSION=3.1.3
TAG=rust-v$VERSION
RELEASE=https://github.com/openai/codex/releases/download/$TAG
SOURCE=https://raw.githubusercontent.com/openai/codex/$TAG
RIPGREP=https://raw.githubusercontent.com/BurntSushi/ripgrep/15.2.0
ZSH=https://raw.githubusercontent.com/zsh-users/zsh/77045ef899e53b9598bebc5a41db93a548a40ca6
PACKAGE=codex-package-x86_64-unknown-linux-musl.tar.gz
FILES="$PACKAGE $RELEASE/$PACKAGE sha256:a676f5722aae0d86cbe3764332f08eab1fdd89dc0a300b3ad4ffbf48611fa3e3
$PACKAGE.sigstore $RELEASE/$PACKAGE.sigstore sha256:909ac7a797793d838aea224d4fe4752bd9b48b64166da601b5c014c31ca7d9cb
cosign-$COSIGN_VERSION https://github.com/sigstore/cosign/releases/download/v$COSIGN_VERSION/cosign-linux-amd64 sha256:4629c757b7618056f8ddd7e2625ae9fdd94c0372a65049520bc7d9df9efc7f71
codex-LICENSE $SOURCE/LICENSE sha256:d17f227e4df5da1600391338865ce0f3055211760a36688f816941d58232d8dc
codex-NOTICE $SOURCE/NOTICE sha256:9d71575ecfd9a843fc1677b0efb08053c6ba9fd686a0de1a6f5382fd3c220915
bubblewrap-COPYING $SOURCE/codex-rs/vendor/bubblewrap/COPYING sha256:b7993225104d90ddd8024fd838faf300bea5e83d91203eab98e29512acebd69c
ripgrep-COPYING $RIPGREP/COPYING sha256:01c266bced4a434da0051174d6bee16a4c82cf634e2679b6155d40d75012390f
ripgrep-LICENSE-MIT $RIPGREP/LICENSE-MIT sha256:0f96a83840e146e43c0ec96a22ec1f392e0680e6c1226e6f3ba87e0740af850f
ripgrep-UNLICENSE $RIPGREP/UNLICENSE sha256:7e12e5df4bae12cb21581ba157ced20e1986a0508dd10d0e8a4ab9a4cf94e85c
zsh-LICENCE $ZSH/LICENCE sha256:d06fdf3ef9b1ec69d6b9e170b0a9516fbad3523261ff1668bde3bfea6e0ef5f5"
NOTICES="opt/codex/LICENSE opt/codex/NOTICE opt/codex/licenses/bubblewrap/COPYING
opt/codex/licenses/ripgrep/COPYING opt/codex/licenses/ripgrep/LICENSE-MIT
opt/codex/licenses/ripgrep/UNLICENSE opt/codex/licenses/zsh/LICENCE
opt/codex/codex-resources/voice/NOTICE.md"

# The signature is good only from the release workflow of the tag, run by
# GitHub Actions. cosign fetches Sigstore's trusted root into its home.
package_verify()
{
    cosign=$(fetched "cosign-$COSIGN_VERSION")
    chmod +x "$cosign"
    mkdir -p "$SCRATCH/cosign"
    HOME="$SCRATCH/cosign" "$cosign" verify-blob \
        --bundle "$(fetched "$PACKAGE.sigstore")" \
        --certificate-identity "https://github.com/openai/codex/.github/workflows/rust-release.yml@refs/tags/$TAG" \
        --certificate-oidc-issuer https://token.actions.githubusercontent.com \
        "$(fetched "$PACKAGE")"
}

package_stage()
{
    opt="$1/opt/codex"
    mkdir -p "$opt/licenses/bubblewrap" "$opt/licenses/ripgrep" "$opt/licenses/zsh" \
        "$1/usr/bin" "$1/etc/codex"
    tar -xzf "$(fetched "$PACKAGE")" -C "$opt" --no-same-owner
    cp "$(fetched codex-LICENSE)" "$opt/LICENSE"
    cp "$(fetched codex-NOTICE)" "$opt/NOTICE"
    cp "$(fetched bubblewrap-COPYING)" "$opt/licenses/bubblewrap/COPYING"
    for f in COPYING LICENSE-MIT UNLICENSE; do
        cp "$(fetched "ripgrep-$f")" "$opt/licenses/ripgrep/$f"
    done
    cp "$(fetched zsh-LICENCE)" "$opt/licenses/zsh/LICENCE"
    chmod 0644 "$opt/LICENSE" "$opt/NOTICE" "$opt"/licenses/*/*
    ln -s ../../opt/codex/bin/codex "$1/usr/bin/codex"
    cat > "$1/etc/codex/config.toml" <<'TOML'
# /etc/codex/config.toml: Rach2's settings for Codex, which a person's
# ~/.codex/config.toml overrides; codex(1) says why each is set.
check_for_update_on_startup = false

[features]
daemon_auto_start = false
TOML
    chmod 0644 "$1/etc/codex/config.toml"
}

# Codex writes its state under its home, ~/.codex, when it starts, so it
# runs with a home of its own, and warns that it makes no links to its
# helpers there, since the home is under the temporary directory.
package_check()
{
    out=$(HOME="$SCRATCH" "$1/opt/codex/bin/codex" --version 2> /dev/null)
    if [ "$out" != "codex-cli $VERSION" ]; then
        echo "codex: --version printed [$out], not codex-cli $VERSION" >&2
        return 1
    fi
}
