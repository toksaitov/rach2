#!/bin/sh
# game.sh: on the trial root, as root, start Crysis as test on the session
# that session.sh started, detached, with the settings of Linux's reference
# on this machine (tools/lenovo/debian-ref/crysis.sh): Low, 1366x768,
# fullscreen, no vsync, sound on, and Gallium's frame times dumped unseen.
#
# Usage: game.sh [ARGUMENT...]
#
# The arguments follow `-devmode` on the game's command line. The output is
# /tmp/80-12/game.out and the frame times /tmp/80-12/hud/frametime_(ms).
set -eu
log=/tmp/80-12
game=/data/crysis/game
saves=/data/crysis/saves
display=$(sed -n 's/.*xserver listening on display \(:[0-9]*\).*/\1/p' \
    "$log/weston.log" | head -n 1)
[ -n "$display" ] || { echo 'game: no X display in weston.log' >&2; exit 1; }

cat > "$game/system.cfg" <<'CFG'
sys_spec = 1
r_Width = 1366
r_Height = 768
r_Fullscreen = 1
r_vsync = 0
s_SoundEnable = 1
CFG
sed -i -E -e 's/^(sys_spec_[A-Za-z]+) = .*/\1 = 1/' \
    -e 's/^r_Width = .*/r_Width = 1366/' -e 's/^r_Height = .*/r_Height = 768/' \
    -e 's/^r_Fullscreen = .*/r_Fullscreen = 1/' "$saves/game.cfg"
sed -i -E 's/(name="OptionCfg.sys_spec_[A-Za-z]+" value=")[0-9]+/\11/' \
    "$saves/Profiles/default/attributes.xml"
chown test:test "$game/system.cfg" "$saves/game.cfg" \
    "$saves/Profiles/default/attributes.xml"

rm -rf "$log/hud" "$log/game.out"
install -d -o test -g test "$log/hud"
args=
for word in "$@"; do args="$args '$word'"; done
setsid nohup su -s /bin/sh test -c "env XDG_RUNTIME_DIR=/run/user/1000 \
PULSE_SERVER=unix:/run/user/1000/pulse/native DISPLAY=$display \
GALLIUM_HUD=frametime GALLIUM_HUD_VISIBLE=false GALLIUM_HUD_PERIOD=0 \
GALLIUM_HUD_DUMP_DIR=$log/hud PROTON_USE_XALIA=0 \
MESA_SHADER_CACHE_SHOW_STATS=true \
WINEDEBUG=+timestamp,+pid,+loaddll,+d3d_caps,fixme-all \
crysis.sh run -devmode$args" > "$log/game.out" 2>&1 < /dev/null &
echo "game: started on $display: -devmode$args"
date
