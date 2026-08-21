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

# Already minted, and nobody asked for them to be replaced: the common case, and
# the fast one, since every image target calls this.
if [ -f shadow ] && [ -z "${RACH2_REPLACE_PASSWORDS:-}" ]; then
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

# The credentials are minted once and then left alone: every build calls this,
# and a script that reminted on each call would change the machine's passwords
# under whoever was using them. Replacing them is therefore something to ask for
# by name, which is what the sealing session does when it gives a machine
# passwords of its own.
if [ -f shadow ] && [ -z "${RACH2_REPLACE_PASSWORDS:-}" ]; then
    exit 0
fi
if [ -f shadow ]; then
    # The old hashes go before the new plaintext arrives, and not after. The
    # publication is three files and one rename, so an interruption in the
    # middle of it leaves something; what it must not leave is new passwords in
    # the plaintext files beside the old hashes in `shadow`, because the next
    # ordinary build takes `shadow` being there as the whole answer and would
    # ship credentials nobody has. With it removed first, an interruption leaves
    # no `shadow` at all, and the next build mints a complete set.
    echo "gen_demo_password: replacing the credentials already in krffs/"
    rm -f shadow
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
# RACH2_ROOT_PASSWORD="$(LC_ALL=C tr -dc 'A-Za-z0-9' < /dev/urandom | head -c
# 20)"
#
# Those take effect on a tree that has no credentials yet. To replace ones that
# are already there, which is what a machine going onto a real network needs,
# set RACH2_REPLACE_PASSWORDS=1 as well; without it this script leaves an
# existing krffs/shadow alone whatever else is set.
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
# read from stdin, so it stays out of the process table. The credential file is
# removed first so it is recreated fresh under the 077 umask, never rewritten
# through a stale loose mode.
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
