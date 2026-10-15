#!/usr/bin/env sh

#
# gen_demo_password.sh: make the passwords of the images' root and test
# accounts, and krffs/shadow, which the manifests stage as /etc/shadow.
#
# Each password is written in plain text to a file git ignores,
# rach2_demo_password for root and rach2_test_password for the test account,
# and its SHA-512 crypt hash ($6$) to krffs/shadow, also ignored. The hashes
# are made by build/krffs/mkhash.krffs, which is musl's implementation, so
# the guest checks the hash this script makes. Nothing is made while
# krffs/shadow exists, unless RACH2_REPLACE_PASSWORDS=1 is set. The aging
# fields of each line are empty, as adduser(8) writes them: a last change of
# day 0 tells a reader of shadow(5) that root forced a change at the next
# login, which OpenSSH's server enforces. A shadow file with day 0 in a line
# keeps its hashes and has those fields emptied. A shadow file older than
# this script, for which make runs it, is touched once it is up to date, so
# that make does not run it again.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

MKHASH=../build/krffs/mkhash.krffs

# Whether a line of krffs/shadow gives its last change as day 0.
day_zero() {
    awk -F: '$3 == "0" { found = 1 } END { exit !found }' shadow
}

# Whether this script is newer than krffs/shadow.
older() {
    [ -n "$(find gen_demo_password.sh -newer shadow)" ]
}

# Made already, up to date, and not to be replaced: the common case, and the
# fast one.
if [ -f shadow ] && [ -z "${RACH2_REPLACE_PASSWORDS:-}" ] && ! day_zero && ! older; then
    exit 0
fi

# One run at a time: image targets under make -j must not interleave their
# writes. mkdir is the portable atomic test-and-set; when another run made
# shadow while this one waited, the test below finds it and stops.
until mkdir .gen_demo_password.lock 2>/dev/null; do
    sleep 1
done
trap 'rmdir .gen_demo_password.lock' EXIT

# The credentials are made once and then left alone: every build runs this,
# and a script that made new ones on each run would change the machine's
# passwords under whoever was using them. Replacing them is asked for by
# name, with RACH2_REPLACE_PASSWORDS=1, as for a machine given passwords of
# its own.
if [ -f shadow ] && [ -z "${RACH2_REPLACE_PASSWORDS:-}" ]; then
    if day_zero; then
        umask 077
        rm -f shadow.tmp
        awk -F: -v OFS=: '$3 == "0" { $3 = $4 = $5 = $6 = $7 = $8 = $9 = "" } { print }' \
            shadow > shadow.tmp
        mv shadow.tmp shadow
        echo "gen_demo_password: emptied the aging fields of krffs/shadow"
    fi
    touch shadow
    exit 0
fi

if [ ! -x "$MKHASH" ]; then
    echo "gen_demo_password: no $MKHASH; build the krffs tools first." >&2
    exit 1
fi
if [ -f shadow ]; then
    # The old hashes go before the new passwords are written, and not after.
    # The writing is three files and one rename, so an interruption part way
    # leaves something; it must not leave new passwords in the plain text
    # files beside the old hashes in `shadow`, since the next build takes an
    # existing `shadow` as complete and would stage hashes of passwords
    # nobody has. With it removed first, an interruption leaves no `shadow`,
    # and the next build makes a complete set.
    echo "gen_demo_password: replacing the credentials already in krffs/"
    rm -f shadow
fi

# The development passwords of a teaching system: the same on every build,
# written in plain text to a file git ignores, and typed at the console by a
# person at the machine. They are short and easy to type for that person;
# every automated caller reads the file.
#
# RACH2_ROOT_PASSWORD and RACH2_TEST_PASSWORD give others, and a random token
# suits a machine where a demo account matters, such as the output of
#
#   LC_ALL=C tr -dc 'A-Za-z0-9' < /dev/urandom | head -c 20
#
# They take effect in a tree that has no credentials yet. To replace those a
# tree has, which a machine for any other network needs, set
# RACH2_REPLACE_PASSWORDS=1 as well; without it this script leaves an
# existing krffs/shadow alone whatever else is set.
#
# The salts are random in every case.
DEFAULT_ROOT_PASSWORD="1qA2wS3eD"
DEFAULT_TEST_PASSWORD="4rF5tG6yH"

# random_token N: N random characters of the alphabet of a salt. LC_ALL=C
# keeps tr working on bytes whatever the host's locale is.
random_token() {
    LC_ALL=C tr -dc 'A-Za-z0-9./' < /dev/urandom | dd bs=1 "count=$1" 2>/dev/null
}

# mint USER FILE PASSWORD: write PASSWORD to FILE and append USER's line to
# shadow.tmp. mkhash reads the password on its standard input, not from its
# arguments, so it does not appear in the process table. FILE is removed
# first, so that it is made again under the 077 umask rather than written
# through a looser mode it had.
mint() {
    user="$1"
    file="$2"
    password="$3"

    salt="$(random_token 16)"
    hash="$(printf '%s\n' "$password" | "$MKHASH" "\$6\$$salt")"

    umask 077
    rm -f "$file"
    printf '%s\n' "$password" > "$file"
    printf '%s:%s:::::::\n' "$user" "$hash" >> shadow.tmp
    echo "gen_demo_password: wrote krffs/$file"
}

# Both lines go to a temporary file that is renamed into place, so an
# interrupted run leaves no partial shadow that a later run would accept.
rm -f shadow.tmp
mint root rach2_demo_password "${RACH2_ROOT_PASSWORD:-$DEFAULT_ROOT_PASSWORD}"
mint test rach2_test_password "${RACH2_TEST_PASSWORD:-$DEFAULT_TEST_PASSWORD}"
mv shadow.tmp shadow
echo "gen_demo_password: wrote krffs/shadow"
echo "gen_demo_password: ssh -p 2222 test@localhost takes krffs/rach2_test_password"
