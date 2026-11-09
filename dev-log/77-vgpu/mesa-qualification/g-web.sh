#!/bin/sh
#
# g-web.sh OUT: probe.html in Chromium (default switches, then with
# --ignore-gpu-blocklist --enable-unsafe-webgpu, then the same with ANGLE on
# Vulkan) and in Firefox ESR (WebGPU on), each fullscreen under the session of
# g-sess.sh with a new profile; each row's JSON is OUT/ROW.json. A browser
# that has posted nothing in 120 s is stopped and its row says so.
#
set -u
o=$1
. ~/sess.env
mkdir -p "$o"
d=$(cd "$(dirname "$0")" && pwd)

# row NAME COMMAND...: start the page server and the browser, wait for a result.
row() {
    n=$1
    shift
    rm -rf ~/.cache/mesa_shader_cache* ~/prof
    mkdir -p ~/prof
    python3 "$d/probe-server.py" 8077 "$o/$n.json" &
    srv=$!
    setsid "$@" >"$o/$n.log" 2>&1 </dev/null &
    br=$!
    i=0
    while kill -0 $srv 2>/dev/null && [ $i -lt 120 ]; do
        sleep 1
        i=$((i + 1))
    done
    kill -- -$br 2>/dev/null
    sleep 2
    kill -9 -- -$br 2>/dev/null
    kill $srv 2>/dev/null
    wait 2>/dev/null
    [ -s "$o/$n.json" ] || echo '{"none": "no result in 120 s"}' >"$o/$n.json"
    printf '%s %s\n' "$n" "$(cat "$o/$n.json")"
}

C="chromium --ozone-platform=wayland --no-first-run --no-default-browser-check
 --password-store=basic --user-data-dir=$HOME/prof --start-fullscreen
 --enable-logging=stderr"
U=http://127.0.0.1:8077/probe.html
row chromium-default $C $U
row chromium-flags $C --ignore-gpu-blocklist --enable-unsafe-webgpu $U
row chromium-angle-vk $C --ignore-gpu-blocklist --enable-unsafe-webgpu \
    --use-angle=vulkan --enable-features=Vulkan,DefaultANGLEVulkan,VulkanFromANGLE $U
cat >"$o/user.js" <<EOT
user_pref("dom.webgpu.enabled", true);
user_pref("gfx.webgpu.ignore-blocklist", true);
user_pref("webgl.sanitize-unmasked-renderer", false);
user_pref("browser.shell.checkDefaultBrowser", false);
user_pref("datareporting.policy.dataSubmissionEnabled", false);
EOT
mkdir -p ~/prof
row firefox sh -c "cp '$o/user.js' ~/prof/ && MOZ_ENABLE_WAYLAND=1 exec firefox-esr --new-instance --no-remote --kiosk --profile ~/prof $U"
