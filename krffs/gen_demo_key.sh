#!/usr/bin/env sh

#
# gen_demo_key.sh: write authorized_keys and test_authorized_keys, the SSH
# keys the images' root and test accounts accept.
#
# There is a key pair for each account: rach2_demo_ed25519 for root and
# rach2_test_ed25519 for the test account. Both are committed, as the
# project's development keys, so that every clone builds images that accept
# the same keys and any development host reaches any development machine on
# the private network. Their public halves become authorized_keys and
# test_authorized_keys, which the manifests stage into each account's
# ~/.ssh. A key pair is made only when it is absent. RACH2_REPLACE_KEYS=1
# makes new ones over those there, which a machine for any other network
# needs, since the development keys are public.
#
#   ssh -i krffs/rach2_demo_ed25519 -p 2222 root@localhost
#   ssh -i krffs/rach2_test_ed25519 -p 2222 test@localhost
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# gen_key KEY PUB USER: make the key pair KEY and its public half PUB for a
# login as USER, unless the pair is present, as the committed one is.
gen_key() {
    key="$1"
    pub="$2"
    user="$3"

    if [ -f "$key" ] && [ -f "$key.pub" ] && [ -z "${RACH2_REPLACE_KEYS:-}" ]; then
        return 0
    fi
    if [ -f "$key" ]; then
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
    # The public half is copied to a temporary and renamed into place, so an
    # interrupted run leaves no partial $pub that a later run would accept.
    cp "$key.pub" "$pub.tmp"
    [ -s "$pub.tmp" ] || { echo "gen_demo_key: empty public key for $user." >&2; exit 1; }
    mv "$pub.tmp" "$pub"
    echo "gen_demo_key: wrote krffs/$key"
    echo "gen_demo_key: log in with ssh -i krffs/$key -p 2222 $user@localhost"
}

# add_local_keys KEY PUB: write PUB as the public half of KEY followed by
# krffs/PUB.extra, if it exists: the public keys of whoever works on this
# machine, so that they reach an image they built without carrying the
# project's private key between machines. The .extra file is ignored by git,
# so a clone builds images that accept the demo key alone.
#
# PUB is written whole each time, not appended to, so a rebuild gives the
# same file and deleting the .extra file takes its keys out again. Nothing
# is written when the public half of KEY is absent, the case of a public key
# put in PUB by hand.
add_local_keys() {
    key="$1"
    pub="$2"
    extra="$pub.extra"

    [ -f "$key.pub" ] || return 0

    # A private key here would be staged into the image, so every line but a
    # blank or a comment must begin with a key type, and the words PRIVATE
    # KEY, which every private key format has, must not appear.
    if [ -f "$extra" ]; then
        if grep -q 'PRIVATE KEY' "$extra"; then
            echo "gen_demo_key: krffs/$extra has a PRIVATE key; use the .pub half" >&2
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
    # PUB is replaced only when its contents change, so that it keeps its
    # time and a rebuild does not restage the images that have it.
    if [ -f "$pub" ] && cmp -s "$pub.tmp" "$pub"; then
        rm -f "$pub.tmp"
    else
        mv "$pub.tmp" "$pub"
    fi
    if [ -f "$extra" ]; then
        echo "gen_demo_key: krffs/$pub has the demo key and krffs/$extra"
    fi
}

# Git keeps no file mode but the executable bit, and ssh refuses a private key
# others can read.
for key in rach2_demo_ed25519 rach2_test_ed25519; do
    [ ! -f "$key" ] || chmod 600 "$key"
done

gen_key rach2_demo_ed25519 authorized_keys      root
gen_key rach2_test_ed25519 test_authorized_keys test

add_local_keys rach2_demo_ed25519 authorized_keys
add_local_keys rach2_test_ed25519 test_authorized_keys
