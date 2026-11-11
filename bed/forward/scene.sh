#!/bin/sh
# scene.sh: the forward direction's scenes, run inside the Debian guest as
# the user gfx, whose sudo asks no password: a picture drawn through the
# guest's virtio-gpu and checked there, and its rate.
#
#   scene.sh virgl|venus [-o OUT] [-r REF] [-M SCENE [-t SECONDS]]
#
# `virgl` builds triangle.c, bed/fixture's, with the guest's GCC and draws
# the triangle on the guest's llvmpipe and then on its GPU; it passes when
# the GPU's renderer is virgl and both pictures have one checksum, which the
# triangle takes of the pixels it read back. glmark2-es2-drm then draws its
# scene SCENE off screen at SIZE as root, the card's master: once with
# --validate, which compares the frame with glmark2's own reference, and once
# as the benchmark, which gives the frames a second.
# `venus` asks vulkaninfo for the device of Venus's ICD alone, which must be
# Venus, and runs the graphics tests' vkcube.sh as root on that ICD: vkcube
# presents to a Weston of its own on the card, is held after its 300th and
# 345th frames, and each, read back inside the guest, must have the checksum
# REF gives it, a frames file of vkcube.sh, by default vkcube.ref beside this
# file, the Debian reference guest's on the testing PC's lavapipe. A frame's
# sum depends on the GPU that draws it, so another host's run names the
# frames that Debian in Rach2's place drew there; REF `none` records the
# frames without judging them, which is how that reference is taken.
#
# With -M the profile's check gives way to the test bed's measurement
# harness, measure/ beside this file: its SCENE, glmark2 or webgl, which the
# guest's packages have, SECONDS long, as the rung forward-PROFILE; under
# venus, GL goes through Zink on Venus's ICD. Its line is printed as
# `bed-forward: figure: LINE`, and its marks `measure: counts: before` and
# `after` as it reads its counts, at which forward.sh reads Rach2's.
#
# Beside this file in the guest are triangle.c, guest-env.sh, and for venus
# runners/, the graphics tests' runners, and vkcube.ref. OUT, /tmp/out by
# default, gets every log. Each fact is a line `bed-forward: NAME: VALUE`,
# and the last line is
#
#   bed-forward: result: pass: PROFILE: DETAIL
#
# or `bed-forward: result: fail: REASON`; the status is 0 on a pass.
set -u

SCENE=build:use-vbo=true
SIZE=1280x800
ICD=/usr/share/vulkan/icd.d/venus_icd.x86_64.json

here=$(cd "$(dirname "$0")" && pwd)
die()
{
    echo "bed-forward: result: fail: $*"
    exit 1
}

usage='usage: scene.sh virgl|venus [-o OUT] [-r REF] [-M SCENE [-t SECONDS]]'
[ $# -ge 1 ] || die "$usage"
profile=$1
shift
out=/tmp/out
ref=$here/vkcube.ref
measure='' seconds=10
while getopts o:r:M:t: o; do
    case $o in
        o) out=$OPTARG ;;
        r) ref=$OPTARG ;;
        M) measure=$OPTARG ;;
        t) seconds=$OPTARG ;;
        *) die "$usage" ;;
    esac
done
mkdir -p "$out" || die "cannot make $out"

# The performance class of the graphics runs: llvmpipe's threads at the
# guest's processors, as the Debian reference guest's runs have them.
GRAPHICS_CLASS=performance GRAPHICS_DONE=1
export GRAPHICS_CLASS GRAPHICS_DONE
# shellcheck source=../../tests/graphics/guest-env.sh
. "$here/guest-env.sh" || die "guest-env.sh did not load"

# last FILE: the last lines of FILE on one line, for a failure's reason.
last()
{
    tail -n 3 "$1" | tr '\n' ' '
}

# triangle NAME [VAR=VALUE...]: draw the triangle with the environment
# given, its output in OUT/NAME.out, and print its checksum.
triangle()
{
    name=$1
    shift
    env "$@" "$out/triangle" > "$out/$name.out" 2>&1 || die "the $name triangle failed: $(last "$out/$name.out")"
    sed -n 's/^triangle: checksum: //p' "$out/$name.out"
}

# glmark2 LOG [ARG...]: run glmark2-es2-drm on SCENE off screen as root,
# its output in OUT/LOG.
glmark2()
{
    log=$1
    shift
    text=$(sudo glmark2-es2-drm --off-screen -s "$SIZE" -b "$SCENE" "$@" 2>&1)
    status=$?
    printf '%s\n' "$text" > "$out/$log"
    return "$status"
}

virgl()
{
    gcc -std=c99 -O2 -o "$out/triangle" "$here/triangle.c" -ldl > "$out/build.log" 2>&1 ||
        die "the triangle did not build: $(last "$out/build.log")"
    soft=$(triangle llvmpipe LIBGL_ALWAYS_SOFTWARE=1) || exit 1
    gpu=$(triangle gpu) || exit 1
    renderer=$(sed -n 's/^triangle: renderer: //p' "$out/gpu.out")
    echo "bed-forward: triangle: llvmpipe $soft, $renderer $gpu"
    case $renderer in
        virgl*) ;;
        *) die "the GPU's renderer is ${renderer:-unnamed}, not virgl" ;;
    esac
    [ -n "$gpu" ] && [ "$gpu" = "$soft" ] || die "virgl's triangle is ${gpu:-unread}, llvmpipe's $soft"
    glmark2 validate.log --validate || die "glmark2 --validate failed: $(last "$out/validate.log")"
    valid=$(sed -n 's/.*Validation: \([A-Za-z]*\).*/\1/p' "$out/validate.log" | head -n 1)
    glmark2 benchmark.log || die "glmark2 failed: $(last "$out/benchmark.log")"
    fps=$(sed -n 's/.*FPS: \([0-9]*\).*/\1/p' "$out/benchmark.log" | head -n 1)
    echo "bed-forward: glmark2: $(sed -n 's/^ *GL_RENDERER: *//p' "$out/benchmark.log" | head -n 1)"
    echo "bed-forward: glmark2: [$SCENE] at $SIZE: validation ${valid:-none}, ${fps:-no} frames a second"
    [ "$valid" = Success ] || die "glmark2's [$SCENE] validation gave ${valid:-nothing}"
    [ -n "$fps" ] || die "glmark2's [$SCENE] gave no rate"
    echo "bed-forward: result: pass: virgl: triangle $gpu on $renderer;" \
        "glmark2 [$SCENE] validated at $fps frames a second"
}

venus()
{
    VK_DRIVER_FILES=$ICD vulkaninfo --summary > "$out/vulkaninfo.out" 2>&1 ||
        die "vulkaninfo failed: $(last "$out/vulkaninfo.out")"
    device=$(sed -n 's/^[[:space:]]*deviceName[[:space:]]*= //p' "$out/vulkaninfo.out" | head -n 1)
    echo "bed-forward: vulkan: $device"
    case $device in
        *Venus*) ;;
        *) die "Vulkan's device is ${device:-none}, not Venus" ;;
    esac
    text=$(sudo -E sh "$here/runners/vkcube.sh" -i "$ICD" -o "$out/vkcube" 2>&1)
    status=$?
    printf '%s\n' "$text" > "$out/vkcube.out"
    sudo chown -R "$(id -u)" "$out/vkcube" 2> /dev/null
    line=$(grep 'vkcube: result:' "$out/vkcube.out" | tail -n 1)
    [ "$status" -eq 0 ] && [ -n "$line" ] || die "vkcube.sh failed: $(last "$out/vkcube.out")"
    echo "bed-forward: weston: $(sed -n 's/^renderer //p' "$out/vkcube/record" | head -n 1)"
    echo "bed-forward: $line"
    if [ "$ref" = none ]; then
        echo "bed-forward: result: pass: venus: $device; ${line#vkcube: result: }; frames not judged"
        return
    fi
    [ -f "$ref" ] || die "no reference frames $ref"
    for n in 300 345; do
        want=$(sed -n "s/^frame $n .* crc32 //p" "$ref")
        got=$(sed -n "s/^frame $n .* crc32 //p" "$out/vkcube/frames")
        [ -n "$want" ] && [ "$got" = "$want" ] ||
            die "vkcube's frame $n is ${got:-unread}, not the reference's ${want:-missing one}"
    done
    echo "bed-forward: result: pass: venus: $device; ${line#vkcube: result: }"
}

# measure_scene: the harness's scene `measure` on this profile's device.
measure_scene()
{
    [ "$profile" != venus ] || export MESA_LOADER_DRIVER_OVERRIDE=zink VK_DRIVER_FILES=$ICD
    # The harness's marks go out as it prints them.
    {
        MEASURE_RUNNERS=$here/runners sh "$here/measure/measure.sh" -c -r "forward-$profile" -t "$seconds" \
            -o "$out/measure" "$measure" 2>&1
        echo $? > "$out/measure.status"
    } | tee "$out/measure.log" | grep --line-buffered '^measure: counts: '
    [ "$(cat "$out/measure.status")" = 0 ] || die "the harness's $measure failed: $(last "$out/measure.log")"
    sed 's/^/bed-forward: figure: /' "$out/measure.log"
    echo "bed-forward: result: pass: $profile: $(tail -n 1 "$out/measure.log")"
}

case $profile in
    virgl | venus) [ -z "$measure" ] || { measure_scene; exit 0; } ;;
esac
case $profile in
    virgl) virgl ;;
    venus) venus ;;
    *) die "no profile $profile; $usage" ;;
esac
