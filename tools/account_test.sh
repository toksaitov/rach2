#!/usr/bin/env sh

#
# account_test.sh: prove an image can create, authenticate, change and delete an
# account, and that the three credential databases agree afterwards.
#
# It boots a disk three times. The first boot makes the accounts and uses them,
# the second authenticates again from a cold start and reads the databases back,
# the third looks at what is left after the delete. Splitting them that way is
# the point: what is asserted is what survived to the platter, not what one
# process had in its cache.
#
# The authentication is done from an unprivileged account and not from root.
# Root's `su` never asks for a password, so a test that only ran `su testee`
# as root would pass with a wrong hash, a broken password check, or a `passwd`
# that cannot get its set-id bit, while every real user was locked out. Here
# root becomes the account first, and the account authenticates from there: once
# with the right password, once with the wrong one, and once through `passwd`
# itself, which is the staged set-id binary and nothing else.
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

# The accounts this creates and destroys. Their passwords are fixed test
# credentials, not secrets. The second account is a system one, so the two
# together cover both id ranges adduser was configured for. It is given an
# existing group with -G, because a system account made without one is put in
# `nogroup`, and adduser exits rather than invent a group that is not there.
NEWUSER="testee"
NEWPASS="testeepw1"
NEXTPASS="testeepw2"
WRONGPASS="notthepassword"
SYSUSER="sysacct"
EXTRAGROUP="shared"

# The id ranges rach2.config asks adduser for: system accounts inside
# FIRST_SYSTEM_ID..LAST_SYSTEM_ID, regular ones above that up to LAST_ID.
SYS_ID_LOW=100
SYS_ID_HIGH=999
REG_ID_LOW=1000
REG_ID_HIGH=60000

cleanup_boot() {
    kill -9 "$QPID" 2>/dev/null || true
    wait "$QPID" 2>/dev/null || true
    kill "$FPID" 2>/dev/null || true
    rm -f "$FEED"
}

fail() {
    if [ -f "$LOG" ]; then
        cp "$LOG" "$DISK.fail.log" 2>/dev/null || true
    fi
    cleanup_boot
    echo "account-test: FAIL: $1" >&2
    echo "account-test: the failing boot's console log: $DISK.fail.log" >&2
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
#
# A password answers a prompt no shell printed, so it is fed as a line of its
# own like any other: the console's line discipline queues what is typed ahead
# of the prompt, and `bb_ask` reads it when it gets there rather than flushing
# it away.
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
            sleep 6
        done
        sleep 240
    ) > "$FEED" &
    FPID=$!
    qemu-system-i386 -cpu max -display none -kernel "$KERNEL" \
        -append "rach2.fsdisk" \
        -drive "file=$WORK,format=raw,if=ide,index=0,media=disk" \
        -serial stdio < "$FEED" > "$LOG" 2>&1 &
    QPID=$!
}

# Take an account's uid from a /etc/passwd record and fail unless it lands
# inside the range adduser was configured to allocate from. The number is read
# and compared rather than matched against a digit pattern, which is what makes
# the range an assertion instead of a shape, and it is read from the database
# rather than from an `id` run, since the uid is what was written down.
id_in_range() {
    who="$1"
    low="$2"
    high="$3"
    got=$(printf '%s\n' "$PASSWD_TEXT" | sed -n "s/^$who:[^:]*:\([0-9][0-9]*\):.*/\1/p" | head -1)
    if [ -z "$got" ]; then
        fail "no uid was written down for $who at all"
    fi
    if [ "$got" -lt "$low" ] || [ "$got" -gt "$high" ]; then
        fail "$who got uid $got, outside the $low..$high range adduser was configured for"
    fi
}

rm -f "$WORK"
cp "$DISK" "$WORK"

# The first boot: make the accounts, then use them. The creating commands are
# joined with `&&` and not `;`, so the token cannot be printed by a run in which
# one of them failed.
boot_with \
    "adduser -D $NEWUSER && addgroup $EXTRAGROUP && addgroup $NEWUSER $EXTRAGROUP && echo MADE\"\"IT" \
    "adduser -S -D -H -G $EXTRAGROUP $SYSUSER && echo SYS\"\"IT" \
    "echo $NEWUSER:$NEWPASS | chpasswd && echo SET\"\"IT" \
    "su $NEWUSER -c \"su $NEWUSER -c id && echo AUTH\"\"IT\"" \
    "$NEWPASS" \
    "su $NEWUSER -c \"su root -c id && echo CLIM\"\"BED; echo TRIED\"\"IT\"" \
    "$WRONGPASS" \
    "su $NEWUSER -c \"passwd && echo CHANGED\"\"IT\"" \
    "$NEWPASS" \
    "$NEXTPASS" \
    "$NEXTPASS" \
    "sync; echo SYNC\"\"IT"

if ! wait_token "MADEIT" 150; then
    fail "the account, the group, or the account's membership of it was never made"
fi
if ! wait_token "SYSIT" 60; then
    fail "the system account was never created"
fi
if ! wait_token "SETIT" 90; then
    fail "the password was never set"
fi
if ! grep -q "password for '$NEWUSER' changed" "$LOG"; then
    fail "chpasswd did not report changing the password"
fi

# The authentication itself. The outer su is root's and asks nothing; the inner
# one is the account's own and must ask, so what this proves is that the hash
# chpasswd wrote is the hash su reads back and checks.
#
# Every token here is behind an `&&` rather than a `;`. A token printed by the
# next command in a list says only that the shell got that far, which a refused
# `su` also allows; behind `&&` it is printed by the run that succeeded and by
# no other. That is also why `id` is not run anywhere else in this boot: a `uid=`
# line from some other command would answer this assertion just as well.
if ! wait_token "AUTHIT" 120; then
    fail "the account never authenticated with its own password"
fi
if ! grep -q "uid=[0-9][0-9]*($NEWUSER)" "$LOG"; then
    fail "su did not become the account"
fi
if ! wait_token "TRIEDIT" 120; then
    fail "the wrong-password attempt never returned"
fi
if ! grep -qi "incorrect password" "$LOG"; then
    fail "a wrong password was not refused"
fi
if grep -q "CLIMBED" "$LOG"; then
    fail "a wrong password climbed to root anyway"
fi
if grep -q "uid=0(root)" "$LOG"; then
    fail "a wrong password reached root anyway"
fi

# And the set-id binary, exercised as the unprivileged account it exists for.
if ! wait_token "CHANGEDIT" 180; then
    fail "passwd never returned"
fi
if ! grep -q "password for $NEWUSER changed by $NEWUSER" "$LOG"; then
    fail "passwd did not report the account changing its own password"
fi
if ! wait_token "SYNCIT" 60; then
    fail "the first boot never reached its sync"
fi
cleanup_boot

# The second boot. The password set through `passwd` must still authenticate
# from a cold start, which is the difference between a change that reached the
# platter and one that lived in a cache. Then the databases are read back, and
# the accounts deleted.
boot_with \
    "su $NEWUSER -c \"su $NEWUSER -c id && echo AGAIN\"\"IT\"" \
    "$NEXTPASS" \
    "mkpasswd -m sha512 -S abcdefgh hunter2; echo MK\"\"END" \
    "echo BEG\"\"IN; cat /etc/passwd; echo PW\"\"END" \
    "cat /etc/shadow; echo SH\"\"END" \
    "cat /etc/group; echo GR\"\"END" \
    "deluser $NEWUSER && deluser $SYSUSER && sync && echo GONE\"\"IT"

if ! wait_token "AGAINIT" 180; then
    fail "the changed password did not authenticate after a reboot"
fi
if ! wait_token "GREND" 180; then
    fail "the second boot could not read the databases back"
fi

# The hash generators, pinned against the value this project's own SHA-512
# crypt produces for the same password and salt, so the assertion is that
# busybox agrees with the implementation the rest of the image authenticates
# with, not merely that it printed something shaped like a hash.
# shellcheck disable=SC2016
EXPECTED_HASH='$6$abcdefgh$M/eYsB4rVXAm3ZNc88J.UD9rCKAT6FB1rahiwJCHtEndQNORCub5qhjxn50qbqVVthkM.9HpEwtf0t.iV9uH0/'
if ! grep -qF "$EXPECTED_HASH" "$LOG"; then
    fail "mkpasswd did not produce the hash this system's own crypt gives"
fi

# Each database is the text between the marker before it and the marker after,
# and every marker is written split, so a section can never be delimited by the
# console's echo of the command that printed it.
PASSWD_TEXT=$(sed -n '/BEGIN/,/PWEND/p' "$LOG")
SHADOW_TEXT=$(sed -n '/PWEND/,/SHEND/p' "$LOG")
GROUP_TEXT=$(sed -n '/SHEND/,/GREND/p' "$LOG")

# What the databases hold while the accounts are still there. The membership is
# asserted here rather than inferred from the delete, since a group the account
# never joined is also a group it cannot be left in.
case "$PASSWD_TEXT" in
    *"$NEWUSER:"*) ;;
    *) fail "the account is not in /etc/passwd" ;;
esac
case "$PASSWD_TEXT" in
    *"$SYSUSER:"*) ;;
    *) fail "the system account is not in /etc/passwd" ;;
esac
case "$SHADOW_TEXT" in
    *"$NEWUSER:\$6\$"*) ;;
    *) fail "the account's shadow record does not hold a SHA-512 hash" ;;
esac
id_in_range "$NEWUSER" "$REG_ID_LOW" "$REG_ID_HIGH"
id_in_range "$SYSUSER" "$SYS_ID_LOW" "$SYS_ID_HIGH"
if ! printf '%s\n' "$GROUP_TEXT" | grep -qE "^$EXTRAGROUP:.*[:,]$NEWUSER(,|\$)"; then
    fail "the account never joined the shared group"
fi

if ! wait_token "GONEIT" 120; then
    fail "the accounts were never deleted"
fi
cleanup_boot

# The third boot, which is the only honest place to ask what the delete left.
boot_with \
    "echo BEG\"\"IN; cat /etc/passwd; echo PW\"\"END" \
    "cat /etc/shadow; echo SH\"\"END" \
    "cat /etc/group; echo GR\"\"END"

if ! wait_token "GREND" 180; then
    fail "the third boot could not read the databases back"
fi
cleanup_boot

# Each database is the text between the marker before it and the marker after,
# and every marker is written split, so a section can never be delimited by the
# console's echo of the command that printed it.
PASSWD_TEXT=$(sed -n '/BEGIN/,/PWEND/p' "$LOG")
SHADOW_TEXT=$(sed -n '/PWEND/,/SHEND/p' "$LOG")
GROUP_TEXT=$(sed -n '/SHEND/,/GREND/p' "$LOG")

# The delete is two-way: gone from every database and from every member list,
# not merely gone from passwd, since a partial delete leaves exactly that.
for who in "$NEWUSER" "$SYSUSER"; do
    case "$PASSWD_TEXT" in
        *"$who:"*) fail "the deleted account $who is still in /etc/passwd" ;;
    esac
    case "$SHADOW_TEXT" in
        *"$who:"*) fail "the deleted account $who is still in /etc/shadow" ;;
    esac
    case "$GROUP_TEXT" in
        *"$who:"*) fail "the deleted account $who still has its own group" ;;
        *":$who"*) fail "the deleted account $who is still a member of a group" ;;
        *",$who"*) fail "the deleted account $who is still a member of a group" ;;
    esac
done

# The second group survived the delete, which is the difference between removing
# a member and removing the group somebody was a member of.
case "$GROUP_TEXT" in
    *"$EXTRAGROUP:"*) ;;
    *) fail "deleting the account took the shared group with it" ;;
esac

echo "account-test: two accounts were created, authenticated, changed and deleted without trace: OK"
echo "account-test: PASS"
