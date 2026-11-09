#!/bin/sh
#
# g-cube.sh OUT: vkcube on Venus under the session of g-sess.sh: 300 frames in
# FIFO timed, then a screenshot taken while a longer run draws, and the centre
# and corner pixels of the shot.
#
set -u
o=$1
. ~/sess.env
mkdir -p "$o"
export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/virtio_icd.json
t0=$(date +%s.%N)
vkcube --wsi wayland --c 300 --present_mode 2 >"$o/cube-300.txt" 2>&1
st=$?
t1=$(date +%s.%N)
echo "vkcube 300 FIFO frames: exit $st, $(echo "$t1 - $t0" | bc) s"
vkcube --wsi wayland --c 3000 >"$o/cube-shot.txt" 2>&1 &
c=$!
sleep 4
rm -f wayland-screenshot-*.png
(cd "$o" && weston-screenshooter >"$o/shooter.txt" 2>&1)
wait $c
echo "vkcube 3000: exit $?"
f=$(ls "$o"/wayland-screenshot-*.png 2>/dev/null | head -1)
[ -n "$f" ] && mv "$f" "$o/cube.png" && python3 -I -c '
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB"); w, h = im.size
px = [im.getpixel((x, y)) for x in range(0, w, 8) for y in range(0, h, 8)]
print("shot %dx%d, centre %s, corner %s, %d distinct colours in a 1/64 sample" %
      (w, h, im.getpixel((w // 2, h // 2)), im.getpixel((2, 2)), len(set(px))))' "$o/cube.png"
grep -h 'Selected GPU' "$o/cube-300.txt"
cat "$o/shooter.txt"
