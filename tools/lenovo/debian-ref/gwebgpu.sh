#!/bin/sh
# shellcheck disable=SC2086,SC2046 # no path or word list here has a space
# gwebgpu.sh (in run2 step 10's guest, as gfx): Chromium 156 on the probe page
# under the guest's Weston, once per switch set, with Chromium's and Dawn's
# logging on, to see why Dawn lists Venus over hasvk as available and
# requestAdapter returns null. Each run keeps the page's result, chrome://gpu
# and the browser's stderr in ~/out/ref80-webgpu/.
O=~/out/ref80-webgpu; W=~/web; mkdir -p $O ~/prof
XDG_RUNTIME_DIR=/run/user/$(id -u); WAYLAND_DISPLAY=wl-g
export XDG_RUNTIME_DIR WAYLAND_DISPLAY
if ! curl -s -o /dev/null http://127.0.0.1:8000/probe.html; then
	setsid python3 -I $W/probe-server.py $W $O 8000 > $O/server.log 2>&1 < /dev/null &
	sleep 1
fi
SO=$(tr '\0' '\n' < /proc/$(pgrep -f probe-server.py | head -1)/cmdline | sed -n 5p)
LOG="--enable-logging=stderr --v=0 --vmodule=*webgpu*=3,*dawn*=3,*gpu_info*=2,*gpu_init*=2"
BOTH="--enable-unsafe-webgpu --ignore-gpu-blocklist"
CRF="--ozone-platform=wayland --no-first-run --no-default-browser-check --password-store=basic --remote-debugging-port=9222 --start-fullscreen"
# run NAME ARGS...: one start of Chromium on probe.html?name=NAME.
run() {
	n=$1; shift
	rm -rf ~/prof/$n $SO/$n.json
	echo "$(date -u +%T) start $n: $*" >> $O/runs.log
	setsid chromium $CRF $LOG --user-data-dir=$HOME/prof/$n "$@" "http://127.0.0.1:8000/probe.html?name=$n" > $O/$n.log 2>&1 < /dev/null &
	i=0; while [ ! -s $SO/$n.json ] && [ $i -lt 90 ]; do sleep 1; i=$((i + 1)); done
	echo "$(date -u +%T) $n: result after $i s: $(test -s $SO/$n.json && echo yes || echo no)" | tee -a $O/runs.log
	[ "$SO" != "$O" ] && [ -s $SO/$n.json ] && cp $SO/$n.json $O/
	timeout 60 python3 -I $W/cdp.py 9222 $O/$n > $O/$n.cdp.log 2>&1
	pkill -x chromium; sleep 3; pkill -9 -x chromium; sleep 1
	rm -rf ~/prof/$n
	python3 -I -c '
import json, sys
try:
    r = json.load(open(sys.argv[1]))
except Exception as e:
    print("  no result:", e); sys.exit()
g = r.get("webgpu", r)
print("  webgpu:", json.dumps({k: g.get(k) for k in ("ok", "reason", "error", "info", "adapter", "fallbackTried", "computeMs")}, default=str)[:600])
' $O/$n.json
	grep -E "^(WebGPU|Vulkan|GL_RENDERER|Sandboxed)" $O/$n-gpu.txt 2>/dev/null | head -6 | sed 's/^/  /'
	echo "  GL interop extensions: $(grep -o -E 'GL_EXT_(memory_object|semaphore)(_fd)?' $O/$n-gpu.txt 2>/dev/null | sort -u | tr '\n' ' ')"
}
[ $# -gt 0 ] && { run "$@"; exit; }
run wg-both $BOTH
# Chromium 156 makes Dawn search the Vulkan backend on Linux only when its
# GPU process has a Vulkan context (gpu_init.cc, InitializeVulkan), which
# CheckVulkanCompatibilities refuses unless the GL context has
# GL_EXT_memory_object_fd and GL_EXT_semaphore_fd (vulkan_util.cc); otherwise
# the backend list is Null and requestAdapter finds nothing
# (webgpu_decoder_impl.cc, CreatePreferredAdapter). --use-vulkan=native, which
# service_utils.cc reads as kForcedNative, skips that
# check.
run wg-both-forced-native $BOTH --use-vulkan=native
# The GL device is virgl's 0x1af4/0x1050 and Venus reports Intel's IDs: give
# Chromium Intel's IDs for its GPU to see whether the mismatch decides.
run wg-both-intelid $BOTH --gpu-testing-vendor-id=0x8086 --gpu-testing-device-id=0x0a16
# Dawn's GL ES backend (ANGLE on virgl) in compatibility mode.
run wg-both-opengles $BOTH --use-webgpu-adapter=opengles --force-webgpu-compat
echo "$(date -u +%T) done" >> $O/runs.log
