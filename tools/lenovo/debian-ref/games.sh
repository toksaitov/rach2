#!/bin/sh
# shellcheck disable=SC2086,SC2046 # no path or word list here has a space
# games.sh stk|0ad|record NAME: the native games' fixed replays under cage,
# fullscreen on the panel, frame times from Gallium's HUD dump (frames.py),
# GPU clock and energy each second, no sound driver.
#   stk NAME     SuperTuxKart's profile mode: 4 AI karts on hacienda for 90 s,
#                seed 1, 1366x768 fullscreen; it exits by itself and prints
#                its own frame count.
#   0ad NAME     0 A.D.'s replay replays/0ad-mainland/commands.txt played by
#                -replay-visual for LOAD + 150 s, then ended.
#   record NAME  makes that replay: an AI-against-AI game on mainland, seeds
#                fixed, run non-visually for 240 s of wall time.
# Output in $OUT/games/NAME.
set -u
. /root/ref80/lib.sh
need cage python3
K=$1; N=$2; O=$OUT/games/$N
rm -rf $O; mkdir -p $O/hud; chown -R gfx:gfx $OUT/games
pgrep -u gfx -x cage > /dev/null && { echo "games: cage is running" >&2; exit 1; }
W=1366; H=768; SPAN=150; STK_T=90
[ $REHEARSE = 1 ] && { W=640; H=360; SPAN=20; STK_T=15; }
CE=$(cage_env)
# run TIMEOUT CMD...: CMD under cage as gfx, its output in $O/game.out.
run() {
	t=$1; shift
	sampler_start $O/samples.txt
	say "games: start $K $N: $*" | tee $O/run.log
	# shellcheck disable=SC2086
	timeout -k 20 -s INT "$t" runuser -u gfx -- env HOME=/home/gfx XDG_RUNTIME_DIR=$RUN $CE \
		cage -- env SDL_VIDEODRIVER=$SDLV SDL_VIDEO_WAYLAND_ALLOW_LIBDECOR=0 $(hud_env $O/hud) "$@" > $O/game.out 2>&1 < /dev/null
	echo "exit $?" >> $O/run.log
	sampler_stop
	sleep 2; pkill -u gfx -x cage; pkill -9 -u gfx -f pyrogenesis; pkill -9 -u gfx -x supertuxkart
	say "games: end $K $N" | tee -a $O/run.log
}
install -d -o gfx -g gfx -m 700 $RUN
# Debian installs both games in /usr/games, which root's PATH leaves out.
export PATH=$PATH:/usr/games
# SDL 2.32 on Wayland under cage 0.2.0 decorates its window through
# libdecor's GTK plugin, and both games then show nothing and wait in poll
# before making a GL context; without libdecor they draw. SDLV=x11 runs a
# game on cage's Xwayland instead.
SDLV=${SDLV:-wayland}
case $K in
stk)
	run $((STK_T + 120)) supertuxkart --profile-time=$STK_T --track=hacienda --numkarts=4 \
		--seed=1 --fullscreen --screensize=${W}x$H
	cp /home/gfx/.config/supertuxkart/config-0.10/config.xml $O/ 2>/dev/null
	grep -E -i 'frames|frame rate|fps' $O/game.out | tail -5 | tee $O/stk-own.txt
	;;
0ad)
	C=$R80/replays/0ad-mainland/commands.txt
	[ -s $C ] || { echo "games: no replay $C" >&2; exit 1; }
	# gfx cannot read root's home; the game reads its own copy.
	cp $C $O/commands.txt; chown gfx:gfx $O/commands.txt
	sha256sum $C > $O/replay.sha256
	run $((SPAN + 90)) 0ad -replay-visual=$O/commands.txt -conf=windowed:false -conf=xres:$W -conf=yres:$H -conf=sound.mastergain:0
	cp /home/gfx/.config/0ad/logs/mainlog.html $O/ 2>/dev/null
	;;
record)
	D=/home/gfx/.local/share/0ad/replays
	rm -rf $D
	sampler_start $O/samples.txt
	say "games: record $N" | tee $O/run.log
	timeout -s TERM 240 runuser -u gfx -- env HOME=/home/gfx 0ad -autostart="random/mainland" \
		-autostart-seed=1 -autostart-aiseed=1 -autostart-players=2 -autostart-size=192 \
		-autostart-civ=1:athen -autostart-civ=2:brit -autostart-ai=1:petra -autostart-ai=2:petra \
		-autostart-player=-1 -autostart-victory=endless -autostart-nonvisual > $O/game.out 2>&1
	echo "exit $?" >> $O/run.log
	sampler_stop
	f=$(find $D -name commands.txt | head -1)
	[ -n "$f" ] || { echo "games: no replay written" >&2; exit 1; }
	mkdir -p $R80/replays/0ad-mainland
	cp "$(dirname "$f")"/* $R80/replays/0ad-mainland/
	cp "$(dirname "$f")"/* $O/
	echo "replay: $(grep -c '^turn' "$f") turns, $(sha256sum "$f" | cut -c1-16)" | tee -a $O/run.log
	ls -l $R80/replays/0ad-mainland >> $O/run.log
	exit 0
	;;
*) echo "games: unknown game $K" >&2; exit 1 ;;
esac
python3 -I $R80/frames.py "$O/hud/frametime_(ms)" 5 $([ $K = 0ad ] && echo $SPAN) | tee $O/frames.txt
