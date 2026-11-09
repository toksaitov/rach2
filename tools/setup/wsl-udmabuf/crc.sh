#!/bin/sh
#
# crc.sh: check symbol versions against the running WSL kernel's, which its
# installed modules record, before a module built out of its tree loads.
#
# Usage:
#   sh crc.sh MODULE     every import of MODULE, a .ko
#   sh crc.sh -s FILE    every vmlinux symbol the installed modules import,
#                        against FILE, the Module.symvers of a built tree
#
# The kernel has CONFIG_MODVERSIONS, so it refuses an import whose CRC is
# not its own, and an import whose CRC is right but whose type differs is
# not caught at all. Each installed module under /lib/modules/$(uname -r)
# loads, so the CRCs it was built against, which modprobe --dump-modversions
# prints, are the running kernel's. MODULE passes when none of its imports
# differs from them; an import that no installed module makes is reported
# and passes, since the kernel checks it at the load. FILE passes when every
# import of every installed module has FILE's CRC, which proves that the
# tree's type definitions are the running kernel's. Exits 1 on a difference.
set -eu

PATH=$PATH:/usr/sbin:/sbin

die()
{
    echo "crc: $*" >&2
    exit 1
}

[ $# -ge 1 ] || die "usage: crc.sh MODULE | crc.sh -s FILE"
mods=/lib/modules/$(uname -r)
[ -d "$mods" ] || die "no $mods"
finished='' status=0
tmp=$(mktemp -d "${TMPDIR:-/tmp}/rach2-crc.XXXXXX")
trap 'status=$?; rm -rf "$tmp"; [ -n "$finished" ] || [ "$status" -ne 0 ] || status=1; exit "$status"' EXIT

# Every import of every installed module, one "SYMBOL CRC" a line.
find "$mods" -name '*.ko*' | while read -r ko; do
    modprobe --dump-modversions "$ko" 2>/dev/null || true
done | awk 'NF == 2 { print $2, $1 }' | sort > "$tmp/installed"
[ -s "$tmp/installed" ] || die "no symbol versions read from $mods"

if [ "$1" = -s ]; then
    [ $# -eq 2 ] && [ -f "$2" ] || die "usage: crc.sh -s FILE"
    awk -F '\t' '$3 == "vmlinux" { print $2, $1 }' "$2" > "$tmp/tree"
    awk '
        NR == FNR { tree[$1] = $2; next }
        $1 in tree {
            if (tolower(tree[$1]) == tolower($2)) {
                eq++
            } else {
                ne++
                if (!($1 in seen)) {
                    seen[$1] = 1
                    print "crc: " $1 " tree " tree[$1] " installed " $2
                }
            }
        }
        END {
            printf "crc: tree: %d imports of vmlinux symbols, %d equal, %d different\n", eq + ne, eq, ne
            exit ne > 0
        }' "$tmp/tree" "$tmp/installed" || die "result: the tree's symbol versions differ from the running kernel's"
    finished=1
    exit 0
fi

[ $# -eq 1 ] && [ -f "$1" ] || die "usage: crc.sh MODULE"
modprobe --dump-modversions "$1" | awk 'NF == 2 { print $2, $1 }' | sort > "$tmp/module"
[ -s "$tmp/module" ] || die "$1 has no symbol versions"
sort -u "$tmp/installed" > "$tmp/have"
awk '
    NR == FNR { have[$1] = have[$1] " " $2; next }
    {
        n++
        if (!($1 in have)) {
            miss++
            print "crc: " $1 " " $2 " not imported by any installed module"
        } else if (have[$1] == " " $2) {
            eq++
        } else {
            ne++
            print "crc: " $1 " ours " $2 " installed" have[$1]
        }
    }
    END {
        printf "crc: %d imports, %d equal, %d different, %d not seen\n", n, eq, ne, miss
        exit ne > 0
    }' "$tmp/have" "$tmp/module" || die "result: an import's CRC differs; not loading"
finished=1
