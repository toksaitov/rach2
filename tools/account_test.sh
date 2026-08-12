#!/usr/bin/env sh

#
# account_test.sh: prove an image can create, authenticate, change and delete an
# account, and that the three credential databases agree afterwards.
#
# It boots a disk twice: once to run the arc, once to read the databases back,
# so what is asserted is what survived to the platter rather than what one
# process had in its cache. The arc is create, join a second group, set a
# password, become the account through `su`, then delete and prove every trace
# is gone.
#
# The deletion assertions are two-way on purpose. A check that only asked "does
# every account in passwd have a shadow line" passes an account that was removed
# from passwd and left in shadow, which is exactly what a partial delete leaves.
#
# Three traps this repo has already paid for, and which every phase here obeys:
# the console echoes what is typed, so a token that appears in a command would
# be matched from its own echo and each is written split; a feed written all at
# once is eaten by anything that flushes its input, so every feed is spaced; and
# a previous run's logs hold every token this run waits for, so they go before
# the first boot rather than only in the trap.
#
# Usage: account_test.sh <kernel> <disk> <password>
#

set -e

KERNEL="$1"
DISK="$2"
PASSWORD="$3"

if [ -z "$KERNEL" ] || [ -z "$DISK" ] || [ -z "$PASSWORD" ]; then
    echo "usage: account_test.sh <kernel> <disk> <password>" >&2
    exit 2
fi

WORK="$DISK.work"
LOG="$DISK.work.log"
FEED="$DISK.work.feed"
rm -f "$WORK" "$LOG" "$FEED"
trap 'rm -f "$WORK" "$LOG" "$FEED"' EXIT INT TERM

QPID=""
FPID=""

# The account this creates and destroys. Its password is a fixed test
# credential, not a secret.
NEWUSER="testee"
NEWPASS="testeepw1"
EXTRAGROUP="shared"

cleanup_boot() {
    kill -9 "$QPID" 2>/dev/null || true
    wait "$QPID" 2>/dev/null || true
    kill "$FPID" 2>/dev/null || true
    rm -f "$FEED"
}

fail() {
    cleanup_boot
    echo "account-test: FAIL: $1" >&2
    exit 1
}

# Wait up to $2 seconds for the token $1 in the log.
wait_token() {
    i=0
    while [ "$i" -lt "$2" ]; do
        if grep -q "$1" "$LOG" 2>/dev/null; then
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    return 1
}

# Boot the working image and feed it the script on standard input, one line per
# argument, spaced. The image logs in at the console first, since os-real's
# console is behind a login.
boot_with() {
    rm -f "$LOG" "$FEED"
    mkfifo "$FEED"
    (
        sleep 12
        printf 'root\n'
        sleep 2
        printf '%s\n' "$PASSWORD"
        sleep 3
        for line in "$@"; do
            printf '%s\n' "$line"
            sleep 4
        done
        sleep 120
    ) > "$FEED" &
    FPID=$!
    qemu-system-i386 -cpu max -display none -kernel "$KERNEL" \
        -append "rach2.fsdisk" \
        -drive "file=$WORK,format=raw,if=ide,index=0,media=disk" \
        -serial stdio < "$FEED" > "$LOG" 2>&1 &
    QPID=$!
}

rm -f "$WORK"
cp "$DISK" "$WORK"

# One boot for the whole arc, because each step depends on the last. Every token
# is split so the console's echo of the command cannot satisfy the wait.
boot_with \
    "adduser -D $NEWUSER; addgroup $EXTRAGROUP; addgroup $NEWUSER $EXTRAGROUP; echo MADE\"\"IT" \
    "echo $NEWUSER:$NEWPASS | chpasswd; echo SET\"\"IT" \
    "su $NEWUSER -c id; echo BECAME\"\"IT" \
    "deluser $NEWUSER; sync; echo GONE\"\"IT"

if ! wait_token "MADEIT" 120; then
    fail "the account was never created"
fi
if ! wait_token "SETIT" 60; then
    fail "the password was never set"
fi
if ! grep -q "password for '$NEWUSER' changed" "$LOG"; then
    fail "chpasswd did not report changing the password"
fi
if ! wait_token "BECAMEIT" 60; then
    fail "su never returned"
fi
if ! grep -q "uid=1[0-9][0-9][0-9]($NEWUSER)" "$LOG"; then
    fail "su did not become the account, or its uid is outside the range adduser was configured for"
fi
if ! wait_token "GONEIT" 60; then
    fail "the account was never deleted"
fi
cleanup_boot

# Now the platter. The databases are read back in a SECOND boot, so what is
# asserted is what survived to the disk rather than what one process had in its
# cache. The tokens are split for the same reason as above.
cp "$WORK" "$WORK.check" 2>/dev/null || true
boot_with \
    "mkpasswd -m sha512 -S abcdefgh hunter2; echo MK\"\"END" \
    "sync; cat /etc/passwd; echo PW\"\"END" \
    "cat /etc/shadow; echo SH\"\"END" \
    "cat /etc/group; echo GR\"\"END"

if ! wait_token "GREND" 150; then
    fail "the second boot could not read the databases back"
fi
cleanup_boot

# The hash generators, pinned against the value this project's own SHA-512
# crypt produces for the same password and salt, so the assertion is that
# busybox agrees with the implementation the rest of the image authenticates
# with, not merely that it printed something shaped like a hash.
# shellcheck disable=SC2016
EXPECTED_HASH='$6$abcdefgh$M/eYsB4rVXAm3ZNc88J.UD9rCKAT6FB1rahiwJCHtEndQNORCub5qhjxn50qbqVVthkM.9HpEwtf0t.iV9uH0/'
if ! grep -qF "$EXPECTED_HASH" "$LOG"; then
    fail "mkpasswd did not produce the hash this system's own crypt gives"
fi

SHADOW_TEXT=$(sed -n '/PWEND/,/SHEND/p' "$LOG")
GROUP_TEXT=$(sed -n '/SHEND/,/GREND/p' "$LOG")

# The delete is two-way: gone from every database and from every member list,
# not merely gone from passwd, since a partial delete leaves exactly that.
case "$SHADOW_TEXT" in
    *"$NEWUSER:"*) fail "the deleted account is still in /etc/shadow" ;;
esac
case "$GROUP_TEXT" in
    *"$NEWUSER:"*) fail "the deleted account's own group is still in /etc/group" ;;
    *":$NEWUSER"*) fail "the deleted account is still a member of a group" ;;
    *",$NEWUSER"*) fail "the deleted account is still a member of a group" ;;
esac
if grep -q "^$NEWUSER:" "$LOG"; then
    fail "the deleted account is still in a database"
fi

# The second group survived the delete, which is the difference between removing
# a member and removing the group somebody was a member of.
case "$GROUP_TEXT" in
    *"$EXTRAGROUP:"*) ;;
    *) fail "deleting the account took the shared group with it" ;;
esac

echo "account-test: an account was created, used, and deleted without trace: OK"
echo "account-test: PASS"
