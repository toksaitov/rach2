#!/bin/sh
# candidate.sh WORD...: stage on the running release, by hand, a menu block
# that boots the candidate slot once with this boot's line and the WORDs
# after it (78-20's `netconsole=6665@192.168.0.106/,6666@MAC/`), for a kernel
# that `bootctl kernel` has just staged and armed.
#
# The block runs when `next_entry` is `candidate`, before the stub's own test
# of it, and consumes it as the stub does, so the boot after it takes the
# saved default and a hang is left by a power cycle. The release must have
# booted as the candidate, since its line is the candidate's; the words kept
# are every word of /proc/cmdline but the path GRUB puts first. The block
# boots /boot/kernel on the boot partition, serial 1ACE-F00D, as the stub's
# candidate entry does. It prints the block, and `bootctl config` stages it
# in place of the menu; `bootctl config` of a file with one comment line
# empties the menu afterwards.
set -eu

[ $# -ge 1 ] || { echo "usage: candidate.sh WORD..." >&2; exit 2; }
serial=1ACE-F00D
cfg=/dev/shm/candidate.cfg

line=
for word in $(cat /proc/cmdline); do
    case $word in
        /*) ;;
        *) line="$line $word" ;;
    esac
done
case " $line " in
    *" rach2.slot=candidate "*) ;;
    *) echo "candidate: this boot is not the candidate's: $line" >&2; exit 1 ;;
esac
line="${line# } $*"
[ ${#line} -le 510 ] || { echo "candidate: the line is ${#line} bytes, past 510" >&2; exit 1; }
bootctl env next_entry | grep -q 'candidate' ||
    { echo "candidate: no trial of the candidate is armed; run bootctl kernel first" >&2; exit 1; }

{
    printf '# candidate.sh: one boot of the candidate with more words\n'
    printf 'if [ "${next_entry}" = "candidate" ]; then\n'
    printf '    set next_entry=\n'
    printf '    save_env next_entry\n'
    printf '    search --no-floppy --fs-uuid --set=root %s\n' "$serial"
    printf '    multiboot2 /boot/kernel %s\n' "$line"
    printf '    if [ "$grub_platform" = pc ]; then set gfxpayload=auto; fi\n'
    printf '    boot\n'
    printf 'fi\n'
} > "$cfg"
cat "$cfg"
bootctl config "$cfg"
bootctl status
