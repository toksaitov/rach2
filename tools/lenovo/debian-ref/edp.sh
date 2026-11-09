#!/bin/sh
# shellcheck disable=SC2086,SC2046 # no path or word list here has a space
# edp.sh: the panel's output as Linux's i915 leaves it lit, for 78's and 77's
# display work: IGT's register dump (every register its Haswell spec names),
# the raw MMIO bar and the display blocks by address, i915's own
# reports, the panel's EDID and DPCD, the VBT and the mode. Reads only; the
# panel is not touched. Output in $OUT/edp.
. /root/ref80/lib.sh
O=$OUT/edp; mkdir -p $O
D=/sys/kernel/debug/dri/0
say "edp: start"
if [ $REHEARSE = 1 ] || ! lspci -n | grep -q '8086:0a16'; then
	echo "edp: no Haswell GPU here (rehearsal); the dump runs on the Lenovo" | tee $O/skip.txt
	intel_reg --help > $O/intel_reg-help.txt 2>&1
	exit 0
fi
dpkg-query -W intel-gpu-tools > $O/version.txt
intel_reg dump > $O/intel_reg-dump.txt 2> $O/intel_reg-dump.err
# The whole MMIO bar, raw, and the display engine's blocks by address: the
# eDP transcoder (0x6f000), DDI A and its link training (0x64000), panel
# fitter A (0x68080), pipe A and its plane (0x70000, 0x70180), the CPU
# backlight (0x48250), clocks (0x46000, LCPLL at 0x130040), the power well
# (0x45400) and the PCH's panel power and backlight (0xc7200, 0xc8250).
intel_reg snapshot > $O/mmio-snapshot.bin 2> $O/mmio-snapshot.err
for r in 0x6f000:32 0x64000:32 0x68080:16 0x70000:16 0x70180:16 0x48250:4 \
	0x46000:64 0x130040:4 0x45400:8 0xc7200:8 0xc8250:4 0x44000:16 0x42000:4; do
	intel_reg read --count=${r#*:} ${r%:*} 2>&1
done > $O/intel_reg-ranges.txt
for f in i915_display_info i915_edp_psr_status i915_fbc_status i915_power_domain_info \
	i915_frequency_info i915_vbt i915_opregion i915_capabilities; do
	cat $D/$f > $O/$f 2>&1
done
cat $D/eDP-1/i915_dpcd > $O/eDP-1-dpcd.txt 2>&1
cat $D/eDP-1/i915_panel_timings > $O/eDP-1-panel-timings.txt 2>&1
intel_vbt_decode --file $D/i915_vbt > $O/vbt-decode.txt 2>&1
cat /sys/class/drm/card0-eDP-1/edid > $O/edid.bin
edid-decode $O/edid.bin > $O/edid.txt 2>&1
drm_info > $O/drm_info.txt 2>&1
say "edp: $(grep -c . $O/intel_reg-dump.txt) decoded registers, $(grep -c . $O/intel_reg-ranges.txt) raw, snapshot $(stat -c %s $O/mmio-snapshot.bin) bytes"
grep -i -E 'edp|ddi|pp_status|pipe' $O/intel_reg-dump.txt | head -12
