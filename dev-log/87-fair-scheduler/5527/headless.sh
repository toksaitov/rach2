#!/bin/sh
# headless.sh USER RUNDIR: start a Weston of USER's on its headless backend
# with Pixman at 1366x768 and 60 Hz, the kiosk shell and no idle time, on
# RUNDIR/wayland-0, so that Rach2 and Linux are compared without their
# display drivers. Run as root; it ends any Weston first.
set -eu
u=$1 run=$2
pkill -x weston 2> /dev/null || true
sleep 1
mkdir -p "$run"
chown "$u" "$run"
chmod 700 "$run"
rm -f "$run/wayland-0" "$run/wayland-0.lock"
printf '[core]\nshell=kiosk-shell.so\nidle-time=0\nxwayland=false\nrequire-input=false\n' > /tmp/headless.ini
chmod 644 /tmp/headless.ini
setsid su -s /bin/sh "$u" -c "XDG_RUNTIME_DIR=$run exec weston --backend=headless --renderer=pixman \
--width=1366 --height=768 --refresh-rate=60000 --config=/tmp/headless.ini --socket=wayland-0 \
--log=/tmp/headless.log" < /dev/null > /dev/null 2>&1 &
i=0
until [ -S "$run/wayland-0" ] || [ $i -ge 30 ]; do sleep 1; i=$((i + 1)); done
grep -i -E 'renderer|headless|mode' /tmp/headless.log | head -n 5
