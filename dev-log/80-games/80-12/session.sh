#!/bin/bash
# session.sh: on 80-6's trial root, which has no desktop layer, start for the
# user test what the desktop session starts for desktop (vendor/gnu/desktop):
# udevd, seatd, PulseAudio on /dev/dsp and Weston's kiosk shell with Xwayland
# on the DRM device, composited by GL.
#
# Usage: session.sh start | user | stop
#
# `start`, as root, adds the session's groups with test in them, starts the
# device services and then the session as test, and waits for Weston's
# socket. `user` is the session itself, which `start` runs as test. `stop`
# ends all three. The logs are under /tmp/80-12; a file pulse.args there
# gives PulseAudio more arguments, such as -vvvv.
set -eu
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
log=/tmp/80-12
run=/run/user/1000

case ${1:-} in
start)
    mkdir -p "$log"
    chmod 1777 "$log"
    # Debian's numbers for the groups its udev rules name.
    for entry in seat:990 video:44 render:992 input:995 audio:29; do
        grep -q "^${entry%%:*}:" /etc/group ||
            echo "${entry%%:*}:x:${entry##*:}:test" >> /etc/group
    done
    mkdir -p /run/udev /run/user
    install -d -m 0700 -o test -g test "$run"
    install -d -m 1777 /tmp/.X11-unix /tmp/.ICE-unix
    setsid /usr/lib/systemd/systemd-udevd --resolve-names=early \
        > "$log/udevd.log" 2>&1 < /dev/null &
    echo $! > "$log/udevd.pid"
    until udevadm control --ping 2> /dev/null; do sleep 1; done
    udevadm trigger --action=add
    udevadm settle
    SEATD_VTBOUND=0 setsid seatd -g seat > "$log/seatd.log" 2>&1 < /dev/null &
    echo $! > "$log/seatd.pid"
    while [ ! -S /run/seatd.sock ]; do sleep 1; done
    ls -l /dev/dri /dev/input /run/seatd.sock
    setsid su -s /bin/bash test -c "exec $here/session.sh user" \
        > "$log/session.log" 2>&1 < /dev/null &
    echo $! > "$log/session.pid"
    i=0
    while [ ! -S "$run/wayland-0" ] && [ $i -lt 60 ]; do
        sleep 1
        i=$((i + 1))
    done
    [ -S "$run/wayland-0" ] || { cat "$log/session.log"; exit 1; }
    echo "session: weston: up after $i s"
    ;;
user)
    export HOME=/home/test USER=test LOGNAME=test
    export XDG_RUNTIME_DIR=$run
    export LIBSEAT_BACKEND=seatd SEATD_SOCK=/run/seatd.sock
    export WAYLAND_DISPLAY=wayland-0
    export PULSE_SERVER=unix:$run/pulse/native
    export XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=Weston
    if [ "${2:-}" != --bus ]; then
        unset DBUS_SESSION_BUS_ADDRESS
        exec dbus-run-session -- "$0" user --bus
    fi
    id
    # vendor/gnu/desktop/default.pa and weston.ini, which this root lacks.
    cat > "$log/rach2.pa" <<'PA'
.nofail
load-module module-oss device=/dev/dsp sink_name=oss_output source_name=oss_input mmap=0
load-module module-native-protocol-unix
set-default-sink oss_output
set-default-source oss_input
.fail
PA
    printf '[core]\nshell=kiosk-shell.so\nidle-time=0\nxwayland=true\n' \
        > "$log/weston.ini"
    more=
    [ ! -r "$log/pulse.args" ] || more=$(cat "$log/pulse.args")
    # shellcheck disable=SC2086 # the file holds a list of arguments
    pulseaudio -n --daemonize=no --use-pid-file=no --exit-idle-time=-1 \
        --disallow-exit --file="$log/rach2.pa" $more > "$log/pulse.log" 2>&1 &
    pulse=$!
    # --debug lets weston-screenshooter read the output.
    weston --backend=drm --renderer=gl --socket=wayland-0 --debug \
        --config="$log/weston.ini" > "$log/weston.log" 2>&1 &
    weston=$!
    trap 'kill $weston $pulse 2> /dev/null; wait' EXIT
    trap 'exit 1' HUP INT TERM
    wait -n "$pulse" "$weston"
    ;;
stop)
    for name in session seatd udevd; do
        [ -r "$log/$name.pid" ] || continue
        kill -TERM "-$(cat "$log/$name.pid")" 2> /dev/null || :
        rm -f "$log/$name.pid"
    done
    ;;
*)
    echo 'usage: session.sh start | user | stop' >&2
    exit 2
    ;;
esac
