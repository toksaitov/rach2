#!/bin/sh
#
# g-sess.sh start|stop: Weston 14 on the guest's virtio-gpu (DRM backend, GL
# renderer, kiosk shell, Xwayland, --debug for the screenshot protocol)
# through seatd, as user gfx. The session's variables are in ~/sess.env;
# Weston's log is ~/out/weston.log.
#
set -u
r=/run/user/$(id -u)
case $1 in
start)
    sudo systemctl start seatd
    mkdir -p ~/out
    rm -f "$r"/wayland-* "$r"/.X* 2>/dev/null
    XDG_RUNTIME_DIR=$r LIBSEAT_BACKEND=seatd setsid -f weston --backend=drm \
        --renderer=gl --shell=kiosk --xwayland --idle-time=0 --debug \
        --log="$HOME/out/weston.log" </dev/null >/dev/null 2>&1
    i=0
    until [ -S "$r/wayland-1" ] || [ -S "$r/wayland-0" ]; do
        i=$((i + 1))
        [ $i -lt 30 ] || { echo "g-sess: no wayland socket"; tail -20 ~/out/weston.log; exit 1; }
        sleep 1
    done
    s=$(cd "$r" && ls wayland-? | head -1)
    # Xwayland starts on the first X client; its display number is in the log.
    printf 'export XDG_RUNTIME_DIR=%s WAYLAND_DISPLAY=%s DISPLAY=:0\n' "$r" "$s" >~/sess.env
    grep -iE 'renderer|GL version|GL renderer|dmabuf|xwayland' ~/out/weston.log | head -12
    ;;
stop)
    pkill -x weston
    sleep 1
    pkill -x Xwayland
    ;;
esac
