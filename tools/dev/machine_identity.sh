#!/bin/sh
#
# machine_identity.sh: check that an installed machine runs the named kernel
# from the named slot, before a measurement driver times anything.
#
# Usage: machine_identity.sh LOG SSH-COMMAND...
#
# The machine is asked which slot it booted and which kernel that slot holds,
# and the measurement goes on only when they are the slot and the kernel the
# caller names. RACH2_MEASURE_SLOT names the slot, candidate or fallback, and
# RACH2_MEASURE_KERNEL the kernel file on this host, the build the
# measurement is meant to be of; a caller that leaves either out is refused.
#
# The booted slot is `rach2.slot=` on the machine's /proc/cmdline, the value
# `bootctl status` reports as `booted`. The kernel is the SHA-256 of that
# slot's first bytes, as many as the named file holds, read from
# /dev/bootpart at the extent `bootctl status` prints; the slot is padded
# past the kernel, so the padding is left out. The read takes the slot's
# sectors as one run, which is how the image tools write a slot; a slot
# whose clusters are scattered reads as another kernel and is refused.
#
# A trial armed in the boot environment is refused too: `bootctl kernel`
# arms one when it writes the candidate slot, and the slot then holds a
# kernel this boot did not load. Two rewrites of the booted slot leave no
# such mark and are not seen: `bootctl fallback` run on a machine booted from
# the fallback slot, and `bootctl kernel` on one booted from the candidate
# with its trial then dropped by `bootctl cancel`.
#
# SSH-COMMAND is the driver's own ssh invocation without a remote command;
# this script appends one. `bootctl status` reads /dev/bootpart, whose mode
# is 0600, so the command logs in as root. LOG receives the machine's report,
# the names the caller gave and the verdict, so that a measurement's logs
# say which kernel it timed. Exit 0 when the machine runs the named kernel
# from the named slot, 1 when it does not or cannot say, 2 when the caller
# named nothing.
set -eu
[ "$#" -ge 2 ] || {
    echo 'usage: machine_identity.sh LOG SSH-COMMAND...' >&2
    exit 2
}
log=$1
shift
want=${RACH2_MEASURE_SLOT:-}
kernel=${RACH2_MEASURE_KERNEL:-}
case "$want" in
    candidate | fallback) ;;
    *)
        echo 'machine-identity: refused: RACH2_MEASURE_SLOT must name the slot to measure, candidate or fallback' >&2
        exit 2
        ;;
esac
if [ ! -f "$kernel" ] || [ ! -r "$kernel" ]; then
    echo 'machine-identity: refused: RACH2_MEASURE_KERNEL must name the kernel file the machine is to run' >&2
    exit 2
fi
size=$(wc -c <"$kernel" | tr -d ' ')
case "$size" in
    '' | 0 | *[!0-9]*)
        echo 'machine-identity: refused: the named kernel file is empty' >&2
        exit 2
        ;;
esac
sum=$(shasum -a 256 <"$kernel")
sum=${sum%% *}

# refuse TEXT: write the refusal to LOG and stderr, and exit 1.
refuse()
{
    echo "machine-identity: refused: $*" >>"$log"
    echo "machine-identity: refused: $*" >&2
    exit 1
}

# The machine's half, sent as one command: the booted slot, the boot
# environment and extents, and the digest of the booted slot's first SIZE
# bytes when the slot has an extent. It decides nothing; the first line is a
# no-op that names the command.
remote=$(sed "s/@SIZE@/$size/" <<'EOF'
: machine-identity
slot=$(sed -n 's/.*rach2\.slot=\([a-z]*\).*/\1/p' /proc/cmdline)
echo "machine-identity: booted: ${slot:-none}"
status=$(bootctl status) || exit 1
printf '%s\n' "$status"
extent=$(printf '%s\n' "$status" | awk -v s="$slot:" '$2 == s && $3 == "first" { sub(",", "", $5); print $5, $6 }')
set -- $extent
if [ -n "$slot" ] && [ "$#" = 2 ]; then
    dd if=/dev/bootpart bs=512 skip="$1" count="$2" 2>/dev/null | head -c @SIZE@ | sha256sum | sed 's/ .*//; s/^/machine-identity: sha256: /'
fi
EOF
)
if ! "$@" "$remote" </dev/null >"$log" 2>&1; then
    cat "$log" >&2
    refuse 'the machine did not report its booted slot and boot environment'
fi
booted=$(sed -n 's/^machine-identity: booted: //p' "$log")
[ "$booted" != none ] || booted=
have=$(sed -n 's/^machine-identity: sha256: //p' "$log")
next=$(sed -n 's/^bootctl: next_entry: //p' "$log")
echo "machine-identity: named: slot $want, kernel $kernel, $size bytes, sha256 $sum" >>"$log"
case "$next" in
    absent | empty*) trial=none ;;
    *) trial=armed ;;
esac
[ "$booted" = "$want" ] || refuse "the machine booted ${booted:-no slot}, not the named $want"
[ "$trial" = none ] || refuse "a trial is armed (next_entry ${next:-unread}); the $want slot may hold a kernel this boot did not load"
[ "$have" = "$sum" ] || refuse "the $want slot holds sha256 ${have:-unread}, not the named kernel's $sum"
result="machine-identity: result: the machine runs the named kernel: slot $booted, sha256 $sum, $kernel"
echo "$result" >>"$log"
echo "$result"
