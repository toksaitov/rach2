#!/bin/sh
# Read the Lenovo's pipe A and eDP sink before opening the CRC reader.
# Run only while i915 owns the device and the panel's pipe is active.
set -eu

gpu=/sys/bus/pci/devices/0000:00:02.0
[ "$(cat "$gpu/device")" = 0x0a16 ]
[ "$(basename "$(readlink "$gpu/driver")")" = i915 ]

cat /proc/uptime
# Compressor, source PSR, display error status, watermarks, pipe source,
# primary plane, power sequencer and PWM.
regs=$(intel_reg --pci-slot 0000:00:02.0 read \
    0x43208 0x43210 0x64800 0x64840 0x44040 0x45100 0x45108 0x4510c 0x45110 \
    0x6001c 0x70180 0x70188 0x7019c 0x701a4 0x701ac \
    0xc7200 0xc7204 0xc8250 0xc8254)
printf '%s\n' "$regs"

# Find the pages of an enabled linear primary with no plane offset.
reg()
{
    printf '%s\n' "$regs" | awk -v name="($1):" '$(NF - 1) == name { print $NF }'
}
ctl=$(reg 0x00070180)
if [ "$((ctl & 0x80000400))" -eq "$((0x80000000))" ] &&
    [ "$(reg 0x000701a4)" = 0x00000000 ]; then
    size=$(reg 0x0006001c)
    stride=$(reg 0x00070188)
    surface=$(reg 0x000701ac)
    rows=$(((size & 0xffff) + 1))
    pages=$(((stride * rows + 4095) / 4096))
    first=$(((surface & 0xfffff000) / 4096))
    if [ "$pages" -gt 0 ] && [ "$((first + pages))" -le "$((0x80000))" ]; then
        pte=$(printf '0x%x' "$((0x200000 + 4 * first))")
        printf 'scanout: live surface ggtt: %d entries from %s\n' "$pages" "$pte"
        intel_reg --pci-slot 0000:00:02.0 --count="$pages" read "$pte"
    fi
fi

# Read only DPCD's defined bytes: PSR support/config and link status.
for range in 112:2 368:1 514:6; do
    offset=${range%:*}
    count=${range#*:}
    bytes=$(dd if=/dev/drm_dp_aux0 iflag=skip_bytes bs="$count" skip="$offset" count=1 status=none |
        od -An -tx1)
    printf 'scanout: dpcd 0x%03x:%s\n' "$offset" "$bytes"
    # shellcheck disable=SC2086 # od separates every byte with whitespace
    set -- $bytes
    [ "$#" -eq "$count" ] || printf 'scanout: incomplete dpcd read at 0x%03x\n' "$offset" >&2
done
