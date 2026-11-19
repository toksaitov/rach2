#!/bin/sh
# bootwords.sh: give the installed root's boot lines release.sh's words; run on
# the Lenovo's recovery system as `sh -s BOOT WORDS`, the script on stdin.
#
# Each line of the menu on the boot partition BOOT that boots the root's
# candidate or fallback slot gets WORDS, in place of any ramoops or
# netconsole word it had, so that a release that runs again replaces the
# words of the one before. No bootctl verb rewrites the menu, so the words
# stay on the lines that a commit and a fallback copy select. The new menu
# is written beside the old one and put in place in the order rach2-install's
# finish_file and publish_file use, so that a stop leaves the old menu or the
# new one whole, or a kept copy that the next run puts back; then bootctl
# reads the partition. Exit 0 on success; otherwise exit 1, the reason on
# stderr.

p=$1 words=$2 f=/mnt/p1/boot/grub/grub.cfg

say()
{
    echo "bootwords: $*" >&2
}

# put: give the menu its words, or fail.
put()
{
    if [ ! -e $f ] && [ -e $f.keep ]; then
        { rm -f $f.new && cp $f.keep $f.new && sync && cmp -s $f.keep $f.new &&
            mv $f.new $f && sync; } || return 1
        say "the menu, whose name a stop took while it was replaced, is put back from its kept copy"
    fi
    rm -f $f.new $f.keep && sync || return 1
    awk -v words="$words" '
        # drop: remove every whole word of the line that matches RE.
        function drop(re) {
            while (match($0, "[ \t]" re "([ \t]|$)"))
                $0 = substr($0, 1, RSTART - 1) " " substr($0, RSTART + RLENGTH)
        }
        /^[ \t]*(multiboot|multiboot2)[ \t]/ &&
        /[ \t]rach2\.slot=(candidate|fallback)([ \t]|$)/ {
            drop("ramoops\\.[^ \t]+")
            drop("netconsole=[^ \t]+")
            sub(/[ \t]+$/, "")
            $0 = $0 " " words
            roots++
        }
        { print }
        END { if (roots == 0) exit 1 }
    ' $f > $f.new || { say "no line of $f boots the root"; return 1; }
    if cmp -s $f $f.new; then
        rm -f $f.new && sync
        return
    fi
    sync && cp $f.new $f.keep && sync && cmp -s $f.new $f.keep &&
        mv -f $f.new $f && sync && rm -f $f.keep && sync
}

[ -n "$p" ] && [ -n "$words" ] || { say "usage: sh -s BOOT WORDS"; exit 1; }
if ! mkdir -p /mnt/p1 || ! mount -t vfat "$p" /mnt/p1; then
    say "$p would not mount"
    exit 1
fi
put || { s=1; say "the menu on $p could not be given its words"; }
umount /mnt/p1 || { s=1; say "$p would not unmount"; }
[ "${s:-0}" -ne 0 ] || bootctl --target "$p" status > /dev/null ||
    { s=1; say "$p, its words given, is not a boot partition bootctl reads"; }
[ "${s:-0}" -ne 0 ] || echo "bootwords: $p boots the root with $words"
exit "${s:-0}"
