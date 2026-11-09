#!/bin/sh
#
# g-cts.sh: build deqp-vk of VK-GL-CTS 1.4.1.1 in ~/cts. Its pinned sources,
# the commits external/fetch_sources.py names, come as GitHub archives in
# ~/src/cts-ext and are unpacked where that script would clone them; Debian's
# zlib and libpng stand in for the bundled ones, and Vulkan-Docs and the
# validation layers, which the build does not need, are left out. Log in
# ~/out/cts-build.log.
#
set -eux
cd ~
rm -rf cts
mkdir cts
tar xzf src/vk-gl-cts-1.4.1.1.tar.gz -C cts --strip-components=1
for d in spirv-tools glslang spirv-headers amber jsoncpp nvidia-video-samples \
    vulkan-video-samples video_generator; do
    mkdir -p cts/external/$d/src
    tar xzf src/cts-ext/$d.tar.gz -C cts/external/$d/src --strip-components=1
done
cd cts
# 1.4.1.1 stops when the layers' source is absent; deqp-vk runs without them.
sed -i 's/message(FATAL_ERROR "vulkan-validationlayers not found")/message(STATUS "vulkan-validationlayers left out")/' \
    external/vulkan-validationlayers/CMakeLists.txt
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DDEQP_TARGET=default
nice -n 10 ninja -C build -j14 deqp-vk
ls -l build/external/vulkancts/modules/vulkan/deqp-vk
echo g-cts: done
