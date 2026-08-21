#!/usr/bin/env sh

#
# gen_demo_key.sh: ensure the demo SSH keypairs exist for the os-real image's
# dropbear logins.
#
# Generates two keypairs, one per demo account: rach2_demo_ed25519 for root and
# rach2_test_ed25519 for the unprivileged test user. The private halves are
# git-ignored host-side test credentials; the public halves (authorized_keys and
# test_authorized_keys) the os-real manifest stages into each account's ~/.ssh.
# Each keypair is generated only when its public half is absent, so a present
# key is left as is (idempotent). Set RACH2_REPLACE_KEYS=1 to mint new ones over
# whatever is there, which is what a machine going somewhere real needs: the
# keys a development tree has been using are the keys every demo has used.
#
#   ssh -i krffs/rach2_demo_ed25519 -p 2222 root@localhost
#   ssh -i krffs/rach2_test_ed25519 -p 2222 test@localhost
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Generate keypair $1 into public-half file $2 for a login as user $3, unless $2
# is already present.
gen_key() {
    key="$1"
    pub="$2"
    user="$3"

    if [ -f "$pub" ] && [ -z "${RACH2_REPLACE_KEYS:-}" ]; then
        return 0
    fi
    if [ -f "$pub" ]; then
        echo "gen_demo_key: replacing the keypair already in krffs/"
    fi
    if ! command -v ssh-keygen >/dev/null 2>&1; then
        echo "gen_demo_key: no krffs/$pub and no ssh-keygen to make one." >&2
        echo "gen_demo_key: the os-real image is the SSH capstone, so this is fatal." >&2
        echo "gen_demo_key: install openssh, or drop a public key at krffs/$pub." >&2
        exit 1
    fi
    rm -f "$key" "$key.pub"
    ssh-keygen -t ed25519 -N "" -C "rach2-$user" -f "$key" >/dev/null
    # Stage the public half atomically: a temp copy renamed into place, so an
    # interrupted run never leaves a partial $pub that a later run trusts.
    cp "$key.pub" "$pub.tmp"
    [ -s "$pub.tmp" ] || { echo "gen_demo_key: empty public key for $user." >&2; exit 1; }
    mv "$pub.tmp" "$pub"
    echo "gen_demo_key: wrote krffs/$key"
    echo "gen_demo_key: log in with ssh -i krffs/$key -p 2222 $user@localhost"
}

# Compose public-half file $2 from generated keypair $1 plus an optional local
# addition, krffs/$2.extra: a public key belonging to whoever works on this
# machine rather than to the project, so they can reach an image they built
# without carrying the project's private key between machines. The file is
# git-ignored, so a clone builds images that trust the demo key alone.
#
# It recomposes rather than appends, which is what makes a rebuild idempotent
# and what lets the addition be removed again by deleting the file. Nothing
# happens when the generated public half is absent, which is the documented case
# of a public key dropped in by hand.
add_local_keys() {
    key="$1"
    pub="$2"
    extra="$pub.extra"

    [ -f "$key.pub" ] || return 0

    # A private key here would be staged into the image and served to anyone who
    # asks for the file, so the shape is checked rather than trusted: every
    # non-blank line must name a key type, and the word that appears in every
    # private key format must not appear at all.
    if [ -f "$extra" ]; then
        if grep -q 'PRIVATE KEY' "$extra"; then
            echo "gen_demo_key: krffs/$extra holds a PRIVATE key; use the .pub half" >&2
            exit 1
        fi
        if grep -qvE '^[[:space:]]*($|#|(ssh-(rsa|ed25519|dss)|ecdsa-sha2-|sk-)[^[:space:]]*[[:space:]])' \
               "$extra"; then
            echo "gen_demo_key: krffs/$extra has a line that is not a public key" >&2
            exit 1
        fi
    fi

    {
        cat "$key.pub"
        [ -f "$extra" ] && cat "$extra"
    } > "$pub.tmp"
    [ -s "$pub.tmp" ] || { echo "gen_demo_key: composed an empty krffs/$pub" >&2; exit 1; }
    mv "$pub.tmp" "$pub"
    if [ -f "$extra" ]; then
        echo "gen_demo_key: krffs/$pub carries the demo key and krffs/$extra"
    fi
}

gen_key rach2_demo_ed25519 authorized_keys      root
gen_key rach2_test_ed25519 test_authorized_keys test

add_local_keys rach2_demo_ed25519 authorized_keys
add_local_keys rach2_test_ed25519 test_authorized_keys
