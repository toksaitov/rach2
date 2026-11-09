#!/bin/sh
# shellcheck disable=SC2086,SC2046 # no path or word list here has a space
# crysis.sh: Crysis (64-bit, Direct3D 9) under GE-Proton11-12 with WoW64 and
# WineD3D on GL, under cage fullscreen at Low 1366x768, sound on into ALSA's
# silent snd-dummy card (s_SoundEnable = 1), by two routes:
#   direct  Proton's own script, outside any container, as run2 step 9 ran it;
#   umu     umu-launcher 1.4.4 in the Steam Linux Runtime (sniper), the same
#           GE-Proton, GAMEID 0 and protonfixes off, so that the container is
#           the only difference.
# Both use step 9's prefix (/srv/run2/step9/compat), overlay and profile.
#   prep                 the overlay, snd-dummy, umu and its runtime
#   flyby ROUTE NAME     the island flyby (benchmark_gpu, 4 loops), frame
#                        times from Gallium's HUD dump; ended after loop 4
#   start ROUTE NAME [ARGS...]  the game in the background, -DEVMODE, for
#                        the suit's reboot played by uinput-play.py
#   hud NAME             on a game `start` left at the main menu, the
#                        suit's reboot from save 07, the HUD read back
#   stop                 ends the game and cage
# Output in $OUT/crysis/NAME.
set -u
. /root/ref80/lib.sh
S=/srv/run2/step9; P=$S/GE-Proton11-12-x86_64; G=$S/crysis
U="$S/compat/pfx/drive_c/users/steamuser/Documents/My Games/Crysis"
UMU=$S80/umu/umu-run
B=$G/Game/Levels/island/benchmark_gpu.log
if [ $REHEARSE = 1 ] || [ ! -d /srv/crysis/game ]; then
	echo "crysis: no game here (rehearsal); the routes run on the Lenovo"
	[ "${1:-}" = prep ] && { mkdir -p $S80/umu; tar xf $C80/umu-launcher-1.4.4-zipapp.tar -C $S80; python3 $UMU --help 2>&1 | head -3; }
	exit 0
fi
# settings NAME: Low 1366x768 fullscreen, sound on, in the root's system.cfg
# and the profile's files, which override it (step 9).
settings() {
	cat > $G/system.cfg <<C
sys_spec = 1
r_Width = 1366
r_Height = 768
r_Fullscreen = 1
r_vsync = 0
s_SoundEnable = 1
C
	chown gfx:gfx $G/system.cfg
	sed -i -E "s/^(sys_spec_[A-Za-z]+) = .*/\1 = 1/; s/^r_Width = .*/r_Width = 1366/; s/^r_Height = .*/r_Height = 768/; s/^r_Fullscreen = .*/r_Fullscreen = 1/; s/^s_SoundEnable = .*/s_SoundEnable = 1/" "$U/game.cfg"
	sed -i -E "s/(name=\"OptionCfg.sys_spec_[A-Za-z]+\" value=\")[0-9]+/\11/" "$U/Profiles/default/attributes.xml"
	cp $G/system.cfg "$U/game.cfg" $1/
}
# launch ROUTE O ENV ARGS...: the command line for ROUTE, run under cage.
# Root's session bus does not reach gfx, and bwrap refuses to bind it.
# umu's Proton maps drive S: to the game's library, which umu takes as the
# directory above STEAM_COMPAT_INSTALL_PATH, by default the program's own
# (Bin64); Crysis run from the root of S: finds no language, so the install
# path is the game's directory, as Steam sets it.
launch() {
	r=$1; o=$2; e=$3; shift 3
	cd $G/Bin64 || exit 1
	case $r in
	direct)
		# shellcheck disable=SC2086
		runuser -u gfx -- env -u DBUS_SESSION_BUS_ADDRESS HOME=/home/gfx XDG_RUNTIME_DIR=$RUN LIBSEAT_BACKEND=seatd \
			cage -- env STEAM_COMPAT_DATA_PATH=$S/compat STEAM_COMPAT_CLIENT_INSTALL_PATH=$S/steam \
			PROTON_USE_WOW64=1 PROTON_USE_WINED3D=1 WINE_D3D_CONFIG=renderer=gl $e \
			WINEDEBUG=+timestamp,+pid,+loaddll,+d3d_caps,fixme-all \
			$P/proton run ./Crysis.exe -dx9 "$@" > $o/wine.out 2>&1 < /dev/null ;;
	umu)
		# shellcheck disable=SC2086
		runuser -u gfx -- env -u DBUS_SESSION_BUS_ADDRESS HOME=/home/gfx XDG_RUNTIME_DIR=$RUN LIBSEAT_BACKEND=seatd \
			cage -- env WINEPREFIX=$S/compat GAMEID=0 STORE=none PROTONPATH=$P STEAM_COMPAT_INSTALL_PATH=$G \
			PROTONFIXES_DISABLE=1 UMU_RUNTIME_UPDATE=0 PRESSURE_VESSEL_FILESYSTEMS_RW=/srv \
			UMU_LOG=debug PROTON_USE_WOW64=1 PROTON_USE_WINED3D=1 WINE_D3D_CONFIG=renderer=gl $e \
			WINEDEBUG=+timestamp,+pid,+loaddll,+d3d_caps,fixme-all \
			python3 $UMU ./Crysis.exe -dx9 "$@" > $o/wine.out 2>&1 < /dev/null ;;
	*) echo "crysis: unknown route $r" >&2; return 1 ;;
	esac
}
endgame() {
	runuser -u gfx -- env WINEPREFIX=$S/compat/pfx $P/files/bin/wineserver -k 2>/dev/null
	sleep 8; pkill -u gfx -f 'Crysis.exe'; sleep 2; pkill -u gfx -x cage; sleep 2
	pkill -9 -u gfx -f 'Crysis.exe|wineserver|pressure-vessel|pv-adverb|xalia'
}
case ${1:-} in
prep)
	mkdir -p $S/crysis-upper $S/crysis-work $G
	mountpoint -q $G || mount -t overlay overlay -o lowerdir=/srv/crysis/game,upperdir=$S/crysis-upper,workdir=$S/crysis-work,metacopy=on,redirect_dir=on $G
	chown -R gfx:gfx $G
	mount | grep " $G "
	# udev sets the new card's nodes to root:audio 0660 after modprobe
	# returns; the chmod waits for it.
	modprobe snd-dummy && udevadm settle && chmod o+rw /dev/snd/*
	aplay -l 2>&1 | head -5
	mkdir -p $S80/umu; tar xf $C80/umu-launcher-1.4.4-zipapp.tar -C $S80; chmod -R a+rX $S80/umu
	# The first start fetches the runtime once; the runs then set
	# UMU_RUNTIME_UPDATE=0 so that nothing is fetched again.
	say "crysis: umu's runtime"
	# umu resolves the program's name against the working directory, which
	# gfx must be able to read.
	cd /tmp || exit 1
	runuser -u gfx -- env -u DBUS_SESSION_BUS_ADDRESS HOME=/home/gfx XDG_RUNTIME_DIR=$RUN WINEPREFIX=$S/compat GAMEID=0 STORE=none \
		PROTONPATH=$P PROTONFIXES_DISABLE=1 PRESSURE_VESSEL_FILESYSTEMS_RW=/srv \
		python3 $UMU wineboot.exe --version 2>&1 | tail -5
	du -sh /home/gfx/.local/share/umu 2>/dev/null
	find /home/gfx/.local/share/umu -maxdepth 2 -name VERSIONS.txt -exec head -5 {} \; 2>/dev/null
	find /home/gfx/.local/share/umu -maxdepth 3 -name 'BuildID.txt' -exec cat {} \; 2>/dev/null
	;;
flyby)
	r=$2; n=$3; O=$OUT/crysis/$n
	pgrep -u gfx -x cage > /dev/null && { echo "crysis: cage is running" >&2; exit 1; }
	rm -rf $O; mkdir -p $O/hud; chown -R gfx:gfx $OUT/crysis
	settings $O
	rm -f $B $G/Game.log
	sampler_start $O/samples.txt
	say "crysis: flyby $r $n" | tee $O/run.log
	# Record the dump's length as each loop ends; end the game after loop 4.
	( k=0; i=0; while [ $i -lt 3000 ]; do sleep 0.5; i=$((i + 1))
		c=$(grep -c "Finished" $B 2>/dev/null); c=${c:-0}
		if [ "$c" -gt "$k" ]; then k=$c
			echo "$(cut -d' ' -f1 /proc/uptime) run$((k - 1)) $(wc -l < "$O/hud/frametime_(ms)" 2>/dev/null)" >> $O/frames.txt
			[ $k -ge 4 ] && { sleep 2; break; }
		fi
	done; say "crysis: ending the game" >> $O/run.log; endgame ) &
	WCH=$!
	# A bound on the whole run, should the log never show loop 4.
	( i=0; while [ $i -lt 300 ]; do sleep 5; i=$((i + 1)); done
	  say "crysis: 1500 s bound reached" >> $O/run.log; endgame ) &
	GRD=$!
	launch $r $O "$(hud_env $O/hud)" -DEVMODE +map island +exec benchmark_gpu
	echo "exit $?" >> $O/run.log
	sampler_stop; kill $WCH $GRD 2>/dev/null; endgame
	cp $B $G/Game.log $O/ 2>/dev/null
	say "crysis: end $n" | tee -a $O/run.log
	grep -E 'Run|Average|Min|Max|Play Time' $O/benchmark_gpu.log 2>/dev/null | head -24
	python3 -I $R80/crysis-frames.py $O
	;;
start)
	r=$2; n=$3; shift 3; O=$OUT/crysis/$n
	rm -rf $O; mkdir -p $O; chown -R gfx:gfx $OUT/crysis
	settings $O
	rm -f $G/Game.log
	say "crysis: start $r $n: $*" | tee $O/run.log
	( launch $r $O "" -DEVMODE "$@"; echo "exit $?" >> $O/run.log ) &
	;;
hud)
	# On a game that `start` left at the main menu: load the owner's save 07
	# (row 3 of LOAD GAME, by date) through the menu, turn the dialog log on,
	# enter the suit's reboot trigger, and read back the HUD 35 s later: a
	# screenshot, the game's Flash profiler, the dialog's line times and the
	# hudShow flag of a save made then (run2 step 9's trial.sh).
	n=$2; O=$OUT/crysis/$n; Y="python3 -I $R80/uinput-play.py"; V="$U/SaveGames"
	mkdir -p $O
	$Y at:240,275 wait:0.3 click:BTN_LEFT wait:2 at:420,413 wait:0.3 click:BTN_LEFT wait:2 \
		at:936,348 wait:0.3 click:BTN_LEFT wait:1 shot:$O/list.png \
		at:938,595 wait:0.3 click:BTN_LEFT wait:1.5 shot:$O/confirm.png \
		at:566,421 wait:0.3 click:BTN_LEFT wait:45 shot:$O/loaded.png \
		tap:KEY_GRAVE wait:1 type:"log_verbosity 4" tap:KEY_ENTER wait:0.5 \
		type:"ds_loglevel 4" tap:KEY_ENTER wait:0.5 \
		type:"gotoe suit_menu_and_objectives" tap:KEY_ENTER tap:KEY_GRAVE wait:35 shot:$O/after.png \
		tap:KEY_GRAVE wait:1 type:"sys_flash_info 1" tap:KEY_ENTER tap:KEY_GRAVE wait:2 shot:$O/flash.png \
		tap:KEY_GRAVE wait:1 type:"sys_flash_info 0" tap:KEY_ENTER wait:0.5 \
		type:"save hud_$n" tap:KEY_ENTER wait:4 tap:KEY_GRAVE wait:1 > $O/uinput.log 2>&1
	cp $G/Game.log $O/
	l1=$(grep -E "SID=[0-9]+ Playing new line 1:" $O/Game.log | tail -1 | sed 's/.*now=//')
	l2=$(grep -E "SID=[0-9]+ Playing new line 2:" $O/Game.log | tail -1 | sed 's/.*now=//')
	h=$(grep -o -E '<Extension name="GameRules"[^>]*>' "$V/default_hud_$n.xml" | grep -o -E 'hudShow="[0-9]"|hudBroken="[0-9]"' | tr '\n' ' ')
	mv "$V/default_hud_$n.xml" $O/ 2>/dev/null; rm -f "$V/default_hud_$n.CRYSISJMSF"
	echo "crysis: hud $n: line 1 at ${l1:-?} s, line 2 at ${l2:-?} s, GameRules: ${h:-hudShow and hudBroken absent}" | tee -a $O/run.log
	python3 -I -c 'import sys; a, b = float(sys.argv[1]), float(sys.argv[2]); print("crysis: Show after BootSeq: %.0f ms" % ((b + 4 - a - 6) * 1000))' "${l1:-0}" "${l2:-0}" | tee -a $O/run.log
	;;
stop)
	endgame; pgrep -u gfx -l || echo "crysis: nothing runs as gfx"
	;;
*) echo "usage: crysis.sh prep|flyby ROUTE NAME|start ROUTE NAME [ARGS]|hud NAME|stop" >&2; exit 1 ;;
esac
