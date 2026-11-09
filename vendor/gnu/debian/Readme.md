# Debian packages

This layer stages Debian 13's desktop and graphics tools for the GNU root. `make gnu-desktop` builds one closure for Mesa, Weston, seatd, udev, Xwayland, Chromium, Firefox ESR, sound, fonts and test tools. Both amd64 and i386 use the same lists and signed snapshot. `make gnu-webgl` stages Khronos's conformance sources in `/usr/share/webgl`.

## Use

`fetch_debs.sh LAYER [STEP...]` runs `index`, `resolve`, `fetch`, `unpack` and `check`, or only the named steps. `GNU_DEBIAN_LISTS` names every list the layer reads, relative to `vendor/gnu`; the default is the desktop's five lists. A row names a package, optionally `=VERSION`, and optionally the architecture `amd64` or `i386`. Blank lines and comments are ignored. A missing list, package or required version fails the build.

`commands` lists the canonical commands of the desktop closure for the program catalog; update it from the unpacked file lists when the package set changes. The stage is `debian/LAYER` in the GNU build tree; `work/debian/LAYER/closure.txt` records each package, version, archive, path, byte size and SHA-256, and `files/` holds its unpacked paths. Logs are `log/debian-LAYER-STEP.log`. The clone's `build/cache/debian/ARCH` keeps downloads. A cached file with a different digest is removed and refused.

## Sources and license

The snapshot, suites' InRelease digests and archive key fingerprints are in `fetch_debs.sh`. The keys are pinned in `../sources-extra.txt`. Each InRelease is checked by digest and `gpgv`; each Packages index by the signed SHA-256; every package by the index's SHA-256 before extraction. The resolver checks Debian version relations with `dpkg --compare-versions`, including versioned virtual providers. It follows Depends and Pre-Depends, without Recommends. Accounts supplied by the image satisfy `adduser`.

WebGL's full commit and archive SHA-256 are in `../sources-extra.txt`; `fetch_webgl.sh` checks the digest before extraction. The suite keeps its upstream licenses. Debian's packages keep `/usr/share/doc/PACKAGE/copyright` in the image. The project's command pages retain the text of their Debian manuals, rendered to ASCII, with brief local pages for tools that ship no page. `copyright/` keeps the corresponding package notices; non-ASCII code points there are written as roff Unicode escapes. The GNU root also renders the original pages with `render_man.sh`.

## Design

`install_root.sh` takes the desktop closure as one layer, including chromium-headless-shell. Its SKIP list preserves the GNU ports and the image's init, accounts and base configuration. Debian's libraries with different sonames, including OpenSSL 3 and FFmpeg 7, coexist with the newer GNU ports. Python 3.13 is installed by its versioned name for Debian's Python modules; the GNU port keeps `python3`. No package maintainer script runs. The image supplies the BLAS and LAPACK alternatives and the default Adwaita cursor theme, builds the GSettings, GdkPixbuf and MIME caches, renders the upstream manual pages and rebuilds its loader cache.

Mesa 25.0.7 supplies llvmpipe, lavapipe, Crocus and hasvk. Lavapipe's Direct3D limit is feature level 11_1 and shader model 6.0 with the pinned Proton. EGL enumerates only DRM devices with a render node. A card-only display uses Pixman in Weston; a program that opens the card itself can still render with GL.

## Updating

Record a new snapshot only after verifying its archive signatures, then update the InRelease pins and compare the closure and the installer's SKIP list. Mesa's required versions are in `mesa.list`. Later layers add list files and name them in their build rule; their install call belongs in `install_root.sh`. Distinct layers must not overwrite files from an earlier layer.
