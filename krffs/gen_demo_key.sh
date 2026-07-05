#!/usr/bin/env sh

#
# gen_demo_key.sh: ensure the demo SSH keypairs exist for the os-real image's
# dropbear logins.
#
# Generates two keypairs, one per demo account: rach2_demo_ed25519 for root and
# rach2_student_ed25519 for the unprivileged student user. The private halves are
# git-ignored host-side test credentials; the public halves (authorized_keys and
# student_authorized_keys) the os-real manifest stages into each account's
# ~/.ssh. Each keypair is generated only when its public half is absent, so a
# present key is left as is (idempotent).
#
#   ssh -i krffs/rach2_demo_ed25519    -p 2222 root@localhost
#   ssh -i krffs/rach2_student_ed25519 -p 2222 student@localhost
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

    if [ -f "$pub" ]; then
        return 0
    fi
    if ! command -v ssh-keygen >/dev/null 2>&1; then
        echo "gen_demo_key: no krffs/$pub and no ssh-keygen to make one." >&2
        echo "gen_demo_key: the os-real image is the SSH capstone, so this is fatal." >&2
        echo "gen_demo_key: install openssh, or drop a public key at krffs/$pub." >&2
        exit 1
    fi
    rm -f "$key" "$key.pub"
    ssh-keygen -t ed25519 -N "" -C "rach2-$user" -f "$key" >/dev/null
    cp "$key.pub" "$pub"
    echo "gen_demo_key: wrote krffs/$key"
    echo "gen_demo_key: log in with ssh -i krffs/$key -p 2222 $user@localhost"
}

gen_key rach2_demo_ed25519    authorized_keys         root
gen_key rach2_student_ed25519 student_authorized_keys student
