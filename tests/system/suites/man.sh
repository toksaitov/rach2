#!/usr/bin/env sh
#
# man.sh: the manual pages on the machine.
#
# Boots the os-real image, logs in as root and reads rach2(7) two ways. Through
# a pipe, `man` must give the staged page byte for byte: BusyBox's `man` puts
# its output through `col -b -p -x` when standard output is not a terminal,
# and this system's `col` copies its input, since the pages had their
# overstrikes removed on the host. On the console under the pager, the page's
# first line and its last must appear, the last after the pager is sent to the
# end. `man`'s exit status proves nothing: it ignores its pager's.
#
# Usage: man.sh KERNEL IMAGE HARNESS PASSWORD PAGE LOG
#   PAGE is the rendered page on the host, build/man/cat7/rach2.7, whose first
#   and last lines are what the console must show.
set -u
KERNEL=$1; IMAGE=$2; HARNESS=$3; PASSWORD=$4; PAGE=$5; LOG=$6
fail() { echo "man-test: FAIL: $1" >&2; exit 1; }

[ -s "$PAGE" ] || fail "the rendered page $PAGE is missing or empty"
# The lines are matched as fixed text: the harness's expect takes a regular
# expression, so the parentheses and dots of a page's header are escaped.
# shellcheck disable=SC2016 # the dollar is a member of the class
first=$(head -1 "$PAGE" | sed 's/[][\.*^$()|+?{}]/\\&/g')
# shellcheck disable=SC2016
last=$(tail -1 "$PAGE" | sed 's/[][\.*^$()|+?{}]/\\&/g')

if "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" --qemu-arg=-cpu --qemu-arg=max \
    --timeout 120 --steptmo 60 --ok "MANDONE" --log "$LOG" \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" \
    "expect=# " "send=man rach2 > /tmp/rach2.7; cmp /tmp/rach2.7 /usr/share/man/cat7/rach2.7 && test -s /tmp/rach2.7 && echo MAN\"\"SAME" \
    "expect=MANSAME" "send=man rach2" "expect=$first" "sendraw=G" "expect=$last" \
    "sendraw=q" "expect=# " "send=echo MAN\"\"DONE" "expect=MANDONE" > /dev/null 2>&1; then
    echo "man-test: man rach2 through a pipe is the staged page byte for byte: OK"
    echo "man-test: and on the console under the pager it shows its first line and its last: OK"
else
    grep -a "MAN\|man:\|RACH2\|less" "$LOG" | tr -d '\r' | tail -8 >&2 || true
    fail "the manual on the machine"
fi
