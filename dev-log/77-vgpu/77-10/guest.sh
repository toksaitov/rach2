#!/bin/sh
# Run the Lenovo's Venus comparison inside either guest, from the payload root.
#
#   guest.sh SYSTEM DISPLAY [PART [OUTPUT=MODE]]
#
# PART is all (the default): vkcube's rounds, and under GTK dEQP-VK and the
# two WebGPU requests; cube, the rounds alone; or browser, the two requests
# alone, under GTK. OUTPUT=MODE goes to vkcube.sh's -m.
set -eu
system=$1 display=$2 part=${3:-all} mode=${4:-}
case $system in debian) account=gfx ;; rach2) account=test ;; *) exit 2 ;; esac
case $display in gtk|sdl) ;; *) exit 2 ;; esac
case $part in all|cube) ;; browser) [ "$display" = gtk ] || exit 2 ;; *) exit 2 ;; esac
base=$(pwd)
out=$base/build/runs/results
TMPDIR=$base/build/tmp
export TMPDIR
mkdir -p "$out" "$TMPDIR"
chown "$account" "$out" "$TMPDIR"
icd=/usr/share/vulkan/icd.d/venus_icd.x86_64.json
test -f "$icd"

# as_account SCRIPT: run SCRIPT in sh as the ordinary account, from the payload
# root. Both guests have su; Rach2's GNU root has no runuser. No path here has
# a space or a quote. Each row's own user makes /tmp/graphics.env, since
# Debian's fs.protected_regular refuses root's O_CREAT of another's file in
# the sticky /tmp.
as_account()
{
    rm -f /tmp/graphics.env
    su -s /bin/sh "$account" -c "cd $base && set -e && $1"
}
# A version line's failure is kept in the record and stops nothing.
for line in 'uname -a' 'cat /proc/cmdline' "cat $icd" \
    'sha256sum /usr/lib/x86_64-linux-gnu/venus/libvulkan_virtio.so' \
    'sha256sum "$(command -v vkcube)"' 'weston --version' 'chromium --version'; do
    sh -c "$line" || echo "venus-lenovo: $line: status $?"
done > "$out/versions" 2>&1
VK_DRIVER_FILES=$icd vulkaninfo --summary > "$out/vulkaninfo" 2> "$out/vulkaninfo.err"
grep 'Virtio-GPU Venus' "$out/vulkaninfo"
grep 'Intel' "$out/vulkaninfo"
rounds='1 2 3'
[ "$part" != browser ] || rounds=''
for round in $rounds; do
    echo "venus-lenovo: vkcube: $system $display round $round"
    rm -f /tmp/graphics.env
    (
        GRAPHICS_CLASS=performance GRAPHICS_DONE=1
        MESA_SHADER_CACHE_DIR=$TMPDIR/cache-cube-$round
        . ./tests/graphics/guest-env.sh
        graphics_env > /tmp/graphics.env
        sh tests/graphics/runners/vkcube.sh -i "$icd" ${mode:+-m "$mode"} -o "$out/cube-$round"
    )
done
if [ "$display" = gtk ] && [ "$part" = all ]; then
    for list in smoke sample; do
        echo "venus-lenovo: deqp: $system $list"
        case $list in
            smoke) cases='-l tests/graphics/deqp-vk-venus-smoke.txt' ;;
            sample) cases='-e 500 -l tests/graphics/deqp-vk-lavapipe.txt.gz -f tests/graphics/deqp-vk-lavapipe.flakes' ;;
        esac
        as_account "GRAPHICS_CLASS=conformance GRAPHICS_DONE=1
            MESA_SHADER_CACHE_DIR=$TMPDIR/cache-$list
            . ./tests/graphics/guest-env.sh
            graphics_env > /tmp/graphics.env
            exec sh tests/graphics/runners/deqp.sh -v 1.4.1.1 -j 4 -i $icd \
                -o $out/deqp-$list $cases vk"
    done
fi
if [ "$display" = gtk ] && [ "$part" != cube ]; then
    for request in webgpu-venus webgpu-venus-native; do
        echo "venus-lenovo: browser: $system $request"
        as_account "GRAPHICS_CLASS=performance GRAPHICS_DONE=1
            MESA_SHADER_CACHE_DIR=$TMPDIR/cache-$request
            . ./tests/graphics/guest-env.sh
            graphics_env > /tmp/graphics.env
            exec sh tests/graphics/browser/run.sh chromium $request $out/chromium-$request"
    done
fi
echo "venus-lenovo: result: collected $system $display $part"
