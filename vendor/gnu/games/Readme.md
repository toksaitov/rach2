# The games layer

The games layer adds to the GNU root what games need beyond the desktop: today innoextract, which unpacks Inno Setup installers such as GOG's offline installers, on both architectures. It is a Debian closure of its own, from the snapshot and keys of the [Debian layer](../debian/Readme.md), installed after the desktop closure, so that the programs and settings the games tasks add have one directory and one install script.

## Use

`innoextract -d DIR SETUP.exe` unpacks an installer and the `SETUP-N.bin` files beside it into `DIR`; `innoextract --list` lists its files and `--version` prints the program's version and the formats it reads. Debian's page is rendered on the machine as `man innoextract`.

### Proton

On x86-64, `rach2-game [--id ID] [--prefix DIR] [--] PROGRAM [ARGUMENT...]` runs a Windows program through `/opt/proton/proton run`, without Steam. `--id` chooses a decimal game ID (the inherited `SteamGameId`, or 0); give each game its own ID or prefix. The default compatibility directory is `$XDG_DATA_HOME/rach2/games/ID`, or `$HOME/.local/share/rach2/games/ID`; Proton keeps its Wine prefix under `pfx` there. `--prefix` chooses that compatibility directory, not its `pfx` child. An inherited `STEAM_COMPAT_DATA_PATH` also chooses it. The directories must be absolute paths. The launcher creates them only when it runs a program; `--print` prints the settings without creating a prefix or starting Proton.

The route sets `PROTON_USE_WOW64=1`, `PROTON_USE_WINED3D=1` and `WINE_D3D_CONFIG=renderer=gl`: GE-Proton11-12's WoW64 and WineD3D on OpenGL, which runs on llvmpipe in QEMU and Crocus on Haswell. `PROTON_NO_FSYNC=0` leaves fsync on, including when the calling shell disabled it. Proton then sets `WINEFSYNC=1`; its Wine tests the kernel's futex support. The launcher sets both `SteamGameId` and `SteamAppId` to the ID and `PROTON_LOG=1`, since Proton writes no log without `SteamGameId`. The log is `steam-ID.log` under `PROTON_LOG_DIR`, which defaults to `logs` in the compatibility directory. Proton replaces that log on each launch. `STEAM_COMPAT_CLIENT_INSTALL_PATH` defaults to a directory named `steam` beside the default game directories; it supplies Proton's required client path without installing Steam. An inherited value is kept.

The pinned DXVK and vkd3d-proton are outside this game route. Under Linux on the Lenovo's HD 4400, hasvk reports Vulkan 1.2.305: DXVK 3.1.1 refuses it for Vulkan 1.3 and then, with that check bypassed, `shaderInt16`; vkd3d-proton 3.1 refuses it for `maintenance5` and `maintenance6`. Venus between the program and hasvk does not remove the refusals. The second discovery run's report, 2026-11-08 to 09, section "Step 10: Venus through QEMU on the HD 4400", records them, with the raw output at `step10/probes-detail.txt`. WineD3D's Vulkan renderer drew small programs under Linux but crashed in Crysis as its timedemo started (the same report, step 8); the game route therefore uses its GL renderer.

## Sources and license

[`layer.list`](layer.list) names the packages, in the Debian layer's list format, each pinned to the version Debian's snapshot has. `make gnu-games` runs `../debian/fetch_debs.sh games` on it, which checks the snapshot's signed indices and every package by its SHA-256, as for the desktop. The stage is `debian/games` in the GNU build tree, with its closure and file lists under `work/debian/games`. innoextract 1.9-0.1+b1 is under the zlib license, and Boost's libraries are mostly under the Boost Software License 1.0; each package keeps its notice at `/usr/share/doc/PACKAGE/copyright`.

## Design

`install_root.sh` sources [`install_games.sh`](install_games.sh) once the desktop closure, Venus, the i386 layer, Proton and the test suites are in the tree, before the agents layer. It installs the games closure as the desktop's is installed: a package of `SKIP` is left out, and so is one the desktop closure installed at the same version, while one it installed at another version, or a file the root already has, stops the image. innoextract's closure is the program and Boost 1.83's filesystem, iostreams and program options libraries; the C and C++ runtimes, zlib, bzip2, liblzma and zstd it needs are the GNU ports'. Since nothing resolved the closure against the root, the script then refuses the image when a library that an ELF file of the layer needs is in none of `/lib`, `/usr/lib` and `/usr/lib/MULTIARCH`.

## Updating

A package is a line of `layer.list` with its version, and the version changes when the Debian layer's snapshot does. The image depends on the list and on every script here, so `make os-real-gnu-ext2` rebuilds the root after an edit.

## Tests

`innoextract --version` on the booted root shows that the program and its libraries load. The GNU family's `game_launcher` row on x86-64 checks `rach2-game --print`, including its settings, an explicit ID and prefix, and a calling shell that disabled fsync or chose Vulkan. It checks that printing creates no compatibility directory.
