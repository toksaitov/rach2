#!/bin/bash
# play.sh BROWSER CLIP WEB OUT [SECS]: one play of 84-7.1's page on the
# desktop session's Weston, as desktop.sh's play80 runs it on the Lenovo, for
# SECS (58) seconds, the clip looped when LOOP is 1, and the kernel's
# counters around it: /proc/stat's processor times and big-lock lines, and
# debugfs's bkl/calls and bkl/profile, zeroed before and read after, the
# profile in its whole-kernel mode. WEB holds the clip, video.html and
# probe-server.py; OUT gets BROWSER-CLIP.{json,log,stat0,stat1,calls,profile}
# and the result line, and with THREADS naming threads.sh, each thread's run
# and wait before and after in BROWSER-CLIP.{threads0,threads1}, and with
# SAMPLER naming sampler.py, each second's in BROWSER-CLIP.samples. Run as
# root in the guest.
set -u

[ $# -ge 4 ] || { echo 'usage: play.sh BROWSER CLIP WEB OUT [SECS]' >&2; exit 2; }
b=$1 c=$2 web=$3 out=$4 secs=${5:-58}
ENV="export HOME=/home/desktop XDG_RUNTIME_DIR=/run/user/1001 WAYLAND_DISPLAY=wayland-0 \
XDG_SESSION_TYPE=wayland; "
n=$b-${c%%.*}
p=$out/profile-$n
srv=''
trap '[ -z "$srv" ] || kill "$srv" 2> /dev/null; rm -rf "$p"' EXIT
mkdir -p "$out" "$p"
chown desktop:desktop "$out"
grep -q ' /sys/kernel/debug ' /proc/mounts || mount -t debugfs none /sys/kernel/debug
page=video.html
if [ "${LOOP:-0}" = 1 ]; then
    sed 's/<video id="v" muted/<video id="v" loop muted/' "$web/video.html" > "$web/loop.html"
    page=loop.html
fi
python3 -I "$web/probe-server.py" "$web" "$out" 8000 > "$out/server.log" 2>&1 &
srv=$!
sleep 1
rm -f "$out/$n.json"
url="http://127.0.0.1:8000/$page?name=$n&clip=$c&secs=$secs"
case $b in
    firefox)
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
exec ${WRAP:-} firefox-esr --new-instance --no-remote --profile $p --kiosk '$url'"
        ;;
    chromium)
        line="exec ${WRAP:-} chromium --ozone-platform=wayland --kiosk --no-first-run \
--no-default-browser-check --password-store=basic --user-data-dir=$p '$url'"
        ;;
    *) echo "play: no browser $b" >&2; exit 2 ;;
esac
chown -R desktop:desktop "$p"
: > "$out/$n.log"
chown desktop:desktop "$out/$n.log"
echo 0 > /sys/kernel/debug/bkl/calls
echo "${PROFILE:-kernel}" > /sys/kernel/debug/bkl/profile
{ head -n 1 /proc/stat; grep -E '^bkl' /proc/stat; } > "$out/$n.stat0"
[ -z "${THREADS:-}" ] || sh "$THREADS" > "$out/$n.threads0"
smp=''
if [ -n "${SAMPLER:-}" ]; then
    python3 -I "$SAMPLER" "$out/$n.samples" &
    smp=$!
fi
setsid su -s /bin/bash desktop -c "$ENV$line" < /dev/null >> "$out/$n.log" 2>&1 &
pid=$!
i=0
while [ ! -s "$out/$n.json" ] && [ $i -lt $((secs + 60)) ] && kill -0 $pid 2> /dev/null; do
    sleep 1
    i=$((i + 1))
done
{ head -n 1 /proc/stat; grep -E '^bkl' /proc/stat; } > "$out/$n.stat1"
[ -z "${THREADS:-}" ] || sh "$THREADS" > "$out/$n.threads1"
[ -z "$smp" ] || kill "$smp"
cat /sys/kernel/debug/bkl/calls > "$out/$n.calls"
cat /sys/kernel/debug/bkl/profile > "$out/$n.profile"
kill -TERM -$pid 2> /dev/null
sleep 3
kill -KILL -$pid 2> /dev/null
wait $pid 2> /dev/null
if [ -s "$out/$n.json" ]; then
    python3 -I -c '
import json, sys
r = json.load(open(sys.argv[1]))
print("play: result: %s: %s frames, %s dropped, %.1f s, error %s, samples %s"
      % (r["name"], r.get("total"), r.get("dropped"), r.get("currentTime") or 0,
         r.get("error") or r.get("playError"),
         [(round(s["t"]), s["dropped"]) for s in r.get("samples", [])]))' "$out/$n.json"
else
    echo "play: result: $n: no result after $i s"
fi
