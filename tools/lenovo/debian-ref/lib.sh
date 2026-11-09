# shellcheck shell=sh disable=SC2034,SC2086 # sourced; no path has a space
# lib.sh: what the 80-2 session's scripts share on the Lenovo's Debian;
# sourced by each of them, never run.

R80=/root/ref80			# the scripts, copied by mac.sh push
S80=/srv/ref80			# caches, work trees and outputs
C80=$S80/cache			# the pinned sources mac.sh push copies
OUT=$S80/out			# one directory per step, fetched by mac.sh fetch
RUN=/run/user/1000		# gfx's runtime directory
GPU=/sys/class/drm/card0
RAPL=/sys/class/powercap/intel-rapl:0
# The rehearsal guest under QEMU has no GPU, no KVM and no game; mac.sh
# rehearse leaves this file there, and the steps that need those say so and
# skip instead of failing.
REHEARSE=0
[ -e $R80/rehearse ] && REHEARSE=1

say() { echo "$(date -u +%T) $*"; }

# need CMD...: each command exists, or the step stops with a diagnostic.
need() {
	for c; do command -v "$c" > /dev/null || { echo "ref80: missing: $c" >&2; exit 1; }; done
}

# gfx CMD...: run CMD as gfx with its home and runtime directory.
gfx() {
	install -d -o gfx -g gfx -m 700 $RUN
	runuser -u gfx -- env HOME=/home/gfx XDG_RUNTIME_DIR=$RUN "$@"
}

# sampler_start FILE: each second, the uptime, the GPU's actual clock and the
# package and uncore energy counters (microjoules) into FILE; sampler_stop
# ends it.
sampler_start() {
	( while :; do
		echo "$(cut -d' ' -f1 /proc/uptime) $(cat $GPU/gt_act_freq_mhz 2>/dev/null || echo -) $(cat $RAPL/energy_uj 2>/dev/null || echo -) $(cat $RAPL:1/energy_uj 2>/dev/null || echo -)"
		sleep 1
	done > "$1" ) &
	SAMPLER=$!
}
sampler_stop() { [ -n "${SAMPLER:-}" ] && kill $SAMPLER 2>/dev/null; SAMPLER=; }

# cage_env: the environment cage needs. On the Lenovo it takes the panel
# through seatd; in the rehearsal it draws into a headless output with the
# pixman renderer, and clients fall back to llvmpipe.
cage_env() {
	if [ $REHEARSE = 1 ]; then
		echo "LIBSEAT_BACKEND=noop WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1"
	else
		echo "LIBSEAT_BACKEND=seatd"
	fi
}

# hud_env DIR: Gallium's HUD dumps each frame's time into DIR without
# drawing; the same Mesa reading Rach2's runs take (Crocus on the Lenovo,
# llvmpipe in the rehearsal).
hud_env() {
	echo "GALLIUM_HUD=frametime GALLIUM_HUD_VISIBLE=false GALLIUM_HUD_PERIOD=0 GALLIUM_HUD_DUMP_DIR=$1"
}

# quiet_check: nothing of anyone else's draws or runs as gfx, and the
# backlight is where the setup put it. Prints what it finds.
quiet_check() {
	echo "gfx processes: $(pgrep -u gfx -l | tr '\n' ' ')"
	echo "backlight: $(cat /sys/class/backlight/intel_backlight/brightness 2>/dev/null || echo -)"
	echo "sound modules: $(lsmod | awk '/^snd/ {print $1}' | tr '\n' ' ')"
}
