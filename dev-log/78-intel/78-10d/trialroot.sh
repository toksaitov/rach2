#!/bin/sh
# trialroot.sh IMAGE WORD...: stage on the running release, by hand, the
# menu block that `bootctl rootimage GPTID=...,6:IMAGE` writes, with the
# WORDs (78-10d's `rach2.i915`) after this boot's words, and arm it once
# with the branch's bootctl at /dev/shm/bootctl. The release's bootctl has
# no `rootimage`, and the branch's takes the words of the line it runs
# under, which has no driver. 78-5c's copy of this script gives the words
# `rach2.i915 rach2.i915.disable_display` itself.
#
# The words kept are those utils/bootctl.c's trial_drops keeps: every word
# but a path, rach2.root=, rach2.rootfstype=, rach2.rootimage=, rach2.slot=,
# init=, rach2.fsdisk, rach2.fswindow and rach2.rescue. The block is
# write_trial_menu's text, with the boot partition's serial 1ACE-F00D. It
# prints the block, `bootctl config` stages it in place of the menu, and
# `bootctl trial trialroot` arms it; the next boot takes it once and the one
# after it the saved default.
set -eu

[ $# -ge 2 ] || { echo "usage: trialroot.sh /PATH WORD..." >&2; exit 2; }
spec=GPTID=1acef00d-0f1e-4d2c-8b3a-c0ffee000001,6:$1
shift
serial=1ACE-F00D
cfg=/dev/shm/trialroot.cfg

[ -x /dev/shm/bootctl ] || { echo "trialroot: no /dev/shm/bootctl" >&2; exit 1; }
tokens=
for word in $(cat /proc/cmdline); do
    case $word in
        /* | rach2.root=* | rach2.rootfstype=* | rach2.rootimage=* | rach2.slot=* | init=*) ;;
        rach2.fsdisk | rach2.fswindow | rach2.rescue) ;;
        *) tokens="$tokens $word" ;;
    esac
done
tokens="$tokens $*"
line="rach2.rootimage=$spec rach2.slot=trialroot$tokens"
[ ${#line} -le 510 ] || { echo "trialroot: the line is ${#line} bytes, past 510" >&2; exit 1; }

{
    printf '# bootctl rootimage: one boot of a root image\n'
    printf 'menuentry "Start Rach2 once from the root image %s" --id trialroot {\n' "$spec"
    printf '    search --no-floppy --fs-uuid --set=root %s\n' "$serial"
    printf '    multiboot2 /boot/kernel %s\n' "$line"
    printf '    if [ "$grub_platform" = pc ]; then set gfxpayload=auto; fi\n'
    printf '    boot\n'
    printf '}\n'
    printf 'if [ "${next_entry}" = "trialroot" ]; then\n'
    printf '    set next_entry=\n'
    printf '    save_env next_entry\n'
    printf '    search --no-floppy --fs-uuid --set=root %s\n' "$serial"
    printf '    multiboot2 /boot/kernel %s\n' "$line"
    printf '    if [ "$grub_platform" = pc ]; then set gfxpayload=auto; fi\n'
    printf '    boot\n'
    printf 'fi\n'
    printf '# bootctl rootimage: end\n'
} > "$cfg"
cat "$cfg"
bootctl config "$cfg"
/dev/shm/bootctl trial trialroot
/dev/shm/bootctl status
