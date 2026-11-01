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
# `bootctl status` reports as `booted`. The kernel is the SHA-256 of the
# slot file's first bytes, as many as the named file holds; the slot is
# padded past the kernel, so the padding is left out. Each slot file is read
# from /dev/bootpart by its cluster chain in the FAT, starting at the first
# sector `bootctl status` prints, since `bootctl fallback` can leave the
# fallback in more than one run and a `bootctl` older than the `runs:` line
# names only the first. Both slots are hashed and logged; the verdict is the
# booted slot's, and the other slot's digest is reported beside it.

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
# environment and extents, each slot's runs by its FAT chain and the digest
# of each slot's first SIZE bytes. It decides nothing; the first line is a
# no-op that names the command. The boot sector gives the FAT's geometry:
# bytes a sector at offset 11, sectors a cluster at 13, reserved sectors at
# 14, FATs at 16 and sectors a FAT at 36; a FAT32 entry's top four bits are
# reserved.
remote=$(sed "s/@SIZE@/$size/" <<'EOF'
: machine-identity
slot=$(sed -n 's/.*rach2\.slot=\([a-z]*\).*/\1/p' /proc/cmdline)
echo "machine-identity: booted: ${slot:-none}"
status=$(bootctl status) || exit 1
printf '%s\n' "$status"
set -- $(dd if=/dev/bootpart bs=512 count=1 2>/dev/null | od -An -tu1 -v -j11 -N29)
[ "$#" -eq 29 ] || exit 1
bps=$(($1 + $2 * 256))
spc=$3
res=$(($4 + $5 * 256))
fatsz=$((${26} + ${27} * 256 + ${28} * 65536 + ${29} * 16777216))
data=$((res + $6 * fatsz))
first() {
    printf '%s\n' "$status" | awk -v s="$1:" '$2 == s && $3 == "first" { sub(",", "", $5); print $5 }'
}
c1=$(first candidate)
c2=$(first fallback)
runs=$(dd if=/dev/bootpart bs="$bps" skip="$res" count="$fatsz" 2>/dev/null | od -An -tu4 -v |
    awk -v c1="${c1:-0}" -v c2="${c2:-0}" -v data="$data" -v spc="$spc" \
        -v need=$(((@SIZE@ + bps * spc - 1) / (bps * spc))) '
function walk(name, sector,    c, k, prev, start, len) {
    if (sector < data)
        return
    c = (sector - data) / spc + 2
    for (k = 0; k < need && c >= 2 && c < n; k++) {
        if (len && c == prev + 1)
            len++
        else {
            if (len)
                print name, data + (start - 2) * spc, len * spc
            start = c
            len = 1
        }
        prev = c
        c = fat[c] % 268435456
    }
    if (len)
        print name, data + (start - 2) * spc, len * spc
}
{ for (i = 1; i <= NF; i++) fat[n++] = $i }
END { walk("candidate", c1); walk("fallback", c2) }')
for name in candidate fallback; do
    list=$(printf '%s\n' "$runs" | awk -v s="$name" '$1 == s { printf " %s+%s", $2, $3 }')
    [ -n "$list" ] || continue
    echo "machine-identity: $name: runs:$list"
    printf '%s\n' "$runs" | awk -v s="$name" '$1 == s { print $2, $3 }' | while read -r at count; do
        dd if=/dev/bootpart bs="$bps" skip="$at" count="$count" 2>/dev/null
    done | head -c @SIZE@ | sha256sum | sed "s/ .*//; s/^/machine-identity: sha256: $name /"
done
EOF
)
if ! "$@" "$remote" </dev/null >"$log" 2>&1; then
    cat "$log" >&2
    refuse 'the machine did not report its booted slot and boot environment'
fi
booted=$(sed -n 's/^machine-identity: booted: //p' "$log")
[ "$booted" != none ] || booted=
have=$(sed -n "s/^machine-identity: sha256: ${booted:-none} //p" "$log")
next=$(sed -n 's/^bootctl: next_entry: //p' "$log")
echo "machine-identity: named: slot $want, kernel $kernel, $size bytes, sha256 $sum" >>"$log"
case "$next" in
    absent | empty*) trial=none ;;
    *) trial=armed ;;
esac
[ "$booted" = "$want" ] || refuse "the machine booted ${booted:-no slot}, not the named $want"
[ "$trial" = none ] || refuse "a trial is armed (next_entry ${next:-unread}); the $want slot may hold a kernel this boot did not load"
[ "$have" = "$sum" ] || refuse "the $want slot holds sha256 ${have:-unread}, not the named kernel's $sum"
case "$booted" in
    candidate) other=fallback ;;
    *) other=candidate ;;
esac
also=$(sed -n "s/^machine-identity: sha256: $other //p" "$log")
if [ "$also" = "$sum" ]; then
    also="the $other slot holds it too"
else
    also="the $other slot holds sha256 ${also:-unread}"
fi
result="machine-identity: result: the machine runs the named kernel: slot $booted, sha256 $sum, $kernel; $also"
echo "$result" >>"$log"
echo "$result"
