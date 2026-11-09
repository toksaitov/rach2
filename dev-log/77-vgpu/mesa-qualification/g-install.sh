#!/bin/sh
# g-install.sh: the guest's packages from trixie (Mesa 25.0.7 among them), with
# trixie-backports added as a source and nothing taken from it yet.
set -eux
export DEBIAN_FRONTEND=noninteractive
cat >/etc/apt/sources.list.d/backports.sources <<EOT
Types: deb deb-src
URIs: http://deb.debian.org/debian
Suites: trixie-backports
Components: main
Signed-By: /usr/share/keyrings/debian-archive-keyring.pgp
EOT
apt-get update
apt-get install -y --no-install-recommends linux-image-amd64
apt-get install -y \
    mesa-vulkan-drivers mesa-utils vulkan-tools libvulkan-dev libgl1-mesa-dri \
    weston xwayland seatd chromium firefox-esr python3-websocket python3-pil \
    gcc-mingw-w64-x86-64 gcc-mingw-w64-i686 wine wine64 \
    cmake ninja-build g++ python3 pkg-config libpng-dev zlib1g-dev \
    libwayland-dev libx11-dev libxcb1-dev libxcb-randr0-dev wayland-protocols \
    libgl-dev libegl-dev git bc \
    strace jq curl xz-utils file
apt-cache policy mesa-vulkan-drivers
