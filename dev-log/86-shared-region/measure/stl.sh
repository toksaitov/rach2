#!/bin/sh
# stl.sh: run piglit's streaming-texture-leak on the guest's GPU, as
# piglit.sh -g runs it, while sampling memory and the virtio-gpu counts once
# a second; inside a graphics guest, Rach2's or Debian's, as an ordinary
# user, with tests/graphics/guest-env.sh sourced.
#
#   stl.sh DIR OUT
#
# DIR holds ioctlcount.c, which is built there with the guest's cc. OUT gets
# `samples`, one line a second: the milliseconds since the test began,
# MemAvailable, Shmem and MemFree of /proc/meminfo in kB, the counters of
# ioctlcount.c (EXECBUFFER, WAIT, WAIT with NOWAIT, WAIT answered EBUSY,
# RESOURCE_CREATE, GEM_CLOSE, TRANSFER_TO_HOST, TRANSFER_FROM_HOST,
# CREATE_BLOB), then each vgpu_m_ counter of /proc/stat where the kernel has
# them, Rach2's measuring branch; `fdinfo`, the drm- lines of the test's DRM
# files at the peak of its memory where the kernel gives them; `log`, the
# test's output; and `result`, its exit status, time and last counts.
set -u

dir=$1 out=$2
mkdir -p "$out"
cc -shared -fPIC -O2 -o "$out/ioctlcount.so" "$dir/ioctlcount.c" -ldl || exit 1
bin=$(find /usr/lib /usr/libexec /usr/local /opt -path '*piglit/bin/streaming-texture-leak' -type f 2>/dev/null | head -n 1)
[ -n "$bin" ] || { echo "stl: no piglit streaming-texture-leak" >&2; exit 1; }
echo "stl: test: $bin"

# As piglit.sh -g: piglit's surfaceless EGL platform on the device EGL
# finds, no fast skip.
PIGLIT_PLATFORM=surfaceless_egl EGL_PLATFORM=surfaceless PIGLIT_NO_FAST_SKIP=1
export PIGLIT_PLATFORM EGL_PLATFORM PIGLIT_NO_FAST_SKIP
counts=$out/counts
: > "$counts"

now_ms()
{
    date +%s%3N
}

sample()
{
    t=$(($(now_ms) - start))
    mem=$(awk '$1 == "MemAvailable:" { a = $2 } $1 == "Shmem:" { s = $2 } $1 == "MemFree:" { f = $2 }
               END { print a, s, f }' /proc/meminfo)
    ic=$(od -An -t u8 -v "$counts" 2>/dev/null | tr -s ' \n' '  ')
    vg=$(awk '/^vgpu_m_/ { printf " %s", $2 }' /proc/stat)
    echo "$t $mem $ic$vg" >> "$out/samples"
    [ -z "${STL_ECHO:-}" ] || echo "stl: sample: $t $mem $ic$vg"
}

# The names of the samples' columns, Rach2's counters only where they are.
{
    printf '# ms avail_kb shmem_kb free_kb exec wait wait_nowait wait_busy create gem_close to_host from_host blob'
    awk '/^vgpu_m_/ { printf " %s", $1 } END { print "" }' /proc/stat
} > "$out/samples"
start=$(now_ms)
sample
LD_PRELOAD=$out/ioctlcount.so IOCTLCOUNT_FILE=$counts "$bin" -auto -fbo > "$out/log" 2>&1 &
pid=$!
least=999999999
while kill -0 "$pid" 2>/dev/null; do
    sleep 1
    sample
    avail=$(awk '$1 == "MemAvailable:" { print $2 }' /proc/meminfo)
    if [ "$avail" -lt "$least" ]; then
        least=$avail
        for f in /proc/$pid/fdinfo/*; do
            grep -q '^drm-' "$f" 2>/dev/null && { echo "== $f at $(($(now_ms) - start)) ms"; grep '^drm-' "$f"; }
        done > "$out/fdinfo.tmp" 2>/dev/null && mv "$out/fdinfo.tmp" "$out/fdinfo"
    fi
done
wait "$pid"
status=$?
end=$(now_ms)
sleep 2
sample
{
    echo "status $status"
    echo "ms $((end - start))"
    echo "least_avail_kb $least"
    echo "last $(tail -n 1 "$out/samples")"
    tail -n 3 "$out/log"
} > "$out/result"
echo "stl: result: status $status, $((end - start)) ms"
