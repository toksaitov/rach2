# guest-env.sh: the environment of a graphics run inside its guest, Rach2's
# or Debian's, the same on both sides of every comparison.
# shellcheck shell=sh
#
# Sourced in the guest with GRAPHICS_CLASS set to the run's class:
#
#   conformance  dEQP-VK, piglit, the WebGL suite and Wine's tests:
#                LP_NUM_THREADS=0, since these suites already run a process
#                on each CPU and llvmpipe's own threads cost 4 to 5% with
#                five guests and buy nothing alone (run 2, steps 3 and 4).
#   vkd3d        vkd3d-proton's suite at its measured setting, until it is
#                qualified at 0: llvmpipe's threads left at the guest's CPU
#                count, and VKD3D_JOBS test processes at once, the CPU count
#                in a guest of 16 GiB or more and 4 below, since at 8 jobs it
#                needs 9.1 GB and in 8 GiB the guest's kernel killed tests.
#   performance  glmark2, the WebGL fill and ALU gates, vkcube, the game
#                routes and 77-2's and 77-5's comparisons: LP_NUM_THREADS at
#                the guest's CPU count.
#
# GRAPHICS_DONE=1 marks a run whose result is a Done's: Mesa's shader cache,
# MESA_SHADER_CACHE_DIR, is emptied as this file is sourced, so that no
# shader compiled by an earlier run is reused. graphics_xvfb starts Xvfb with
# -noreset, so that it does not reset when its last client leaves, and
# graphics_env prints the environment, one `env NAME=VALUE` line each, for
# the run's record.

graphics_cpus()
{
    nproc 2>/dev/null || grep -c '^processor' /proc/cpuinfo
}

graphics_memory_kib()
{
    awk '$1 == "MemTotal:" { print $2 }' /proc/meminfo
}

GRAPHICS_CPUS=$(graphics_cpus)
case "${GRAPHICS_CLASS:-}" in
    conformance)
        LP_NUM_THREADS=0
        ;;
    vkd3d)
        LP_NUM_THREADS=$GRAPHICS_CPUS
        if [ "$(graphics_memory_kib)" -ge $((15 * 1024 * 1024)) ]; then
            VKD3D_JOBS=$GRAPHICS_CPUS
        else
            VKD3D_JOBS=4
        fi
        export VKD3D_JOBS
        ;;
    performance)
        LP_NUM_THREADS=$GRAPHICS_CPUS
        ;;
    *)
        echo "guest-env: GRAPHICS_CLASS is conformance, vkd3d or performance, not ${GRAPHICS_CLASS:-unset}" >&2
        return 2
        ;;
esac
export LP_NUM_THREADS GRAPHICS_CLASS GRAPHICS_CPUS

MESA_SHADER_CACHE_DIR=${MESA_SHADER_CACHE_DIR:-${TMPDIR:-/tmp}/graphics-shader-cache}
export MESA_SHADER_CACHE_DIR
if [ "${GRAPHICS_DONE:-0}" = 1 ]; then
    rm -rf "$MESA_SHADER_CACHE_DIR"
fi
mkdir -p "$MESA_SHADER_CACHE_DIR"
GRAPHICS_DONE=${GRAPHICS_DONE:-0}
export GRAPHICS_DONE

# graphics_xvfb DISPLAY: start Xvfb on DISPLAY, :99 by default, at the
# window's 1280x800, with -noreset, and export DISPLAY.
graphics_xvfb()
{
    DISPLAY=${1:-:99}
    export DISPLAY
    Xvfb "$DISPLAY" -noreset -screen 0 1280x800x24 >/dev/null 2>&1 &
    GRAPHICS_XVFB=$!
    export GRAPHICS_XVFB
}

graphics_env()
{
    echo "env GRAPHICS_CLASS=$GRAPHICS_CLASS"
    echo "env GRAPHICS_CPUS=$GRAPHICS_CPUS"
    echo "env GRAPHICS_MEMORY_KIB=$(graphics_memory_kib)"
    echo "env LP_NUM_THREADS=$LP_NUM_THREADS"
    [ -z "${VKD3D_JOBS:-}" ] || echo "env VKD3D_JOBS=$VKD3D_JOBS"
    echo "env MESA_SHADER_CACHE_DIR=$MESA_SHADER_CACHE_DIR"
    echo "env MESA_SHADER_CACHE_FILES=$(find "$MESA_SHADER_CACHE_DIR" -type f | wc -l)"
    echo "env GRAPHICS_DONE=$GRAPHICS_DONE"
    echo "env XVFB=-noreset"
}
