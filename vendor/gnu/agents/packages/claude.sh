# shellcheck shell=sh disable=SC2034 # fetch_agents.sh reads the variables
# claude, Anthropic's Claude Code, a coding agent for the terminal; one
# executable, Anthropic's linux-x64 build, which links against glibc.
# Anthropic signs each release's manifest.json, which lists every platform's
# binary with its SHA-256, with its release key. The pins are the binary's
# SHA-256 as the manifest lists it, the digests of the manifest, its
# signature and the key, and the key's fingerprint, FINGERPRINT;
# package_verify checks the signature with gpgv and the binary against the
# manifest, as https://code.claude.com/docs/en/setup describes. The license
# is LICENSE.md of the npm package of the same version, pinned by the
# SHA-512 the npm registry lists. The binary is installed unmodified as
# /opt/claude/bin/claude, with the license beside it and a link in
# /usr/bin, and the managed settings in /etc/claude-code turn its updates
# and its sandbox off.
VERSION=2.1.296
LICENSE=Proprietary
FINGERPRINT=31DDDE24DDFAB679F42D7BD2BAA929FF1A7ECACE
RELEASE=https://downloads.claude.ai/claude-code-releases/$VERSION
FILES="claude-$VERSION $RELEASE/linux-x64/claude sha256:24972e3bc859fab2b46ed4c1e51f7d6130f06d3bd550811a114640de3370d0de
manifest-$VERSION.json $RELEASE/manifest.json sha256:e50e872d91f3b7da2fb3d8d38505d709e0a90f48da1951dd51176930c8da8270
manifest-$VERSION.json.sig $RELEASE/manifest.json.sig sha256:ab95ac645a820036734e38a6fc21d41f0535412722a463d91e0b6077082e29c9
claude-code.asc https://downloads.claude.ai/keys/claude-code.asc sha256:bd70a5e4a268002704024ceba7f8446024114e94f3f0bdd11c23a9e592be81c6
claude-code-$VERSION.tgz https://registry.npmjs.org/@anthropic-ai/claude-code/-/claude-code-$VERSION.tgz sha512:397fe4feba5cb6132a9c528e59ca4d71b88bb0c2ad0232cea503491e56220c2154ce75822307514f297fa7d49d1d5aecaf366e3cc08da998e7a5a9089e2eaea7"
NOTICES=opt/claude/LICENSE.md

# The key is published armored; gpgv reads a binary keyring, which is the
# armor's base64 body without its checksum line. gpgv's VALIDSIG status line
# ends with the fingerprint of the primary key that made the signature.
package_verify()
{
    manifest=$(fetched "manifest-$VERSION.json")
    sig=$(fetched "manifest-$VERSION.json.sig")
    key=$(fetched claude-code.asc)
    binary=$(fetched "claude-$VERSION")
    sed -e '1,/^$/d' -e '/^=/,$d' "$key" | base64 -d > "$SCRATCH/claude-code.gpg"
    signer=$(GNUPGHOME="$SCRATCH" gpgv --status-fd 1 --keyring "$SCRATCH/claude-code.gpg" \
        "$sig" "$manifest" 2> /dev/null | awk '$2 == "VALIDSIG" { print $NF }')
    if [ "$signer" != "$FINGERPRINT" ]; then
        echo "gnu: agents: claude: refused: manifest.json is not signed by key $FINGERPRINT" >&2
        return 1
    fi
    listed=$(awk '/"linux-x64":/ { p = 1 } p && /"checksum":/ { gsub(/[",]/, "", $2); print $2; exit }' "$manifest")
    got=$(digest_of sha256 "$binary") || return 1
    if [ -z "$listed" ] || [ "$got" != "$listed" ]; then
        echo "gnu: agents: claude: refused: the binary's sha256 $got is not the manifest's [$listed]" >&2
        return 1
    fi
}

package_stage()
{
    binary=$(fetched "claude-$VERSION")
    npm=$(fetched "claude-code-$VERSION.tgz")
    mkdir -p "$1/opt/claude/bin" "$1/usr/bin" "$1/etc/claude-code"
    cp "$binary" "$1/opt/claude/bin/claude"
    chmod 0755 "$1/opt/claude/bin/claude"
    tar -xzf "$npm" -C "$1/opt/claude" --strip-components=1 --no-same-owner package/LICENSE.md
    ln -s ../../opt/claude/bin/claude "$1/usr/bin/claude"
    cat > "$1/etc/claude-code/managed-settings.json" <<'EOF'
{
  "env": {
    "DISABLE_UPDATES": "1"
  },
  "sandbox": {
    "enabled": false
  }
}
EOF
}

# claude reads its settings from its home, so it runs with one of its own.
package_check()
{
    out=$(HOME="$SCRATCH" "$1/opt/claude/bin/claude" --version)
    case $out in
        "$VERSION (Claude Code)") ;;
        *) echo "claude: --version printed [$out], not $VERSION (Claude Code)" >&2; return 1 ;;
    esac
}
