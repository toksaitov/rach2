# GE-Proton

`make ARCH=x86_64 gnu-proton` stages GE-Proton11-12 under `/opt/proton` in the GNU root. The i686 root has no Proton layer. The launcher is `/opt/proton/proton`; Steam is not installed.

## Sources and license

`../sources-extra.txt` pins the 566,859,228-byte release archive by SHA-256 and records the release's published SHA-512. `fetch_proton.sh` verifies both before extraction, including on a cache hit. The release keeps its licenses and notices. The clone caches it under `build/cache/gnu`.

## Design

The desktop Debian lists supply the display, Vulkan, GnuTLS and GStreamer libraries the runtime loads. GE-Proton's own libraries stay in its release tree. Its DXVK and vkd3d-proton use lavapipe in a software-only guest; WineD3D is the Haswell path. The caller chooses a private prefix and the renderer. This build installs the release; the graphics runners qualify its Direct3D paths.
