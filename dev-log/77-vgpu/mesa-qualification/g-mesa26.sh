#!/bin/sh
#
# g-mesa26.sh: replace every Mesa package of the guest with trixie-backports'
# 26.1.6, so that Venus, virgl's GL driver, llvmpipe and lavapipe all come from
# it, and list what apt changed in ~/out/mesa26-apt.txt.
#
set -eu
export DEBIAN_FRONTEND=noninteractive
p=$(dpkg-query -W -f '${Package}\n' mesa-vulkan-drivers libgl1-mesa-dri \
    libegl-mesa0 libgbm1 libglx-mesa0 mesa-libgallium mesa-va-drivers \
    mesa-vdpau-drivers 2>/dev/null)
sudo apt-get install -y -t trixie-backports $p >~/out/mesa26-apt.txt 2>&1
grep -E '^(Inst|Unpacking|Setting up)' ~/out/mesa26-apt.txt | grep -vi trigger | head -40
dpkg-query -W 'mesa-*' libgl1-mesa-dri libegl-mesa0 libgbm1 libglx-mesa0 \
    libdrm2 libvulkan1 2>/dev/null
