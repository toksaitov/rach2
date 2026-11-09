#!/bin/sh
#
# g-vk.sh OUT: the guest's Vulkan facts on Venus into OUT: the Mesa packages,
# vulkaninfo's summary and full text with Venus as the only device, Venus's
# start-up line, and each extension DXVK, vkd3d-proton and 32-bit WoW64 ask.
#
set -u
o=$1
mkdir -p "$o"
v=/usr/share/vulkan/icd.d/virtio_icd.json
dpkg-query -W 'mesa-*' 'libgl1-mesa-dri' 'libegl-mesa0' 'libgbm1' \
    'libglx-mesa0' 'mesa-vulkan-drivers' >"$o/mesa-packages.txt" 2>&1
uname -a >"$o/uname.txt"
VK_DRIVER_FILES=$v vulkaninfo --summary >"$o/summary.txt" 2>&1
VK_DRIVER_FILES=$v vulkaninfo >"$o/full.txt" 2>&1
VK_DRIVER_FILES=$v VN_DEBUG=init vulkaninfo --summary >/dev/null 2>"$o/vn-init.txt"
for e in VK_KHR_load_store_op_none VK_EXT_load_store_op_none \
    VK_KHR_maintenance5 VK_KHR_maintenance6 VK_EXT_map_memory_placed \
    VK_EXT_external_memory_host VK_KHR_swapchain VK_KHR_synchronization2 \
    VK_EXT_robustness2 VK_KHR_robustness2 VK_EXT_graphics_pipeline_library; do
    if grep -q "^[[:space:]]*$e[[:space:]]*:" "$o/full.txt"; then
        echo "$e yes"
    else
        echo "$e no"
    fi
done >"$o/wanted.txt"
grep -A3 -i 'placedMemoryMap\|memoryMapPlaced\|memoryUnmapReserve\|minPlacedMemoryMapAlignment' \
    "$o/full.txt" >"$o/placed.txt" 2>&1
grep -E 'deviceName|apiVersion|driverInfo|driverName|conformanceVersion|deviceType' \
    "$o/summary.txt"
cat "$o/wanted.txt"
