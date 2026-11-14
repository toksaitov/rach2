#!/bin/sh
# loop.sh N OUT: run piglit's shader-mem-barrier N times on the guest's GPU,
# surfaceless EGL, as piglit.sh -g does, and append each run's counts and
# failing subtests to OUT.
n=$1 out=$2
bin=/usr/lib/x86_64-linux-gnu/piglit/bin/arb_shader_image_load_store-shader-mem-barrier
EGL_PLATFORM=surfaceless PIGLIT_PLATFORM=surfaceless_egl PIGLIT_NO_FAST_SKIP=1
LP_NUM_THREADS=0 MESA_SHADER_CACHE_DISABLE=true
export EGL_PLATFORM PIGLIT_PLATFORM PIGLIT_NO_FAST_SKIP LP_NUM_THREADS MESA_SHADER_CACHE_DISABLE
: > "$out"
i=1
while [ "$i" -le "$n" ]; do
    "$bin" --quick -auto -fbo > /tmp/smb.out 2>&1
    rc=$?
    echo "run $i rc $rc pass $(grep -c '"pass"' /tmp/smb.out) fail $(grep -c '"fail"' /tmp/smb.out)" >> "$out"
    grep '"fail"' /tmp/smb.out | sed "s/^/run $i FAIL /" >> "$out"
    grep -A2 'Probe value' /tmp/smb.out | sed "s/^/run $i PROBE /" >> "$out"
    i=$((i + 1))
done
echo LOOP-DONE >> "$out"
