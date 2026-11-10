# GE-Proton

`make ARCH=x86_64 gnu-proton` stages GE-Proton11-12 under `/opt/proton` in the GNU root. The i686 root has no Proton layer. The launcher is `/opt/proton/proton`; Steam is not installed.

## Sources and license

`../sources-extra.txt` pins the 566,859,228-byte release archive by SHA-256 and records the release's published SHA-512. `fetch_proton.sh` verifies both before extraction, including on a cache hit. The release keeps its licenses and notices. The clone caches it under `build/cache/gnu`. [`sources.txt`](sources.txt) pins the Wine, Wine Staging and GE source archives of that release for the DirectInput rebuild.

## Local changes

[`dinput-shutdown.patch`](dinput-shutdown.patch) skips the input worker's stop handshake when `RtlDllShutdownInProgress` is true. Process exit has already terminated that worker before DLL detach callbacks run; an application releasing its last DirectInput interface there otherwise waits forever for the worker to set a new event. Ordinary interface release and DLL unload retain their existing handshake.

## Design

The desktop Debian lists supply the display, Vulkan, GnuTLS and GStreamer libraries the runtime loads. GE-Proton's own libraries stay in its release tree. Its DXVK and vkd3d-proton use lavapipe in a software-only guest; WineD3D is the Haswell path. The caller chooses a private prefix and the renderer. The graphics runners qualify its Direct3D paths.

## Building

After extracting the release, `fetch_dinput.sh` rebuilds only `dinput.dll` and `dinput8.dll` for x86-64 and i386, with Wine's native build tools and clang's Windows targets. The Linux host needs clang 19 or later, lld, LLVM's `llvm-dlltool`, autoconf, flex and bison. The tooling machine provisions them. No Windows SDK or MinGW compiler is needed.

The source begins at GE's pinned Valve Wine commit. `prepare_dinput.py` applies the production hunks of Staging's three `dinput-scancode` patches, GE's `proton-ds5-haptic` patches 0197, 0198, 0199 and 0202, and the HID header addition of 0209. This retains GE's keyboard mapping, Sony controller handling and private HID requests without applying unrelated modules' patches. Both DLLs share the corrected `dinput_main.c` and import `ucrtbase` as the release does. The staged `share/rach2-dinput` keeps the local patch and source pins. Build output stays under the GNU build tree's `work/proton-dinput`; no probes run in the build.
