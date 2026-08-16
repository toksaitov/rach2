#!/usr/bin/env sh

#
# gen_demo_password.sh: ensure the demo account passwords and their /etc/shadow
# exist for the os-real image's logins.
#
# Mints a random password per demo account, writes each plaintext to a
# git-ignored host-side credential file (rach2_demo_password for root,
# rach2_test_password for the test account), and hashes both into krffs/shadow
# (git-ignored), which the os-real manifest stages as /etc/shadow. The hashes
# are SHA-512 crypt ($6$), minted by build/krffs/mkhash.krffs, the vendored
# musl implementation, so the guest verifies exactly what this script mints.
# Everything is generated only when krffs/shadow is absent, so present
# credentials are left as is (idempotent); delete krffs/shadow to mint fresh
# ones.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

MKHASH=../build/krffs/mkhash.krffs

if [ -f shadow ]; then
    exit 0
fi

if [ ! -x "$MKHASH" ]; then
    echo "gen_demo_password: no $MKHASH; build the krffs tools first." >&2
    exit 1
fi

# One generation at a time: concurrent image targets under make -j must not
# interleave their writes. mkdir is the portable atomic test-and-set; a holder
# that minted shadow while we waited satisfies this run too.
until mkdir .gen_demo_password.lock 2>/dev/null; do
    sleep 1
done
trap 'rmdir .gen_demo_password.lock' EXIT
if [ -f shadow ]; then
    exit 0
fi

# The demo passwords. These are development credentials for a teaching system:
# the same on every build, written in plaintext to a git-ignored file, and typed
# at a console by whoever is sitting at the machine. Short and memorable beats
# random here, because the one place they are actually used is a person typing
# them at a laptop across the room; every automated caller reads the file.
#
# Set RACH2_ROOT_PASSWORD or RACH2_TEST_PASSWORD to override, and use a random
# token for either if this machine is somewhere a demo account matters:
#
#     RACH2_ROOT_PASSWORD="$(LC_ALL=C tr -dc 'A-Za-z0-9' < /dev/urandom | head -c 20)"
#
# The salts stay random regardless, since nobody types a salt.
DEFAULT_ROOT_PASSWORD="1qA2wS3eD"
DEFAULT_TEST_PASSWORD="4rF5tG6yH"

# A random token from the urandom pool: $1 characters of the salt/password
# alphabet. LC_ALL=C keeps tr byte-oriented whatever the host locale is.
random_token() {
    LC_ALL=C tr -dc 'A-Za-z0-9./' < /dev/urandom | dd bs=1 "count=$1" 2>/dev/null
}

# Mint account $1's password $3 into credential file $2 and append the shadow
# line to shadow.tmp. The password never reaches mkhash's argument list: it is
# read from stdin, so it stays out of the process table. The credential file is removed first so it is recreated
# fresh under the 077 umask, never rewritten through a stale loose mode.
mint() {
    user="$1"
    file="$2"
    password="$3"

    salt="$(random_token 16)"
    hash="$(printf '%s\n' "$password" | "$MKHASH" "\$6\$$salt")"

    umask 077
    rm -f "$file"
    printf '%s\n' "$password" > "$file"
    printf '%s:%s:0:0:99999:7:::\n' "$user" "$hash" >> shadow.tmp
    echo "gen_demo_password: wrote krffs/$file"
}

# Stage the shadow file atomically: both lines into a temp file renamed into
# place, so an interrupted run never leaves a partial shadow a later run trusts.
rm -f shadow.tmp
mint root rach2_demo_password "${RACH2_ROOT_PASSWORD:-$DEFAULT_ROOT_PASSWORD}"
mint test rach2_test_password "${RACH2_TEST_PASSWORD:-$DEFAULT_TEST_PASSWORD}"
mv shadow.tmp shadow
echo "gen_demo_password: wrote krffs/shadow"
echo "gen_demo_password: ssh -p 2222 test@localhost takes krffs/rach2_test_password"
