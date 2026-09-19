#!/usr/bin/env sh
#
# man.sh: the manual pages on the machine.
#
# Boots the os-real image, logs in as root and reads the pages two ways.
# Through a pipe, `man` must give every staged page byte for byte, each one
# found by its name and section and compared with the staged file
# decompressed, and every alias of the table must give its target's page
# the same way; a page `man` cannot find, or gives differently, is named as
# BAD and fails the run. On the console under the pager, rach2(7)'s first
# line and its last must appear, the last after the pager is sent to the end.
#
# Then the recovery image, whose console is a shell without a login, must give
# every page it stages the same way: the pages man/recovery lists are the ones
# a person at a broken machine is directed to.
#
# The count each boot reads is required to be the count the host expects:
# every rendered page under build/man and every line of man/aliases on
# os-real, and every line of man/recovery on the recovery image, so a
# section or an alias the image did not stage fails the run rather than
# going unread.
#
# Usage: man.sh KERNEL IMAGE HARNESS PASSWORD PAGE LOG RKERNEL RIMAGE
#   PAGE is the rendered page on the host, build/man/cat7/rach2.7, whose first
#   and last lines are what the console must show. RKERNEL and RIMAGE are the
#   recovery kernel and image; the second boot's log is LOG with -recovery
#   before its suffix.
set -u
KERNEL=$1; IMAGE=$2; HARNESS=$3; PASSWORD=$4; PAGE=$5; LOG=$6; RKERNEL=$7; RIMAGE=$8
fail() { echo "man-test: FAIL: $1" >&2; exit 1; }
MANDIR=$(dirname "$(dirname "$PAGE")")
pages=$(find "$MANDIR" -path "$MANDIR/cat*" -type f | wc -l | tr -d ' ')
aliases=$(grep -cv '^#' man/aliases | tr -d ' ')
expected=$((pages + aliases))
rexpected=$(grep -cv '^#' man/recovery | tr -d ' ')
[ "$pages" -gt 0 ] || fail "no rendered page under $MANDIR"
# Every staged page by its own name, then every alias of the table against
# its target's page, each loop one line typed at the shell; the recovery
# boot has only the pages.
READPAGES="for f in \$M/cat*/*; do b=\${f##*/}; man \${b##*.} \${b%.*} > /tmp/p; gunzip -c \$f > /tmp/q; cmp -s /tmp/p /tmp/q && test -s /tmp/p || echo BAD \$f; n=\$((n+1)); done"
READALIASES="while read -r a s t; do case \$a in '#'*|'') continue;; esac; man \$s \$a > /tmp/p; gunzip -c \$M/cat\$s/\$t.\$s > /tmp/q; cmp -s /tmp/p /tmp/q && test -s /tmp/p || echo BAD \$a; n=\$((n+1)); done < \$M/aliases"

[ -s "$PAGE" ] || fail "the rendered page $PAGE is missing or empty"
# The lines are matched as fixed text: the harness's expect takes a regular
# expression, so the parentheses and dots of a page's header are escaped.
# shellcheck disable=SC2016 # the dollar is a member of the class
first=$(gunzip -c "$PAGE" | head -1 | sed 's/[][\.*^$()|+?{}]/\\&/g')
# shellcheck disable=SC2016
last=$(gunzip -c "$PAGE" | tail -1 | sed 's/[][\.*^$()|+?{}]/\\&/g')

if "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" --qemu-arg=-cpu --qemu-arg=max \
    --timeout 600 --steptmo 480 --ok "MANDONE" --never "BAD /" --log "$LOG" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=# " "send=n=0; M=/usr/share/man" "expect=# " "send=$READPAGES" "expect=# " "send=$READALIASES" \
    "expect=# " "send=echo MAN\"\"ALL \$n" \
    "expect=MANALL ${expected}[^0-9]" "send=man rach2" "expect=$first" "sendraw=G" "expect=$last" \
    "sendraw=q" "expect=# " "send=echo MAN\"\"DONE" "expect=MANDONE" > /dev/null 2>&1; then
    count=$(grep -a "MANALL [0-9]" "$LOG" | tail -1 | tr -d '\r' | sed 's/.*MANALL //')
    echo "man-test: every staged page, $count of them, is what man gives through a pipe, byte for byte: OK"
    echo "man-test: and rach2(7) on the console under the pager shows its first line and its last: OK"
else
    grep -a "MAN\|man:\|RACH2\|less" "$LOG" | tr -d '\r' | tail -8 >&2 || true
    fail "the manual on the machine"
fi

RLOG=${LOG%.log}-recovery.log
if "$HARNESS" --kernel "$RKERNEL" --initrd "$RIMAGE" --qemu-arg=-cpu --qemu-arg=max \
    --timeout 300 --steptmo 240 --ok "MANDONE" --never "BAD /" --log "$RLOG" \
    "expect=recovery image" "expect=# " "send=n=0; M=/usr/share/man" "expect=# " "send=$READPAGES" \
    "expect=# " "send=echo MAN\"\"ALL \$n" \
    "expect=MANALL ${rexpected}[^0-9]" "send=echo MAN\"\"DONE" "expect=MANDONE" > /dev/null 2>&1; then
    count=$(grep -a "MANALL [0-9]" "$RLOG" | tail -1 | tr -d '\r' | sed 's/.*MANALL //')
    echo "man-test: the recovery image gives its $count pages through the pipe the same way: OK"
else
    grep -a "MAN\|man:" "$RLOG" | tr -d '\r' | tail -8 >&2 || true
    fail "the manual on the recovery image"
fi
