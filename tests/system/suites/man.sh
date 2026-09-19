#!/usr/bin/env sh
#
# man.sh: the manual pages on the machine.
#
# Boots the os-real image, logs in as root and reads the pages two ways.
# Through a pipe, `man` must give every staged page byte for byte, each one
# found by its name and section, an alias's gzipped copy decompressed first,
# since `man` opens a page by its signature: BusyBox's `man` puts its output
# through
# `col -b -p -x` when standard output is not a terminal, and this system's
# `col` copies its input, since the pages had their overstrikes removed on the
# host; a page `man` cannot find, or gives differently, is named as BAD and
# fails the run. On the console under the pager, rach2(7)'s first line and its
# last must appear, the last after the pager is sent to the end. `man`'s exit
# status proves nothing: it ignores its pager's.
#
# Then the recovery image, whose console is a shell without a login, must give
# every page it stages the same way: the pages man/recovery lists are the ones
# a person at a broken machine is directed to.
#
# The count each boot reads is required to be the count the host expects:
# every rendered page and alias copy under build/man on os-real, and every
# line of man/recovery on the recovery image, so a section or an alias the
# image did not stage fails the run rather than going unread.
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
expected=$(find "$MANDIR" -path "$MANDIR/cat*" -type f | wc -l | tr -d ' ')
rexpected=$(grep -cv '^#' man/recovery | tr -d ' ')
[ "$expected" -gt 0 ] || fail "no rendered page under $MANDIR"
READALL="n=0; for f in /usr/share/man/cat*/*; do b=\${f##*/}; man \${b##*.} \${b%.*} > /tmp/p; if gunzip -t \$f 2>/dev/null; then gunzip -c \$f > /tmp/q; else cp \$f /tmp/q; fi; cmp -s /tmp/p /tmp/q && test -s /tmp/p || echo BAD \$f; n=\$((n+1)); done; echo MAN\"\"ALL \$n"

[ -s "$PAGE" ] || fail "the rendered page $PAGE is missing or empty"
# The lines are matched as fixed text: the harness's expect takes a regular
# expression, so the parentheses and dots of a page's header are escaped.
# shellcheck disable=SC2016 # the dollar is a member of the class
first=$(head -1 "$PAGE" | sed 's/[][\.*^$()|+?{}]/\\&/g')
# shellcheck disable=SC2016
last=$(tail -1 "$PAGE" | sed 's/[][\.*^$()|+?{}]/\\&/g')

if "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" --qemu-arg=-cpu --qemu-arg=max \
    --timeout 600 --steptmo 480 --ok "MANDONE" --never "BAD /" --log "$LOG" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=# " "send=$READALL" \
    "expect=MANALL $expected[^0-9]" "send=man rach2" "expect=$first" "sendraw=G" "expect=$last" \
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
    "expect=recovery image" "expect=# " "send=$READALL" \
    "expect=MANALL $rexpected[^0-9]" "send=echo MAN\"\"DONE" "expect=MANDONE" > /dev/null 2>&1; then
    count=$(grep -a "MANALL [0-9]" "$RLOG" | tail -1 | tr -d '\r' | sed 's/.*MANALL //')
    echo "man-test: the recovery image gives its $count pages through the pipe the same way: OK"
else
    grep -a "MAN\|man:" "$RLOG" | tr -d '\r' | tail -8 >&2 || true
    fail "the manual on the recovery image"
fi
