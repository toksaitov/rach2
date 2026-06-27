#!/usr/bin/env sh

#
# gen_demo_key.sh: ensure a demo SSH keypair exists for the os-real image's
# dropbear login.
#
# Generates krffs/rach2_demo_ed25519 (the private half, git-ignored, a host-side
# test credential) and krffs/authorized_keys (the public half, which the os-real
# manifest stages into /root/.ssh) only when authorized_keys is absent. Log in with
# `ssh -i krffs/rach2_demo_ed25519 -p 2222 root@localhost`. Idempotent: a present
# authorized_keys is left as is.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

KEY="rach2_demo_ed25519"

if [ -f authorized_keys ]; then
    exit 0
fi
if ! command -v ssh-keygen >/dev/null 2>&1; then
    echo "gen_demo_key: no ssh-keygen; the os-real SSH login will lack a key" >&2
    exit 0
fi

rm -f "$KEY" "$KEY.pub"
ssh-keygen -t ed25519 -N "" -C "rach2-demo" -f "$KEY" >/dev/null
cp "$KEY.pub" authorized_keys
echo "gen_demo_key: wrote krffs/$KEY"
echo "gen_demo_key: log in with ssh -i krffs/$KEY -p 2222 root@localhost"
