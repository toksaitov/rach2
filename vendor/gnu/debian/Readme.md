# Debian packages

This layer stages Debian 13's desktop and graphics tools for the GNU root. `make gnu-desktop` builds one closure for Mesa, Weston, seatd, udev, Xwayland, Chromium, Firefox ESR, sound, fonts and test tools. Both amd64 and i386 use the same lists and signed snapshot. `make gnu-webgl` stages Khronos's conformance sources in `/usr/share/webgl`.

## Use

`fetch_debs.sh LAYER [STEP...]` runs `index`, `resolve`, `fetch`, `unpack` and `check`, or only the named steps. `GNU_DEBIAN_LISTS` names every list the layer reads, relative to `vendor/gnu`; the default is the desktop's five lists. A row names a package, optionally `=VERSION`, and optionally the architecture `amd64` or `i386`. Blank lines and comments are ignored. A missing list, package or required version fails the build.

`i386.list` is the list of the x86-64 root's i386 layer, which `make ARCH=x86_64 gnu-i386` resolves against the i386 indices of the same snapshot; `multiarch.sh LAYER` then lists each package of the layer's closure with its Multi-Arch field in `work/debian/LAYER/multiarch.txt`, from which `install_root.sh` takes the `same` packages ([The i386 layer](../Readme.md#the-i386-layer)).

`commands` lists the canonical commands of the desktop closure for the program catalog; update it from the unpacked file lists when the package set changes. The stage is `debian/LAYER` in the GNU build tree; `work/debian/LAYER/closure.txt` records each package, version, archive, path, byte size and SHA-256, and `files/` holds its unpacked paths. Logs are `log/debian-LAYER-STEP.log`. The clone's `build/cache/debian/ARCH` keeps downloads. A cached file with a different digest is removed and refused.

## Sources and license

The snapshot, suites' InRelease digests and archive key fingerprints are in `fetch_debs.sh`. The desktop and the browser reference guest use the same snapshot so their Chromium and Firefox binaries match. The keys are pinned in `../sources-extra.txt`. Each InRelease is checked by digest and `gpgv`; each Packages index by the signed SHA-256; every package by the index's SHA-256 before extraction. The resolver checks Debian version relations with `dpkg --compare-versions`, including versioned virtual providers. It follows Depends and Pre-Depends, without Recommends. Accounts supplied by the image satisfy `adduser`. `GNU_DEBIAN_UNFOLLOWED` names more packages whose dependency ends the search, since another layer supplies them, as the i386 layer's SuperTuxKart takes its data from the games layer.

WebGL's full commit and archive SHA-256 are in `../sources-extra.txt`; `fetch_webgl.sh` checks the digest before extraction. The suite keeps its upstream licenses. Debian's packages keep `/usr/share/doc/PACKAGE/copyright` in the image. The project's command pages retain the text of their Debian manuals, rendered to ASCII, with brief local pages for tools that ship no page. `copyright/` keeps the corresponding package notices; non-ASCII code points there are written as roff Unicode escapes. The GNU root also renders the original pages with `render_man.sh`.

## Design

`install_root.sh` takes the desktop closure as one layer, including chromium-headless-shell. Its SKIP list preserves the GNU ports and the image's init, accounts and base configuration. Debian's `libtinfo6` stays because the GNU ncurses build has no separate `libtinfo.so.6`; LLVM loads it through libedit. Debian's libraries with different sonames, including OpenSSL 3 and FFmpeg 7, coexist with the newer GNU ports. Python 3.13 is installed by its versioned name for Debian's Python modules; the GNU port keeps `python3`. No package maintainer script runs. The image supplies the BLAS and LAPACK alternatives and the default Adwaita cursor theme, builds the GSettings, GdkPixbuf and MIME caches, renders the upstream manual pages and rebuilds its loader cache.

Mesa 25.0.7 supplies llvmpipe, lavapipe, Crocus and hasvk; the root's Venus driver is Mesa 26.1.6's, which `fetch_venus.sh` stages from `venus.list` beside this layer ([Venus](../Readme.md#venus)). Lavapipe's Direct3D limit is feature level 11_1 and shader model 6.0 with the pinned Proton. EGL enumerates only DRM devices with a render node. A card-only display uses Pixman in Weston; a program that opens the card itself can still render with GL.

### The video layer

`va.list` is the video layer, a closure of its own that `make gnu-va` stages in `debian/va` with `fetch_debs.sh va`, from the same snapshot, and that `install_root.sh` installs right after the desktop closure with `install_debian va`: libva 2.22.0, its DRM, X11 and Wayland backends, Intel's i965 VA driver 2.4.1, and vainfo from libva-utils 2.22.0, 25 packages and 5.5 MB with what they need. A package the desktop closure has at the same version is left to it. libva, given the i915 driver's render node, looks for Intel's media driver, `iHD_drv_video.so`, which the root does not have, and then loads `/usr/lib/MULTIARCH/dri/i965_drv_video.so`, which decodes MPEG-2, H.264, VC-1 and JPEG on Haswell through the video ring. `va.commands` lists vainfo for the program catalog. libva, the i965 driver and libva-utils are under the MIT license, the i965 driver's shaders also under the Eclipse Public License 1.0 and the Apache License 2.0, and libva-utils's JPEG decoder under the 3-clause BSD license, as each package's `/usr/share/doc/PACKAGE/copyright` says; `copyright/vainfo.txt` keeps vainfo's notice beside its page.

## Updating

Record a new snapshot only after verifying its archive signatures, then update the InRelease pins and compare the closure and the installer's SKIP list. Mesa's required versions are in `mesa.list`. A later layer, such as the [games layer](../games/Readme.md), has lists and a stage of its own, which `fetch_debs.sh LAYER` builds with `GNU_DEBIAN_LISTS` naming its lists, and is installed by `install_root.sh`'s `install_debian LAYER` after the desktop. A package an earlier layer installed is left out of a later one when the versions match and refused when they do not, and a file an earlier layer installed is never replaced.
