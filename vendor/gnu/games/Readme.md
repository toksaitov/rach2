# The games layer

The games layer adds to the GNU root what games need beyond the desktop: today innoextract, which unpacks Inno Setup installers such as GOG's offline installers, on both architectures. It is a Debian closure of its own, from the snapshot and keys of the [Debian layer](../debian/Readme.md), installed after the desktop closure, so that the programs and settings the games tasks add have one directory and one install script.

## Use

`innoextract -d DIR SETUP.exe` unpacks an installer and the `SETUP-N.bin` files beside it into `DIR`; `innoextract --list` lists its files and `--version` prints the program's version and the formats it reads. Debian's page is rendered on the machine as `man innoextract`.

## Sources and license

[`layer.list`](layer.list) names the packages, in the Debian layer's list format, each pinned to the version Debian's snapshot has. `make gnu-games` runs `../debian/fetch_debs.sh games` on it, which checks the snapshot's signed indices and every package by its SHA-256, as for the desktop. The stage is `debian/games` in the GNU build tree, with its closure and file lists under `work/debian/games`. innoextract 1.9-0.1+b1 is under the zlib license, and Boost's libraries are mostly under the Boost Software License 1.0; each package keeps its notice at `/usr/share/doc/PACKAGE/copyright`.

## Design

`install_root.sh` sources [`install_games.sh`](install_games.sh) once the desktop closure, Venus, the i386 layer, Proton and the test suites are in the tree, before the agents layer. It installs the games closure as the desktop's is installed: a package of `SKIP` is left out, and so is one the desktop closure installed at the same version, while one it installed at another version, or a file the root already has, stops the image. innoextract's closure is the program and Boost 1.83's filesystem, iostreams and program options libraries; the C and C++ runtimes, zlib, bzip2, liblzma and zstd it needs are the GNU ports'. Since nothing resolved the closure against the root, the script then refuses the image when a library that an ELF file of the layer needs is in none of `/lib`, `/usr/lib` and `/usr/lib/MULTIARCH`.

## Updating

A package is a line of `layer.list` with its version, and the version changes when the Debian layer's snapshot does. The image depends on the list and on every script here, so `make os-real-gnu-ext2` rebuilds the root after an edit.

## Tests

No suite runs the layer yet. `innoextract --version` on the booted root shows that the program and its libraries load.
