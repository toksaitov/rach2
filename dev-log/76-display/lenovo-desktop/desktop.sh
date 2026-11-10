#!/bin/bash
# desktop.sh: 76-17b's step on the Lenovo's trial root: the desktop session's
# state, the browsers' video on the panel against 80-2's Debian numbers, and
# the console after Weston's death. Run as root from the unpacked bundle.
#
#   desktop.sh state OUT        the boot, fbdrm, the session and Weston's output
#   desktop.sh clips WEB        80-2's two clips, read from Debian's p5
#   desktop.sh debian WEB OUT   80-2's path: each clip once, 58 s, both browsers
#   desktop.sh video CLIPS OUT  76-17a.4's six clips, both browsers, video.sh -d
#   desktop.sh kill OUT         Weston killed, then the console's state
#
# The bundle, the tree's root three directories above this script, holds tools/lenovo/debian-ref's
# video.html and probe-server.py and tests/graphics/runners as the tree has
# them, and debugfs, e2fsprogs' static build, beside this script or where
# DEBUGFS names it; debugfs reads p5's ext4, or the partition P5 names, and
# writes nothing there. Each
# browser runs as the session's user, `desktop`, on its Weston,
# wayland-0, as a program a person starts there would. Each command prints
# `desktop: result: ...` lines and keeps its evidence in OUT.
set -u

here=$(cd "$(dirname "$0")" && pwd)
bundle=$(cd "$here/../../.." && pwd)
ref=$bundle/tools/lenovo/debian-ref
runners=$bundle/tests/graphics/runners
ENV="export HOME=/home/desktop XDG_RUNTIME_DIR=/run/user/1001 WAYLAND_DISPLAY=wayland-0 \
PULSE_SERVER=unix:/run/user/1001/pulse/native XDG_SESSION_TYPE=wayland; "
CLIPS80="h264-1080p30.mp4 vp9-1080p30.webm"
SECS=58

die()
{
    echo "desktop: $*" >&2
    exit 1
}

# as_desktop LINE: run LINE in bash as the session's user, with its groups
# and the session's environment.
as_desktop()
{
    su -s /bin/bash desktop -c "$ENV$1"
}

# westons: the process IDs of the running compositors.
westons()
{
    for p in /proc/[0-9]*; do
        [ "$(cat "$p/comm" 2>/dev/null)" = weston ] && echo "${p#/proc/}"
    done
}

# outdir DIR: make DIR, writable by the session's user.
outdir()
{
    { mkdir -p "$1" && chown desktop:desktop "$1"; } || die "cannot make $1"
}

cmd_state()
{
    out=$1
    outdir "$out"
    {
        echo "== cmdline"; cat /proc/cmdline
        echo "== bootctl"; bootctl status
        echo "== root"; grep -E ' / | /data ' /proc/mounts
        echo "== drm"
        for c in /sys/class/drm/card*-*; do
            [ -d "$c" ] || continue
            echo "$c status $(cat "$c/status" 2>/dev/null) enabled $(cat "$c/enabled" 2>/dev/null) modes $(tr '\n' ' ' < "$c/modes" 2>/dev/null)"
        done
        ls /dev/dri 2>&1
        echo "== kernel"; dmesg | grep -E '(fbdrm|drm|console|i915|vtd|synaptics|psmouse|ps2mouse|input):'
        echo "== session"; cat /run/rach2-desktop.pid 2>&1
        ps -e -o pid,user,args | grep -E '[w]eston|[s]eatd|[u]devd|[p]ulseaudio|[r]ach2-'
        echo "== services"
        grep -a -i -E 'weston|drm|output|renderer|mode|head|backend|seat|current' /var/log/services | tail -n 120
    } > "$out/state.txt" 2>&1
    cp /var/log/services "$out/services.log"
    dmesg > "$out/dmesg.txt"
    w=$(westons | tr '\n' ' ')
    mode=$(grep -a -E '^ +[0-9]+x[0-9]+@.*current' /var/log/services | tail -n 1 | sed 's/^ *//')
    owner=$(for p in $w; do stat -c %U "/proc/$p"; done | sort -u | tr '\n' ' ')
    renderer=$(grep -a -o -E 'Using [A-Za-z]+ renderer' /var/log/services | tail -n 1)
    echo "desktop: result: weston ${w:-none}as ${owner:-nobody}; ${renderer:-no renderer}; mode ${mode:-none}"
}

cmd_clips()
{
    web=$1
    debugfs=${DEBUGFS:-$here/debugfs}
    [ -x "$debugfs" ] || die "no $debugfs"
    mkdir -p "$web"
    for c in $CLIPS80; do
        for dir in /srv/ref80/web /srv/run2/step9/web; do
            rm -f "$web/$c"
            "$debugfs" -R "dump $dir/$c $web/$c" "${P5:-/dev/disk0p5}" > /dev/null 2>&1
            [ -s "$web/$c" ] && break
        done
        [ -s "$web/$c" ] || die "no $c on p5"
        echo "desktop: result: $c from p5 $dir: $(wc -c < "$web/$c") bytes sha256 $(sha256sum "$web/$c" | cut -d' ' -f1)"
    done
    chown -R desktop:desktop "$web"
}

# play80 BROWSER CLIP WEB OUT: one play of 80-2's page in a new browser.
play80()
{
    b=$1 c=$2 web=$3 out=$4
    n=$b-${c%%-*}
    p=$out/profile-$n
    rm -rf "$p" "$out/$n.json"
    mkdir -p "$p"
    url="http://127.0.0.1:8000/video.html?name=$n&clip=$c&secs=$SECS"
    case $b in
        firefox)
            # 80-2's prefs: VA-API and hardware decoding off, as on Debian.
            cat > "$p/user.js" << 'EOF'
user_pref("browser.shell.checkDefaultBrowser", false);
user_pref("browser.startup.homepage_override.mstone", "ignore");
user_pref("browser.aboutwelcome.enabled", false);
user_pref("datareporting.policy.dataSubmissionEnabled", false);
user_pref("datareporting.healthreport.uploadEnabled", false);
user_pref("toolkit.telemetry.reportingpolicy.firstRun", false);
user_pref("app.update.auto", false);
user_pref("media.ffmpeg.vaapi.enabled", false);
user_pref("media.hardware-video-decoding.enabled", false);
EOF
            line="MOZ_ENABLE_WAYLAND=1 MOZ_LOG=PlatformDecoderModule:5 \
exec firefox-esr --new-instance --no-remote --profile $p --kiosk '$url'"
            ;;
        chromium)
            : > "$out/$n.cdp"
            chown desktop:desktop "$out/$n.cdp"
            line="exec python3 -I $runners/cdp.py $out/$n.cdp '$url' chromium --ozone-platform=wayland \
--kiosk --no-first-run --no-default-browser-check --password-store=basic --user-data-dir=$p"
            ;;
    esac
    chown -R desktop:desktop "$p"
    : > "$out/$n.log"
    chown desktop:desktop "$out/$n.log"
    head -n 1 /proc/stat > "$out/$n.stat0"
    grep -E '^bkl' /proc/stat >> "$out/$n.stat0"
    setsid su -s /bin/bash desktop -c "$ENV$line" < /dev/null >> "$out/$n.log" 2>&1 &
    pid=$!
    i=0
    while [ ! -s "$out/$n.json" ] && [ $i -lt $((SECS + 60)) ] && kill -0 $pid 2> /dev/null; do
        sleep 1
        i=$((i + 1))
    done
    head -n 1 /proc/stat > "$out/$n.stat1"
    grep -E '^bkl' /proc/stat >> "$out/$n.stat1"
    kill -TERM -$pid 2> /dev/null
    sleep 3
    kill -KILL -$pid 2> /dev/null
    wait $pid 2> /dev/null
    rm -rf "$p"
    if [ -s "$out/$n.json" ]; then
        python3 -I -c '
import json, sys
r = json.load(open(sys.argv[1]))
print("desktop: result: %s: %s frames, %s dropped, %s corrupted, %sx%s, %.1f s played, error %s, samples %s"
      % (r["name"], r.get("total"), r.get("dropped"), r.get("corrupted"), r.get("width"), r.get("height"),
         r.get("currentTime") or 0, r.get("error") or r.get("playError"),
         [(round(s["t"]), s["dropped"]) for s in r.get("samples", [])]))' "$out/$n.json"
    else
        echo "desktop: result: $n: no result after $i s"
    fi
    case $b in
        firefox) dec=$(grep -a -o 'has been initialized - description: [^,]*' "$out/$n.log" | sed 's/^.*description: //' | sort -u | tr '\n' ';') ;;
        chromium)
            dec=$(python3 -I -c '
import json, sys
names = []
for line in open(sys.argv[1], encoding="utf-8"):
    for p in json.loads(line)["params"].get("properties", []):
        if p.get("name") == "kVideoDecoderName" and p["value"] not in names:
            names.append(p["value"])
print(";".join(names))' "$out/$n.cdp" 2> /dev/null)
            ;;
    esac
    echo "desktop: result: $n decoder ${dec:-unknown}"
}

cmd_debian()
{
    web=$1 out=$2
    outdir "$out"
    for c in $CLIPS80; do
        [ -s "$web/$c" ] || die "no $web/$c; run clips first"
    done
    cp "$ref/video.html" "$web/"
    python3 -I "$ref/probe-server.py" "$web" "$out" 8000 > "$out/server.log" 2>&1 &
    srv=$!
    trap 'kill $srv 2> /dev/null' EXIT
    sleep 1
    kill -0 $srv 2> /dev/null || die "the server ended: $(tail -n 3 "$out/server.log")"
    chown desktop:desktop "$out"
    for b in firefox chromium; do
        for c in $CLIPS80; do
            play80 "$b" "$c" "$web" "$out" | tee -a "$out/results"
        done
    done
    kill $srv 2> /dev/null
    wait $srv 2> /dev/null
    trap - EXIT
}

cmd_video()
{
    clips=$1 out=$2
    outdir "$out"
    chown -R desktop:desktop "$clips"
    status=0
    for b in chromium firefox; do
        outdir "$out/$b"
        as_desktop "TMPDIR=/tmp sh $runners/video.sh -d -b $b -c $clips -o $out/$b \
h264-720p.mp4 h264-1080p.mp4 vp9-720p.webm vp9-1080p.webm av1-720p.webm av1-1080p.webm" \
            > "$out/$b.log" 2>&1 || status=1
        grep -a '^video: result:' "$out/$b.log" | sed 's/^video:/desktop: video:/'
    done
    echo "desktop: result: video status $status"
}

cmd_kill()
{
    out=$1
    outdir "$out"
    before=$(dmesg | wc -l)
    sup=$(cat /run/rach2-desktop.pid 2> /dev/null)
    w=$(westons | tr '\n' ' ')
    [ -n "$w" ] || die "no weston runs"
    # shellcheck disable=SC2086 # process IDs
    kill -KILL $w
    sleep 5
    left=$(westons | tr '\n' ' ')
    dmesg | tail -n +$((before + 1)) > "$out/dmesg-after-kill.txt"
    {
        echo "== killed $w; left ${left:-none}"
        echo "== supervisor $sup alive: $(kill -0 "$sup" 2> /dev/null && echo yes || echo no)"
        echo "== kernel"; cat "$out/dmesg-after-kill.txt"
        echo "== drm"
        for c in /sys/class/drm/card*-*; do
            [ -d "$c" ] && echo "$c enabled $(cat "$c/enabled" 2>/dev/null)"
        done
        echo "== term"; ps -e -o pid,args | grep '[t]erm'
    } > "$out/kill.txt" 2>&1
    cat "$out/kill.txt"
    echo "desktop: result: weston ${left:-gone}; $(grep -c -E 'console: .*(taken back|firmware)' "$out/dmesg-after-kill.txt") console lines"
}

[ "$(id -u)" = 0 ] || die "run as root"
cmd=${1:-}
[ $# -gt 0 ] && shift
case $cmd in
    state) [ $# -eq 1 ] || die "usage: desktop.sh state OUT"; cmd_state "$1" ;;
    clips) [ $# -eq 1 ] || die "usage: desktop.sh clips WEB"; cmd_clips "$1" ;;
    debian) [ $# -eq 2 ] || die "usage: desktop.sh debian WEB OUT"; cmd_debian "$1" "$2" ;;
    video) [ $# -eq 2 ] || die "usage: desktop.sh video CLIPS OUT"; cmd_video "$1" "$2" ;;
    kill) [ $# -eq 1 ] || die "usage: desktop.sh kill OUT"; cmd_kill "$1" ;;
    *) die "usage: desktop.sh state|clips|debian|video|kill ARG..." ;;
esac
