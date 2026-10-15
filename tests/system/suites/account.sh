#!/usr/bin/env sh

#
# account.sh: an image creates, authenticates, changes and deletes an
# account, and the three credential databases agree afterwards.
#
# It runs one of two arcs, named by the flavor argument, because the two
# images do not have the same tools. `real` is os-real, whose account tools
# are busybox's, on a disk. `simple` is os-simple, whose account tools are
# this project's own, in RAM: it has no `deluser`, so its arc creates,
# authenticates, climbs and changes a password and stops there; deleting an
# account in that image is root editing three files.
#
# The real arc boots a disk three times. The first boot makes the accounts
# and uses them, the second authenticates again from a cold start and reads
# the databases back, the third looks at what is left after the delete. So
# what is checked is what reached the disk, not what one process had in its
# cache.
#
# The authentication is done from an unprivileged account and not from root.
# Root's `su` never asks for a password, so a test that only ran `su testee`
# as root would pass with a wrong hash, a broken password check, or a `passwd`
# that cannot get its set-id bit, while every user was locked out. Here root
# becomes the account first, and the account authenticates from there: once
# with the right password, once with the wrong one, and once through `passwd`
# itself, which is the staged set-id binary and nothing else.
#
# The deletion checks are two-way. A check that only asked "does every
# account in passwd have a shadow line" passes an account that was removed
# from passwd and left in shadow, which is what a partial delete leaves.
#
# The console echoes what is typed, so a token that appears in a command is
# written split, or it would match its own echo; a line typed before the
# program that reads it is asking is eaten by one that flushes its input, as
# login does, so every line is typed at the prompt or the marker it answers;
# and the logs of an earlier run hold every token this run waits for, so they
# are removed before the first boot as well as in the trap.
#
# Usage: account.sh <kernel> <image> <password> [real|simple]
#

# shellcheck source=tests/system/deadline.sh
# The runner helper, whose `until` and `gone` are the suite's waits.
RUNUTIL="$(cd "$(dirname "$0")/../../.." && pwd)/build/tests/system/runutil"
. "$(cd "$(dirname "$0")/../../.." && pwd)/tests/system/deadline.sh"
set -e

KERNEL="$1"
DISK="$2"
PASSWORD="$3"
FLAVOR="${4:-real}"

if [ -z "$KERNEL" ] || [ -z "$DISK" ] || [ -z "$PASSWORD" ]; then
    echo "usage: account.sh <kernel> <image> <password> [real|simple]" >&2
    exit 2
fi

# What this suite writes is named after the suite, under the images
# directory: the os-simple image is shared, and a copy or a log named after
# it would be a file no suite declares.
OUT="$(dirname "$DISK")/account-$FLAVOR"
WORK="$OUT.work"
LOG="$OUT.work.log"

# How the image is attached, the one thing the two arcs boot differently:
# os-real is a disk the kernel mounts and writes back, os-simple is a module
# in RAM. It is expanded unquoted, since it is several arguments.
case "$FLAVOR" in
    real)   HARNESS_MEDIA="--append rach2.fsdisk --drive file=$WORK,format=raw,if=ide,index=0,media=disk" ;;
    simple) HARNESS_MEDIA="--initrd $WORK" ;;
    *)
        echo "account.sh: $FLAVOR: the flavor is real or simple" >&2
        exit 2
        ;;
esac

rm -f "$WORK" "$LOG"

QPID=""

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

# The simple arc's account, and the unprivileged account the image already
# stages, whose password gen_demo_password.sh minted beside root's.
SIMPLEUSER="newbie"
SIMPLEPASS="newbiepw1"
TESTPASSWORD="$(cat "$(dirname "$0")/../../../krffs/rach2_test_password" 2>/dev/null || echo none)"

# The id ranges rach2.config asks adduser for: system accounts inside
# FIRST_SYSTEM_ID..LAST_SYSTEM_ID, regular ones above that up to LAST_ID.
SYS_ID_LOW=100
SYS_ID_HIGH=999
REG_ID_LOW=1000
REG_ID_HIGH=60000

# cleanup_boot: stop the machine. It is safe to call twice, since the
# ordinary paths call it and the trap calls it again; without the trap a
# cancelled build would leave QEMU running on a disk copy whose name has been
# removed. The pid is cleared before it is signalled, so a trap that fires
# between the kill and the wait finds nothing left to kill.
cleanup_boot() {
    qpid="$QPID"
    QPID=""
    if [ -n "$qpid" ]; then
        kill -9 "$qpid" 2>/dev/null || true
        wait "$qpid" 2>/dev/null || true
    fi
}

# cleanup: stop the machine and remove the working image and the log. When
# the run did not pass, which includes a stop by the runner, the log is kept
# as $OUT.fail.log, the only record of what the machine did.
PASSED=""
cleanup() {
    cleanup_boot
    if [ -z "$PASSED" ] && [ -f "$LOG" ]; then
        cp "$LOG" "$OUT.fail.log" 2>/dev/null || true
    fi
    rm -f "$WORK" "$LOG"
}

trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

# fail REASON: keep the log as $OUT.fail.log, stop the machine, print REASON
# and exit 1.
fail() {
    if [ -f "$LOG" ]; then
        cp "$LOG" "$OUT.fail.log" 2>/dev/null || true
    fi
    cleanup_boot
    echo "account-test: FAIL: $1" >&2
    echo "account-test: the failing boot's console log: $OUT.fail.log" >&2
    exit 1
}

# wait_token PATTERN SECONDS: wait up to SECONDS for PATTERN in the log. A
# pattern anchored at the end allows trailing space, since the console ends
# its lines with a carriage return.
wait_token() {
    "$RUNUTIL" until "$LOG" "$1" "$2"
}

HARNESS="$(cd "$(dirname "$0")/../../.." && pwd)/build/tests/system/harness"
# boot_steps STEP...: boot the working image and type the arc's STEPs at its
# console through the harness: each line is typed once the output it answers
# has arrived, a login prompt, a password prompt or a marker the line before
# printed, so no line is eaten by a program that flushes its input before
# asking. The last step is the token the harness waits for, and it ends the
# machine once that token has been printed; the log stays for the checks.
boot_steps() {
    last=""
    for step; do
        last="$step"
    done
    case "$last" in
        expect=*) ;;
        *) fail "an arc must end with the token it waits for, not '$last'" ;;
    esac
    rm -f "$LOG"
    # shellcheck disable=SC2086
    "$HARNESS" --kernel "$KERNEL" $HARNESS_MEDIA --qemu-arg=-cpu --qemu-arg=max \
        --timeout 300 --steptmo 60 --log "$LOG" --ok "${last#expect=}" "$@" \
        > /dev/null 2>&1 \
        || fail "the machine did not answer every line of the arc; the log says which"
}

# boot_with STEP...: boot_steps for an arc that starts from root's shell; the
# console login is answered first.
boot_with() {
    boot_steps "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" "$@"
}

# id_in_range NAME LOW HIGH: fail unless the uid of NAME in $PASSWD_TEXT is
# within LOW..HIGH, the range adduser was configured to allocate from. The
# number is compared, not matched against a digit pattern, and it is read from
# the database, not from an `id` run, since the uid is what was written down.
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

# run_simple_arc: the arc for os-simple, whose tools are this project's own:
# `login` at the console, `adduser` to make an account, `su` to become it and
# `passwd` to change a password, with no `deluser` to finish with.
#
# Every step is typed at a shell whose PATH is the one `login` built, and the
# commands are bare names, so a PATH that reached nothing fails here rather
# than being worked around with absolute paths. The second half runs as the
# unprivileged `test` account, reached by leaving root's shell and answering
# the login prompt init puts back: `su` asks for a password, and `passwd`
# needs its set-id bit, only when the caller is not root.
#
# Each su attempt is read between its own markers: counted over the whole
# boot, the results would be met as well by a passwd that changed nothing,
# where the old password works and the new one is refused. A refused attempt
# is checked twice, that su refused and that the command behind it did not
# run, since the message alone would pass a `su` that printed a refusal and
# then ran the shell. After the change come three attempts: the old password,
# which must be refused; the new password offered to the other account, which
# must be refused too, since a passwd that rewrote the wrong record would
# leave that; and the new password for its own account, which must work. The
# markers are printed by `echo` with an ordinary argument, since the ISH shell
# does no quote removal and a split token would arrive with its quotes in it;
# the console's echo of a command lands just before the output it delimits,
# so it does not separate an attempt from its own result.
run_simple_arc() {
    boot_with \
        "send=pwd" "expect=/root" \
        "send=adduser $SIMPLEUSER" "expect=created with uid" "expect=New password:" "send=$SIMPLEPASS" \
        "expect=Retype new password:" "send=$SIMPLEPASS" "expect=password changed for $SIMPLEUSER" \
        "send=cat /etc/passwd" "expect=$SIMPLEUSER:x:" \
        "send=exit" "expect=rach2 login:" "send=test" "expect=Password:" "send=$TESTPASSWORD" \
        "send=pwd" "expect=/home/test" \
        "send=echo TRYWRONG" "expect=TRYWRONG" \
        "send=su - $SIMPLEUSER -c pwd" "expect=Password:" "send=$WRONGPASS" "expect=authentication failure" \
        "send=echo TRYRIGHT" "expect=TRYRIGHT" \
        "send=su - $SIMPLEUSER -c pwd" "expect=Password:" "send=$SIMPLEPASS" "expect=/home/$SIMPLEUSER" \
        "send=echo TRYDONE" "expect=TRYDONE" \
        "send=passwd" "expect=Old password:" "send=$TESTPASSWORD" \
        "expect=New password:" "send=$NEXTPASS" "expect=Retype new password:" "send=$NEXTPASS" \
        "expect=password changed" \
        "send=echo TRYOLD" "expect=TRYOLD" \
        "send=su - test -c pwd" "expect=Password:" "send=$TESTPASSWORD" "expect=authentication failure" \
        "send=echo TRYOTHER" "expect=TRYOTHER" \
        "send=su - $SIMPLEUSER -c pwd" "expect=Password:" "send=$NEXTPASS" "expect=authentication failure" \
        "send=echo TRYNEW" "expect=TRYNEW" \
        "send=su - test -c pwd" "expect=Password:" "send=$NEXTPASS" "expect=/home/test" \
        "send=echo TRYEND" "expect=TRYEND"

    if ! wait_token "^/root[[:space:]]*\$" 150; then
        fail "a bare command name after login found nothing; the PATH login builds is wrong"
    fi
    if ! wait_token "created with uid" 120; then
        fail "adduser never created the account"
    fi
    if ! wait_token "^$SIMPLEUSER:x:[0-9]" 120; then
        fail "the new account never reached /etc/passwd"
    fi
    if ! grep -q "^$SIMPLEUSER:x:[0-9]*:[0-9]*:.*:/home/${SIMPLEUSER}:/bin/sh[[:space:]]*\$" "$LOG"; then
        fail "the account adduser wrote names a home or a shell this image does not stage"
    fi

    # The unprivileged half.
    if ! wait_token "^/home/test[[:space:]]*\$" 180; then
        fail "the test account never reached a shell of its own"
    fi
    # The wrong and the right password, each read between its own markers.
    if ! wait_token "TRYDONE" 240; then
        fail "the account's own password was never tried"
    fi
    wrong_try=$(sed -n '/TRYWRONG/,/TRYRIGHT/p' "$LOG")
    right_try=$(sed -n '/TRYRIGHT/,/TRYDONE/p' "$LOG")
    case "$wrong_try" in
        *"authentication failure"*) ;;
        *) fail "su took a password the account does not have" ;;
    esac
    case "$wrong_try" in
        *"/home/$SIMPLEUSER"*) fail "a refused su ran the command anyway" ;;
    esac
    case "$right_try" in
        *"/home/$SIMPLEUSER"*) ;;
        *) fail "the account never authenticated with its own password" ;;
    esac
    if ! wait_token "password changed for test" 180; then
        fail "the account could not change its own password through the set-id passwd"
    fi

    # The three attempts after the change, each read between its own markers.
    if ! wait_token "TRYEND" 240; then
        fail "the attempts after the password change never finished"
    fi
    old_try=$(sed -n '/TRYOLD/,/TRYOTHER/p' "$LOG")
    other_try=$(sed -n '/TRYOTHER/,/TRYNEW/p' "$LOG")
    new_try=$(sed -n '/TRYNEW/,/TRYEND/p' "$LOG")
    # A refused attempt must also not have run the command behind it.
    case "$old_try" in
        *"authentication failure"*) ;;
        *) fail "the password the account replaced still authenticates it" ;;
    esac
    case "$old_try" in
        *"/home/test"*) fail "a refused su ran the command anyway" ;;
    esac
    case "$other_try" in
        *"authentication failure"*) ;;
        *) fail "the new password authenticates an account it was not set for" ;;
    esac
    case "$other_try" in
        *"/home/$SIMPLEUSER"*) fail "a refused su reached the other account anyway" ;;
    esac
    case "$new_try" in
        *"authentication failure"*)
            fail "the changed password does not authenticate the account it was changed for" ;;
    esac
    case "$new_try" in
        *"/home/test"*) ;;
        *) fail "the changed password did not reach the account's own shell" ;;
    esac
    cleanup_boot

    echo "account-test: an account was created, authenticated and changed with our own tools: OK"
    PASSED=1
    echo "account-test: PASS"
}

# run_real_arc: the arc for os-real, whose account tools are busybox's, in
# three boots of the disk.
#
# The first boot makes the accounts and uses them. The creating commands are
# joined with `&&` and not `;`, so a token cannot be printed by a run in which
# one of them failed. In the authentication the outer su is root's and asks
# nothing; the inner one is the account's own and must ask, so the row tests
# that the hash chpasswd wrote is the hash su reads back and checks. Every
# token there is behind an `&&`, since a token printed by the next command in
# a list says only that the shell got that far, which a refused `su` also
# allows; `id` runs nowhere else in the boot, so no other `uid=` line can
# satisfy the check. The set-id passwd is then run as the account. What it
# must refuse comes first, a wrong old password and another account's record,
# each token printed by a `grep` of the program's own complaint, so a passwd
# that changed the password without saying anything prints neither; busybox
# checks who is asking before it asks for a password, so the second refusal
# needs none.
#
# The second boot authenticates with the changed password from a cold start,
# so the change reached the disk and not only a cache. `httpd -m` makes an
# MD5-crypt hash, which busybox httpd accepts in its config; both are tested
# over the loopback address, with no network device in the machine, and the
# request with no credential must be refused and the one with the right
# credential served. Each request is read between its own markers, since over
# the whole log a 401 and the page would also appear for an httpd that had the
# two the wrong way round. mkpasswd's SHA-512 hash must be the one this
# project's own crypt gives for the same password and salt, the crypt the rest
# of the image authenticates with. Then the databases are read back and the
# accounts deleted. The third boot reads what the delete left on disk.
run_real_arc() {

    # The first boot: make the accounts, then use them.
    boot_with \
        "send=adduser -D $NEWUSER && addgroup $EXTRAGROUP && addgroup $NEWUSER $EXTRAGROUP && echo MADE\"\"IT" "expect=MADEIT" \
        "send=adduser -S -D -H -G $EXTRAGROUP $SYSUSER && echo SYS\"\"IT" "expect=SYSIT" \
        "send=echo $NEWUSER:$NEWPASS | chpasswd && echo SET\"\"IT" "expect=SETIT" \
        "send=su $NEWUSER -c \"su $NEWUSER -c id && echo AUTH\"\"IT\"" "expect=Password:" "send=$NEWPASS" "expect=AUTHIT" \
        "send=su $NEWUSER -c \"su root -c id && echo CLIM\"\"BED; echo TRIED\"\"IT\"" "expect=Password:" "send=$WRONGPASS" "expect=TRIEDIT" \
        "send=su $NEWUSER -c \"passwd; echo REFU\"\"SED\"" "expect=Old password:" "send=$WRONGPASS" "expect=REFUSED" \
        "send=su $NEWUSER -c \"passwd root 2>&1 | grep -q change && echo NOT\"\"MINE\"" "expect=NOTMINE" \
        "send=su $NEWUSER -c \"passwd && echo CHANGED\"\"IT\"" "expect=Old password:" "send=$NEWPASS" \
        "expect=New password:" "send=$NEXTPASS" "expect=Retype password:" "send=$NEXTPASS" "expect=CHANGEDIT" \
        "send=sync; echo SYNC\"\"IT" "expect=SYNCIT"

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

    # The authentication, with every token behind an `&&`.
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

    # The set-id passwd, run as the unprivileged account.
    if ! wait_token "REFUSED" 180; then
        fail "passwd took a wrong old password, or refused it without saying so"
    fi
    if ! wait_token "NOTMINE" 120; then
        fail "passwd let an account change another account's password"
    fi
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

    # The second boot: a cold start, the hashes, the databases and the delete.
    boot_with \
        "send=su $NEWUSER -c \"su $NEWUSER -c id && echo AGAIN\"\"IT\"" "expect=Password:" "send=$NEXTPASS" "expect=AGAINIT" \
        "send=mkdir -p /www; echo SERVED\"\"OK > /www/index.html; echo MADE\"\"WWW" "expect=MADEWWW" \
        "send=httpd -m hunter2 > /tmp/webhash; cat /tmp/webhash; echo HASH\"\"ED" "expect=HASHED" \
        "send=echo /:webuser:\$(cat /tmp/webhash) > /etc/httpd.conf; httpd -p 8080 -h /www; echo SERV\"\"ING" "expect=SERVING" \
        "send=echo ANON\"\"BEGIN; wget -q -O - http://127.0.0.1:8080/index.html 2>&1; echo ANON\"\"END" "expect=ANONEND" \
        "send=echo AUTH\"\"BEGIN; wget -q -O - http://webuser:hunter2@127.0.0.1:8080/index.html 2>&1; echo AUTH\"\"END" "expect=AUTHEND" \
        "send=mkpasswd -m sha512 -S abcdefgh hunter2; echo MK\"\"END" "expect=MKEND" \
        "send=echo BEG\"\"IN; cat /etc/passwd; echo PW\"\"END" "expect=PWEND" \
        "send=cat /etc/shadow; echo SH\"\"END" "expect=SHEND" \
        "send=cat /etc/group; echo GR\"\"END" "expect=GREND" \
        "send=deluser $NEWUSER && deluser $SYSUSER && sync && echo GONE\"\"IT" "expect=GONEIT"

    if ! wait_token "AGAINIT" 180; then
        fail "the changed password did not authenticate after a reboot"
    fi
    if ! wait_token "GREND" 180; then
        fail "the second boot could not read the databases back"
    fi

    # The hash `httpd -m` makes, and the server that accepts it.
    # shellcheck disable=SC2016
    if ! grep -qE '^\$1\$[^$]+\$' "$LOG"; then
        fail "httpd -m did not mint an MD5 hash"
    fi
    # Each request is read between its own markers.
    ANON_TEXT=$(sed -n '/ANONBEGIN/,/ANONEND/p' "$LOG")
    AUTH_TEXT=$(sed -n '/AUTHBEGIN/,/AUTHEND/p' "$LOG")
    case "$ANON_TEXT" in
        *"401 Unauthorized"*) ;;
        *) fail "httpd did not refuse a request carrying no credential" ;;
    esac
    case "$ANON_TEXT" in
        *SERVEDOK*) fail "httpd served the page to a request carrying no credential" ;;
    esac
    case "$AUTH_TEXT" in
        *SERVEDOK*) ;;
        *) fail "httpd refused the credential its own -m minted" ;;
    esac

    # mkpasswd's hash, against the one this project's own SHA-512 crypt gives.
    # shellcheck disable=SC2016
    EXPECTED_HASH='$6$abcdefgh$M/eYsB4rVXAm3ZNc88J.UD9rCKAT6FB1rahiwJCHtEndQNORCub5qhjxn50qbqVVthkM.9HpEwtf0t.iV9uH0/'
    if ! grep -qF "$EXPECTED_HASH" "$LOG"; then
        fail "mkpasswd did not produce the hash this system's own crypt gives"
    fi

    # Each database lies between two split markers, which no echoed command has.
    PASSWD_TEXT=$(sed -n '/BEGIN/,/PWEND/p' "$LOG")
    SHADOW_TEXT=$(sed -n '/PWEND/,/SHEND/p' "$LOG")
    GROUP_TEXT=$(sed -n '/SHEND/,/GREND/p' "$LOG")

    # The membership is checked now, since a group never joined is never left.
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

    # The third boot, the only boot that shows what the delete left on disk.
    boot_with \
        "send=echo BEG\"\"IN; cat /etc/passwd; echo PW\"\"END" "expect=PWEND" \
        "send=cat /etc/shadow; echo SH\"\"END" "expect=SHEND" \
        "send=cat /etc/group; echo GR\"\"END" "expect=GREND"

    if ! wait_token "GREND" 180; then
        fail "the third boot could not read the databases back"
    fi
    cleanup_boot

    # Each database lies between two split markers, which no echoed command has.
    PASSWD_TEXT=$(sed -n '/BEGIN/,/PWEND/p' "$LOG")
    SHADOW_TEXT=$(sed -n '/PWEND/,/SHEND/p' "$LOG")
    GROUP_TEXT=$(sed -n '/SHEND/,/GREND/p' "$LOG")

    # The delete is two-way: gone from every database and every member list.
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

    # The shared group survives the delete of its member.
    case "$GROUP_TEXT" in
        *"$EXTRAGROUP:"*) ;;
        *) fail "deleting the account took the shared group with it" ;;
    esac

    echo "account-test: two accounts were created, authenticated, changed and deleted without trace: OK"
    PASSED=1
    echo "account-test: PASS"

}

case "$FLAVOR" in
    real)   run_real_arc ;;
    simple) run_simple_arc ;;
esac
