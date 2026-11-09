# Step 8: today's Rach2 on the GNU root, with Debian's graphics userland

Written as the step ran, from 22:20 on 2026-11-08 (UTC+6). The brief is `run2/brief-step8.md`; the rules are `run2/common.md`. Raw outputs are in `run2/step8/out/`, the scripts in `run2/step8/scripts/`; paths below are relative to `run2/step8/`. WSL's clock reads UTC+5 and is quoted as it reads in the raw files; times in this text are UTC+6.

## Where it ran

* The testing PC, WSL (Ubuntu 26.04, kernel 6.18.54.1, QEMU 10.2.1), as the user `testing`. Work directory `/home/testing/rach2/build/run2/step8/`, images in `/home/testing/rach2/build/cache/run2/step8/`.
* Step 2 runs in the same WSL at the same time. This step keeps to 8 threads: one QEMU at a time, of at most 4 vCPUs, and `make`, `xz` and the like at `-j4` or less.

## Rach2's images

* The release copy, `/home/testing/rach2/build/worktrees/release/` at bb151e3ba (committed 2026-11-08 15:30 UTC+6), has the x86-64 kernel and `images/os-real-gnu.ext2` (4 GiB, sparse). Both were copied to `/home/testing/rach2/build/cache/run2/step8/rach2/`; their SHA-256 equal the release's own `release.sha256` (`out/setup1.log`):
  * `kernel` 0effe5acc26e1bf2f20b87b73e92ad8a9d769b708086b5ba700873f7e95e2cad, the one step 1 booted.
  * `os-real-gnu.ext2` 1851d00c3a06ff637204b1c2bf6058129b2b797ee30a970ebf4fba53585c14a3.
* Nothing was missing, so nothing was rebuilt.
* What the root already has (`out/look2.log`, `out/look3.log`, read with `debugfs -R` on the copy): 4 GiB, 652 MiB free (166,875 free 4 KiB blocks), 215,082 free inodes; glibc 2.44 (`vendor/gnu/sources.txt`), newer than Debian 13's 2.41, so Debian's amd64 binaries can run on it; `/etc/ld.so.conf` lists `/usr/lib/i386-linux-gnu` and `/usr/lib/x86_64-linux-gnu`; strace (a port, which `tests/system/gnu_rows.sh` runs on Rach2). Chromium's closure (`work/chromium/closure.txt` of the release's GNU tree) already put part of Debian's graphics stack in `/usr/lib/x86_64-linux-gnu`: `mesa-libgallium`, `libgl1-mesa-dri`, `libglx-mesa0` and `libgbm1` 25.0.7-2+deb13u1, `libllvm19` 1:19.1.7-3+b1, `libvulkan1` 1.4.309.0-1, `libdrm2` 2.4.124-2, `libglvnd0`, `libgl1`, `libglx0`, `libwayland-server0`, the X client libraries. It lacks EGL (`libegl1`, `libegl-mesa0` and the glvnd vendor file), lavapipe (`mesa-vulkan-drivers` and its ICD file), every tool, Weston and Wine.

## Debian's packages

* The packages were chosen and fetched by Debian's own apt in a Debian 13 guest under KVM in WSL: an overlay of step 1's `prep.qcow2`, `/home/testing/rach2/build/cache/run2/step8/deb/deb8.qcow2`, 4 vCPUs, 8 GiB, logged in with step 1's key where it lies (`scripts/wsl-deb-start.sh`, `scripts/gssh.sh`). Its sources are deb.debian.org's trixie, trixie-updates and trixie-backports and security.debian.org's trixie-security, signed by `debian-archive-keyring`.
* A first boot with `-vga none` never got past GRUB: 1,645 lines of `Booting 'Debian GNU/Linux'` in 6 minutes (`out/deb8.try1.serial.log`), the reset loop the first run saw with a display device that has no VGA. With `-vga std` (`scripts/wsl-deb-restart.sh`) it came up in 10 s. Nothing new; 73-16 already says it.
* The closure: apt against an empty dpkg status file, so that it resolves the whole dependency tree as for an empty root, with `--no-install-recommends` (`scripts/g-closure.sh`). Asked for: `libegl1 libegl-mesa0 libgles2 libgl1 libglx-mesa0 libgl1-mesa-dri mesa-libgallium libgbm1 mesa-vulkan-drivers libvulkan1 mesa-utils mesa-utils-bin vulkan-tools weston libweston-14-0 xwayland wine wine64 libwine fonts-wine`, and the libraries libwine only recommends but Wine's display, OpenGL and Vulkan load at run time (`libxcursor1 libxi6 libxrandr2 libxrender1 libxinerama1 libxcomposite1 libxfixes3 libxxf86vm1 libgnutls30t64 libosmesa6 libdbus-1-3`). apt resolved 316 packages (`out/g-closure.log`) and fetched them, 304 MB in 6 min 23 s (`out/g-download.log`).
* Checks: apt checks each file against the SHA-256 of the `Packages` index its signed `InRelease` lists; each was checked again in the guest against `apt-cache show PACKAGE=VERSION` (316 files, 0 mismatches) and once more on the WSL host against that list (`out/stage.log`). The list with sizes and digests is `out/manifest.txt`.
* Versions that matter: Mesa 25.0.7-2+deb13u1 (lavapipe in `mesa-vulkan-drivers`, llvmpipe in `mesa-libgallium`), LLVM 19.1.7-3+b1, the Vulkan loader 1.4.309.0-1, vulkan-tools 1.4.304.0+dfsg1-1, mesa-utils 9.0.0-2+b2, Weston 14.0.2-1, Xwayland 2:24.1.6-1, Wine 10.0~repack-6 (64-bit only: `wine32:i386` is not in the closure, since the guest has no i386 multiarch), libinput 1.28.1, seatd 0.9.1, pixman 0.44.0, FreeRDP 3.15.0 and neatvnc 0.9.1 (Weston's RDP and VNC backends link them), FFmpeg 7.1.5 (libwine depends on its libraries).

## The root

* Unpacked on the WSL host with `dpkg-deb -x`, into `/home/testing/rach2/build/cache/run2/step8/stage/` (`scripts/wsl-stage.sh`): 299 packages; 17 left out by the `SKIP` list that `vendor/gnu/install_root.sh` uses for Chromium, whose libraries the root builds itself (libc6, libgcc-s1, libstdc++6, libatomic1, zlib1g, liblzma5, libpcre2-8-0, libmount1, libblkid1, libuuid1, libffi8, libbz2-1.0, libzstd1, gcc-14-base, debconf, lsb-base, sysvinit-utils). Every file is under `/etc`, `/usr` and `/var`; none under `/bin`, `/lib` or `/sbin`, so nothing had to be moved for the root, which is not merged-`/usr`. 1.4 GiB unpacked.
* The maintainer scripts were not run. The ones the programs here need were done by hand on the stage (`scripts/wsl-configure.sh`): the `wine` alternative and its slaves (`/usr/bin/wine` to `wine-stable`, `wineboot`, `wineserver` and the rest), `libblas.so.3` and `liblapack.so.3`, and libwine's two `zlib1.dll` copies, made as its postinst makes them. Not done: `glib-compile-schemas`, `gdk-pixbuf-query-loaders`, `fc-cache`, `update-mime-database`, seatd's and x11-common's init scripts.
* The image (`scripts/wsl-image.sh`, `scripts/wsl-disk.sh`): the copy of `os-real-gnu.ext2` grown to 12 GiB (`truncate`, `e2fsck -f`, `resize2fs`); a key made for this step (`seed/id_step8`) added to root's `authorized_keys`, and the stage as one tar (1,457,530,880 bytes, SHA-256 efc60076...) written into it, both with `debugfs -w`; then the volume put at 1 MiB in an MBR disk with one partition of type 83, as `tests/system/suites.mk` makes `gnu-test.disk`. A first boot of the bare volume refused to mount it: `ext2: root: no boot disk with a partition table` (`out/rach2-first.serial.log`), which is what `rach2.root=ext2` documents.
* On Rach2, as root over SSH (`scripts/r-install.sh`, `out/r-install.log`): 6,406 files in the tar, 1,284 of which the root already had (Chromium's closure); GNU tar 1.35 unpacked it with `--no-overwrite-dir` in 23 s, the root's `ldconfig` rebuilt the cache, and `ldd` found every library of `eglinfo`, `vulkaninfo`, `vkcube`, `weston`, `Xwayland`, `wine64`, `wineserver64`, `libvulkan_lvp.so` and `libEGL_mesa.so.0`.

## The machine

* Rach2 under KVM in WSL, QEMU 10.2.1 (`scripts/wsl-rach2-boot.sh`): `-accel kvm -nodefaults -display none -no-reboot -cpu max -smp 4 -m 6G -kernel kernel -append "rach2.root=ext2 rach2.trace=enosys rach2.loglevel=4"`, the disk on an `ich9-ahci`, an `rtl8139` on QEMU's user network with port 22 forwarded, the console in a file. `-smp 4` and 6 GiB in place of the GNU cases' 2 and 2 GiB, for lavapipe's threads and Wine. KVM worked; TCG was not needed. `rach2.loglevel=4` puts the trace's warnings on the console as well as in the kernel's ring, which is 16 KiB.
* Rach2 reported 4 processors and 6,128 MiB, and reached SSH 10 s after QEMU started. `uname`: `Linux rach2 6.18.0-rach2 #1 Rach2 1.0.0 x86_64`. Mounted: the ext2 root, `devfs` on `/dev`, `proc`, `tmpfs` on `/dev/shm`, `sysfs` read-only on `/sys`, `mqueue`. `/dev` has no `dri`, `udmabuf`, `fb0` or `uinput`; `/sys/class` has only `backlight`, `ieee80211`, `net`, `rfkill` and `rtc`, `/sys/bus` only `pci` and `usb` (`out/r-survey.log`).
* Before any program of this step ran, the trace already named one call: 334, `rseq`, asked for by every glibc 2.44 program (crond, ssh-keygen, dbus-daemon, sshd, bash). glibc takes the ENOSYS and goes on.

## How each program was run

* One script, `scripts/suite.sh`, run as root on both systems with `LP_NUM_THREADS=4` and a private `XDG_RUNTIME_DIR`: each program under `strace -f -yy -s 256` (strace 7.2 on Rach2, 6.13 on Debian), through `timeout -s INT`, with the kernel's ring cleared before and saved after. Weston 14 runs headless at 1280x800 under its own strace, with the Pixman renderer, then with the GL renderer, then with Pixman and `--xwayland` for Wine, then with Pixman alone for Wine's own Wayland driver. The programs: `eglinfo -B`; `vulkaninfo --summary` and `vulkaninfo`; `glprobe` (`scripts/glprobe.c`: OpenGL ES 2 through EGL, surfaceless and on the EGL device platform, a triangle drawn 100 times into a 64x64 texture and two pixels read back); `vkprobe` (`scripts/vkprobe.c`: a triangle drawn 100 times off screen through a render pass, read back, then a timeline semaphore and the exports of memory as an opaque fd and as a dma-buf, and of a semaphore and a fence as sync files); in each Weston, `weston-simple-shm` for 10 s, `weston-simple-egl` for 15 s, `vkcube --wsi wayland --c 300`, `eglinfo -B -p wayland` (Pixman only) and `weston-simple-dmabuf-egl` (GL only); `wine --version`; `wineboot -i` in a new prefix; `d3d11probe.exe` (the first run's Direct3D 11 program, `tonight/qemu-wsl/guest-src/d3d11probe.c`: it names the DXGI adapter, compiles two shaders with D3DCompile, draws one triangle into a 64x64 texture and reads two pixels back) on WineD3D's OpenGL renderer and, with `WINE_D3D_CONFIG=renderer=vulkan`, its Vulkan renderer; `wineserver -k`. Those are the parts `info probes weston-pixman weston-gl wine`, which run when no part is named. A last part, `wine-wayland`, run after them on each system as `suite.sh /root/st8 /root/bin wine-wayland`, starts Weston on Pixman without Xwayland, unsets `DISPLAY` so that Wine 10 takes its Wayland driver, and runs `d3d11probe.exe` on both renderers again in the same prefix (the `wl-` rows).
* The probes were built in the Debian guest (GCC 14.2, mingw-w64 GCC 14, glslang 15.2.0; `out/g-build.log`): `glprobe` 1413309d..., `vkprobe` a3879fd1..., `d3d11probe.exe` 5fef6e6e....
* `scripts/analyze.py` reads both systems' traces of each program and lists every call that failed on Rach2, by kind (system calls the kernel lacks, ioctls, paths under `/dev`, `/proc` and `/sys`, other paths, extended attributes, other failures with their flags), each with how the same call ended on Debian: it succeeded, it failed with the same or another error, or Debian never made it. A failure that Debian has too is not counted as a gap.

## Debian's run, the reference

* The same `suite.sh`, as root in the Debian 13 guest above (kernel 6.12.115+deb13.1, as Weston's log reads it), on the same packages, before Rach2's run (`out/debian/`). The guest had `-vga std`, so Linux's bochs-drm gave it `/dev/dri/card0` but no render node, and Debian's kernel has `/dev/udmabuf` (character 10:258). Each program's `rc.txt` reads:

| Program | Exit | Seconds under strace | What it printed |
|---|---|---|---|
| `eglinfo -B` | 3 | 0.66 | surfaceless and device platforms: llvmpipe, OpenGL 4.5, OpenGL ES 3.2; GBM, Wayland and X11 fail `eglInitialize` (no compositor, no render node), hence 3 |
| `vulkaninfo --summary`, `vulkaninfo` | 0, 0 | 0.54, 0.14 | one device, llvmpipe, API 1.4.305, conformance 1.3.1.1 |
| `glprobe surfaceless 100`, `glprobe device 10` | 0, 0 | 0.34, 0.11 | `draw OK` on both platforms |
| `vkprobe 100` | 0 | 0.60 | `draw OK`; memory export as opaque fd and dma-buf, features 0x6 (export and import); semaphore and fence export as sync files, features 0x3 |
| Weston Pixman: `weston-simple-shm`, `weston-simple-egl` | 124, 124 | 10.01, 15.02 | ran until `timeout` stopped them, as they are meant to |
| Weston Pixman: `vkcube --wsi wayland --c 300`, `eglinfo -B -p wayland` | 0, 0 | 8.12, 0.26 | 300 frames on llvmpipe; OpenGL 4.5 over Wayland |
| Weston GL: `weston-simple-shm`, `weston-simple-egl` | 124, 124 | 10.01, 15.02 | as under Pixman |
| Weston GL: `weston-simple-dmabuf-egl` | 1 | 0.02 | `Failed to open drm render node /dev/dri/renderD128`: the guest has none |
| Weston GL: `vkcube` | 0 | 7.95 | 300 frames |
| `wine --version` | 0 | 0.03 | `wine-10.0 (Debian 10.0~repack-6)` |
| `wineboot -i` | 0 | 33.73 | a new prefix; the same `err:ole` and `err:setupapi` lines as on any Wine 10, and `c0000135` for `syswow64\rundll32.exe`, since there is no 32-bit Wine |
| `d3d11probe.exe`, OpenGL renderer | 0 | 8.07 | adapter "NVIDIA GeForce GTX 470" (WineD3D's name for llvmpipe), 7947 MiB; feature level 0xb000; `draw OK` |
| `d3d11probe.exe`, Vulkan renderer | 0 | 8.93 | `Using the Vulkan renderer`; adapter "llvmpipe (LLVM 19.1.7, 256 bits)", vendor 0x10005; feature level 0xb000; `draw OK` |
| `wineserver -k` | 1 | 0.02 | no server left to kill |
| Wayland driver: `d3d11probe.exe`, OpenGL renderer | 0 | 6.73 | adapter "NVIDIA GeForce GTX 470", 1280 MiB; `draw OK` |
| Wayland driver: `d3d11probe.exe`, Vulkan renderer | 0 | 6.60 | adapter llvmpipe; `draw OK` |

* Weston's logs: under GL, `explicit sync: yes` and `dmabuf support: modifiers`, with `failed to query rendering device from EGL`; under Pixman, `explicit sync: no`. The presentation clock is `CLOCK_MONOTONIC_RAW` in both.
* Across all of Debian's traces the programs made 131 different system calls (`out/debian-syscalls.txt`). Rach2's x86-64 table (`rach2/sys/arch/x86_64/systab.c`, 267 entries) has 130 of them. The one it lacks is `rseq`.
* How lavapipe uses `/dev/udmabuf` on Debian (`out/debian/vkprobe/strace.txt.gz`, lines 1458 to 1466 and 24037): when it makes a device it opens `/dev/udmabuf`, makes a sealed memfd, asks `UDMABUF_CREATE` for a dma-buf over it and `DMA_BUF_IOCTL_EXPORT_SYNC_FILE` on that dma-buf; when `vkprobe` exports memory as a dma-buf, it asks `UDMABUF_CREATE` again. Every program here that loads llvmpipe or lavapipe makes the probe (`UDMABUF_CREATE` in 17 of the 26 traces): all but the three Westons on Pixman, both `weston-simple-shm`, `weston-simple-dmabuf-egl`, `wine --version` and the two `wineserver -k`. `wineboot` makes it too, through Wine's display driver.

## Rach2's run

* One run of the five parts on a fresh boot of the disk (`boot3`, 22:55), then the `wine-wayland` part (23:03 to 23:05), as root over SSH: `setsid nohup bash /root/bin/suite.sh /root/st8 /root/bin > /root/suite.log`. Outputs are in `out/rach2/`, one directory per program, with `out/rach2/suite.log`, `out/rach2/suite-wl.log` and the console log `out/rach2-boot3.serial.log`. An earlier boot's results are not used: it ran two copies of the suite at once (an SSH session the Mac had given up on kept its job running in WSL), and they fought over Weston's socket lock; `out/rach2-boot2-dup.serial.log` is that boot's console. `suite.sh` now takes a lock (`flock -n` on `/tmp/suite-st8.lock`) and refuses a second run.
* Every program ended as it did on Debian: the same exit status in all 26 rows, and the same pixels where a program reads them back. The exits, with Rach2's seconds under strace:

| Program | Exit, Rach2 | Seconds, Rach2 (Debian) | What it printed |
|---|---|---|---|
| `eglinfo -B` | 3 | 3.54 (0.66) | as Debian: surfaceless and device platforms work, GBM, Wayland and X11 fail `eglInitialize` |
| `vulkaninfo --summary`, `vulkaninfo` | 0, 0 | 2.45, 0.46 | llvmpipe, API 1.4.305, conformance 1.3.1.1 |
| `glprobe surfaceless 100`, `glprobe device 10` | 0, 0 | 1.63, 0.39 | `draw OK`, center 255 128 0 255, corner 0 0 128 255 |
| `vkprobe 100` | 0 | 2.89 (0.60) | `draw OK`; timeline semaphore wait 0; export differs, below |
| Pixman: `weston-simple-shm`, `weston-simple-egl` | 124, 124 | 10.10, 15.03 | ran until stopped |
| Pixman: `vkcube`, `eglinfo -B -p wayland` | 0, 0 | 11.03, 1.21 | 300 frames; OpenGL 4.5 over Wayland |
| GL: `weston-simple-shm`, `weston-simple-egl` | 124, 124 | 10.26, 15.07 | ran until stopped |
| GL: `weston-simple-dmabuf-egl` | 1 | 0.10 | `Failed to open drm render node /dev/dri/renderD128`, as on Debian |
| GL: `vkcube` | 0 | 9.94 (7.95) | 300 frames |
| `wine --version` | 0 | 0.12 | `wine-10.0 (Debian 10.0~repack-6)` |
| `wineboot -i` | 0 | 209.54 (33.73) | a new prefix, with Debian's messages |
| `d3d11probe.exe`, OpenGL, Xwayland | 0 | 57.54 (8.07) | "NVIDIA GeForce GTX 470", 6128 MiB (the guest's memory, as 7947 MiB is Debian's); feature level 0xb000; `draw OK` |
| `d3d11probe.exe`, Vulkan, Xwayland | 0 | 58.98 (8.93) | `Using the Vulkan renderer`; adapter llvmpipe, vendor 0x10005; `draw OK` |
| `wineserver -k` | 1 | 0.08 | as Debian |
| `d3d11probe.exe`, OpenGL, Wayland driver | 0 | 46.12 (6.73) | 1280 MiB, as on Debian; `draw OK`; `winewayland.so` and `libEGL_mesa.so.0` loaded, no X11 socket opened |
| `d3d11probe.exe`, Vulkan, Wayland driver | 0 | 44.96 (6.60) | adapter llvmpipe; `draw OK` |

* The seconds measure strace, not the kernel, whose speed this step did not measure. Rach2 lacks `process_vm_readv` (the trace names it 26 times, all for strace), so strace 7.2 falls back to reading the tracee's memory through ptrace a word at a time. Wine's runs, which pass many paths, took 6.2 to 7.1 times Debian's; `vkcube` took 1.25 and 1.36 times.
* Weston's logs differ from Debian's here. Under GL: `warning: Disabling render GPU timeline and explicit synchronization due to missing EGL_ANDROID_native_fence_sync extension`, and `explicit sync: no` where Debian has `yes`; one `unexpectedly large timestamp jump (from 71361 to 73340)`. Under every renderer: `presentation clock resolution: 0.000000010 s`, where Debian's is 1 ns, from `clock_getres`. With Xwayland: `Detected an unmapped surface or view in the layer list, which should not occur`, once, while Wine ran.
* Xwayland logged `unlinking stale lock file /tmp/.X0-lock`: `/tmp` is on the ext2 root, so the duplicate run's lock survived the reboot. Xwayland removed it and went on.

## What Rach2 lacked, counted

* `out/analyze.txt` is `scripts/analyze.py out/rach2 out/debian`: for each program, each call that failed on Rach2, with its count and how the same call ended on Debian. A call that failed the same way on Debian is not listed. Set aside, and counted under "set aside" for each program: a terminal request on a descriptor that is no terminal (ENOTTY on Linux too; glibc 2.44 asks `TCGETS2` where Debian's 2.41 asks `TCGETS`), `/dev/tty` with no controlling terminal, waits that were interrupted, timed out or raced, the home directory, Wine prefix and `/tmp` holding different leftovers, searches of `PATH` and of the loader's directories, and Wine's `VFAT_IOCTL_READDIR_BOTH` on directories, ENOTTY on both. `out/analyze.txt` keeps one list a program; the table below sums it by kernel subsystem. Calls are strace's counts over the whole run of each program; "programs" counts the 26 rows that made the call.
* The trace itself (`rach2.trace=enosys`) logged 693 lines over the whole boot (`out/rach2-boot3.serial.log`): `rseq` (334) 642 times, `process_vm_readv` (310) 26, all by strace, `get_mempolicy` (239) 15, by procps's `ps`, `pkill` and `pgrep` and once by Wine's `rundll32.exe` (libnuma, which takes the ENOSYS), and `name_to_handle_at` (303) 10, all by `winedevice.exe`. It logs a number once each time another process asks for it (`rach2/sys/syscall.c`, `syscall_missing`), so its counts are of processes; `out/enosys.txt` (`scripts/enosys.py`) splits them by program. No other warning, oops or fault is in the console log.
* Across all 26 traces Rach2's programs made 136 different calls and Debian's 131 (`out/rach2-syscalls.txt`, `out/debian-syscalls.txt`). Rach2's extra ones: `clock_gettime`, `gettimeofday`, `time` and `clock_getres`, which Debian answers in the vDSO, `get_mempolicy`, `name_to_handle_at`, and `syscall_0xffffffffffffffff`, below.

| Subsystem | What failed | Calls | Programs | Debian | Who asked, and what the trace shows next |
|---|---|---|---|---|---|
| System call table | `rseq`, ENOSYS | 145 | 26 | ok | glibc, in every process; it went on |
| System call table | `name_to_handle_at`, ENOSYS | 10 | 5, Wine | never made | `winedevice.exe` (`winebus.sys`), after `statx` of `/dev` asking `STATX_MNT_ID`; it then opens `/proc/self/fdinfo`, below |
| System call table | `get_mempolicy`, ENOSYS | 1 | `wineboot` | never made | libnuma's start in `rundll32.exe`; it opens `/sys/devices/system/node` next |
| Scheduler | `sched_setscheduler(tid, SCHED_BATCH, 0)`, EINVAL | 17 | 16, those that start Mesa's worker threads | ok | on a thread Mesa has just made, right after its `clone3`; it went on |
| Memory | `madvise(MADV_GUARD_INSTALL)`, EINVAL | 22 | 18 | never made | glibc 2.44, on a new thread's stack; the next call is `mprotect(PROT_NONE)` on the same page. Debian's glibc 2.41 does not ask |
| Device nodes, DRM | `/dev/dri`, ENOENT | 202 | 18 | ok (a card, no render node) | Mesa and the Vulkan loader; they went on to llvmpipe and lavapipe |
| Device nodes, sharing | `/dev/udmabuf`, ENOENT | 27 | 17 | ok | llvmpipe and lavapipe; they offer less, below |
| Device nodes, serial | `access /dev/ttyS0`, ENOENT | 5 | 5, Wine | ok | Wine; it went on |
| sysfs, DRM | `/sys/class/drm/card0/device/boot_vga`, ENOENT | 43 | 10, those that load Vulkan | ok | Mesa's device-select layer, the library loaded last before the read; it went on |
| sysfs, CPU | `/sys/devices/system/cpu/cpuN/cache/indexM/shared_cpu_map`, ENOENT | 1,008 | 5, Wine | ok | Wine; it went on |
| sysfs, firmware | `/sys/class/dmi/id/*` (13 files), ENOENT | 78 | 5, Wine | ok | Wine; it went on |
| sysfs, NUMA | `/sys/devices/system/node/online` and `node`, ENOENT | 64 | 5, Wine | ok | Wine; it went on |
| sysfs, clocks | `/sys/bus/clocksource/devices/clocksource0/current_clocksource`, ENOENT | 6 | 5, Wine | ok | Wine; it went on |
| procfs | `open("/proc/PID/task/TID/comm", O_WRONLY)`, EACCES | 183 | 5, Wine | ok | Wine, naming its threads; it went on |
| procfs | `/proc/sys/kernel/cap_last_cap`, ENOENT | 4 | 4, Weston | ok | Weston; it went on |
| procfs | `/proc/sys/crypto/fips_enabled`, ENOENT | 1 | Xwayland | ok | it went on |
| File system, ext2 | `getxattr`, `fgetxattr`, `fsetxattr`, `fremovexattr` of `user.DOSATTRIB`, EOPNOTSUPP | 6,400 | 5, Wine | ENODATA, and the 3 sets succeed | Wine, which keeps DOS file attributes there; it went on. `rach2/sys/fs/ext2.Readme.md` says the driver neither reads nor writes extended attributes |
| Root contents, not the kernel | `/usr/local/share/fonts`, `/run/udev/control`, `/etc/host.conf`, `/var/lib/dbus/machine-id`, `/usr/share/mime/globs` | 205 | Weston, Wine | ok | the GNU root lacks these files, and no udev runs |

* No ioctl failed on Rach2 where it did not fail on Debian. Rach2's programs asked 9 requests in all: `VFAT_IOCTL_READDIR_BOTH` (3,565, ENOTTY), `FS_IOC_GETFLAGS` (836), `TCGETS2` (158, ENOTTY), `TIOCGPGRP` (31, ENOTTY), `SIOCGIFHWADDR`, `SIOCGIFFLAGS` and `SIOCGIFMTU` (8 each), `SIOCGIWNAME` (4, EOPNOTSUPP, as on Debian) and `DMA_BUF_IOCTL_EXPORT_SYNC_FILE` on a bad descriptor (2, EBADF, as on Debian). No program reached a graphics or input device to ask one.
* What Debian reached that Rach2 never asked for, because the device was absent (`out/unreached.txt`, `scripts/unreached.py`): `UDMABUF_CREATE` on `/dev/udmabuf` and `DMA_BUF_IOCTL_EXPORT_SYNC_FILE` on the dma-buf it made (17 programs each); the DRM identity of `card0` in sysfs, `/sys/dev/char/226:0` and its `device`, `subsystem` and `drm` links and the PCI device's `vendor`, `device`, `subsystem_vendor`, `subsystem_device` and `uevent` (18 programs); `/sys/class/drm/card0/device/config` (10); and in the 5 Wine runs, libudev's walk of `/sys/class/input` and `/sys/devices/platform/i8042/...` with each node's `uevent`, then `/dev/input/event0` to `event3` and `mouse0` and `mouse1` opened by `winebus.sys` with `EVIOCGID`, `EVIOCGNAME`, `EVIOCGBIT`, `EVIOCGUNIQ` and `EVIOCGABS` (65 such ioctls in Debian's `wine-d3d11probe`). On Rach2 Wine opened nothing under `/dev/input`: `/sys/class` has no `input`, so libudev lists no device, though Rach2 has `/dev/input` and evdev.
* What changed with the missing sharing device, read from the programs' own output:
  * lavapipe offers 177 device extension lines in `vulkaninfo` against Debian's 180; the three it drops are `VK_KHR_external_semaphore_fd`, `VK_KHR_external_fence_fd` and `VK_EXT_image_drm_format_modifier`.
  * `vkprobe`: memory export as a dma-buf has features 0x4 (import only) against Debian's 0x6; export of a semaphore or a fence as a sync file has features 0 against 0x3. Export as an opaque fd works on both (0x6), and so does the timeline semaphore.
  * EGL (`eglinfo -p surfaceless`, `out/eglx/`): Rach2 lacks `EGL_ANDROID_native_fence_sync` and `EGL_MESA_image_dma_buf_export`; the other 46 extension names in the output are Debian's.
  * Weston on GL turns off explicit synchronization and its render timeline, as quoted above.
  * Lavapipe's probe for all of this is the one in Debian's trace: a sealed memfd, `UDMABUF_CREATE`, `DMA_BUF_IOCTL_EXPORT_SYNC_FILE`. Rach2 already has the first half: `memfd_create` with `MFD_ALLOW_SEALING` and `F_ADD_SEALS` (`rach2/sys/fs/tmpfs.c`, `rach2/sys/syscall_fs.c`).

## Other differences from Linux the traces show

* The vDSO's clock is off in this machine: the console says `vdso: clock: the system call; no tsc at one rate was measured`, after `smp: cpu1: tsc: not checked: no rate was measured`. So `clock_gettime` (300,852 calls) and `gettimeofday` (277,584) entered the kernel, where Debian's never did. QEMU's `-cpu max` under KVM in WSL gives no invariant TSC unless asked; whether `-cpu max,+invtsc` turns the vDSO clock on is untested.
* A descriptor's name in `/proc/PID/fd` is the path it was opened by, through symbolic links, where Linux gives the file's own path: Wine opens its prefix through `dosdevices/c:` and `dosdevices/z:`, and Rach2 names those descriptors `/root/st8-prefix/dosdevices/c:/windows`, where Debian names them `/root/st8-prefix/drive_c/windows`. `analyze.py` maps one to the other before comparing.
* A descriptor opened on `/proc/self/fdinfo/29` is named `/proc/580/stat` in `/proc/PID/fd` (`out/rach2/wineboot/strace.txt.gz`, the lines after `name_to_handle_at`), and the file it reads holds `pos` and `flags` but no `mnt_id`. With `statx` not filling `STATX_MNT_ID` either (it returned `STATX_BASIC_STATS` when asked for it) and no `name_to_handle_at`, libudev cannot tell whether `/dev` is a mount point. Debian answers the first question from `statx`.
* At an `exit_group`, a thread of the group that was entering a call is reported to strace with call number -1 (`syscall_0xffffffffffffffff`, 6 times, in three Wine runs), where Linux's strace shows the call it was entering.
* `dpkg --print-architecture` in `/usr/bin/wine`'s script reaches BusyBox's `dpkg`, which prints its usage; the script goes on. A root fault, not the kernel's.

## The missing kernel pieces, by program and plan task

* Every program ran and drew correctly, so "missing" here means what Rach2 must add for each program to behave as on Debian. The plan's tasks are named as `plan.md` (draft 5) names them.

| Piece | Programs that ask for it | Plan task | What its absence costs today |
|---|---|---|---|
| `/dev/udmabuf` (`UDMABUF_CREATE` over a sealed memfd), dma-buf descriptors with `DMA_BUF_IOCTL_EXPORT_SYNC_FILE`, and the sync files they give | every program that loads llvmpipe or lavapipe: `eglinfo`, `glprobe`, `vkprobe`, `vulkaninfo`, Weston on GL, `weston-simple-egl`, `vkcube`, Wine's Direct3D runs | 73-3 | lavapipe drops `VK_KHR_external_semaphore_fd`, `VK_KHR_external_fence_fd` and `VK_EXT_image_drm_format_modifier`, and exports dma-bufs and sync files no more; EGL drops `EGL_ANDROID_native_fence_sync` and `EGL_MESA_image_dma_buf_export`; Weston on GL has no explicit synchronization |
| DRM card and render nodes under `/dev/dri` | 18 programs probe it; `weston-simple-dmabuf-egl` needs a render node | 73-4, with a driver (73-6, 73-8) | nothing for rendering: Mesa takes llvmpipe as on a Linux with no GPU; GBM and `weston-simple-dmabuf-egl` fail on both systems |
| The DRM device's identity in sysfs: `/sys/class/drm/cardN/device/boot_vga` and `config`, `/sys/dev/char/226:N` with its `device`, `subsystem` and `drm` links, the PCI ids | Mesa's loader, the device-select layer (18 programs on Debian; `boot_vga` 43 times on Rach2 even with no card) | 73-1 (identity), 73-9 | nothing until a card exists |
| `/sys/class/input` and the input devices' `uevent` files, read by libudev | Wine's `winebus.sys` (5 runs) | 73-9, then 77-4 | Wine opens no input device, though Rach2 has `/dev/input` |
| Writable `/proc/PID/task/TID/comm` | Wine (183 refusals) | none | thread names |
| `/proc/sys/kernel/cap_last_cap`, `/proc/sys/crypto/fips_enabled` | Weston, Xwayland | none | none seen |
| `/sys/devices/system/cpu/cpuN/cache/*`, `/sys/devices/system/node/*`, `/sys/class/dmi/id/*`, `/sys/bus/clocksource/...` | Wine (1,156 refused opens) | none | Wine's view of the processor's caches, NUMA, the firmware and the clock source |
| User extended attributes on ext2 | Wine, `user.DOSATTRIB` (6,400 calls) | none (77-6 puts Crysis on 73-15's data partition) | DOS attributes, such as hidden and system, are not kept |
| `STATX_MNT_ID`, `mnt_id` in `/proc/PID/fdinfo/N`, `name_to_handle_at`; and a descriptor opened on `fdinfo` named `/proc/PID/stat` | libudev in Wine | none | libudev cannot tell a mount point |
| `/proc/PID/fd` naming the file's own path, not the path it was opened by | Wine's prefix links | none | Wine's view of a handle's path differs from Linux's |
| `rseq` | every glibc process | none | none seen |
| `SCHED_BATCH` (and `SCHED_IDLE`) for anyone, as Linux grants them | Mesa's worker threads (16 runs) | none | Mesa's worker threads run at the normal policy |
| `MADV_GUARD_INSTALL` | glibc 2.44 on each new thread | none | glibc uses `mprotect` |
| `get_mempolicy` | libnuma (procps, Wine) | none | none seen |
| `process_vm_readv` | strace | none | strace reads a word at a time |
| A vDSO clock in QEMU under KVM in WSL | everything; 578,436 time calls entered the kernel here | 73-16 (the profile) | speed only |

## How this changes milestones 73, 76 and 77

* 73 display (the plan says about 11,000 lines). The software path that 73's goal rests on, llvmpipe and lavapipe under Weston 14 with Xwayland, already runs on today's kernel with Debian's binaries unchanged, This removes no task of 73, since none was for rendering in software, and adds none for it. The step confirms 73-3's `/dev/udmabuf` as the one kernel interface the software stack probes, and finds its base, sealed memfds, already in the kernel; it gives 73-9 a measured list of the sysfs files Mesa's loader and libudev read. It does not measure 73-4 to 73-8, 73-10 to 73-12 or 73-18: a headless Weston opens no DRM device and no input device, so the step says nothing about their size. Seven small differences from Linux that no task holds (`rseq`, `SCHED_BATCH` and `SCHED_IDLE`, `MADV_GUARD_INSTALL`, `get_mempolicy`, `process_vm_readv`, `cap_last_cap`, `fips_enabled`) fit one small task of 73, since AGENTS.md has a limit Linux lacks fixed in the milestone under way; by my estimate, not measured, about 800 to 1,300 lines, most of it `rseq` and `process_vm_readv`. The vDSO clock is a change of 73-16's QEMU profile, to be tried.
* 76 ia32 (about 5,200 lines). No change, and no evidence: Debian's Wine 10.0 here is 64-bit only (`c0000135` for `syswow64\rundll32.exe` on both systems), so nothing 32-bit ran. The step shows only that 64-bit Wine needs nothing of 76.
* 77 games. 77-3's 64-bit half is done on today's kernel: a new prefix and a Direct3D 11 program on WineD3D's OpenGL and Vulkan renderers, through Xwayland and through Wine's Wayland driver, with the pixels right. Its kernel work is left to 76-1 for 32-bit programs, and `futex_waitv`, which Debian's Wine does not use and this step did not reach. The Wine-only gaps above belong to no task of 77 and would form one: by my estimate, not measured, about 1,500 to 2,700 lines, the most for user extended attributes on ext2 (which 77-6 may need for an installer that sets DOS attributes on the data partition), then the CPU, NUMA, firmware and clock files of sysfs, the mount id, and thread names. Wine's input (77-4) waits on 73-9's `/sys/class/input`. WineD3D reported the guest's memory as video memory (6128 MiB of 6 GiB), so 77-3's `VideoMemorySize` stays a virgl matter.

## Findings

1. Today's kernel runs the whole software graphics stack of Debian 13 unchanged: Mesa 25.0.7's llvmpipe and lavapipe, Weston 14 headless on Pixman and on GL, Xwayland, and Wine 10.0 with WineD3D on OpenGL and on Vulkan, through Xwayland and through Wine's Wayland driver. All 26 runs end with Debian's exit status, and every pixel the probes read back matches. This changes the plan's view of 73 and 77: no kernel work is needed to render and composite in software, 73-13's packages can be brought up and checked on today's kernel with a headless Weston, and 77-3's 64-bit half is done; 73's size is in the display, DRM and input tasks, which this step did not reach.
2. The system call table is not where the gaps are. Debian's runs made 131 different calls; Rach2's x86-64 table has 130 of them, and the trace named only `rseq`, `name_to_handle_at`, `get_mempolicy` and, for strace, `process_vm_readv` in 693 lines. No ioctl failed on Rach2 where it did not on Debian. The gaps are device nodes, sysfs and procfs files, two flags, extended attributes on ext2 and the mount id, so the plan's sizes should be argued per device and per file, not per call.
3. `/dev/udmabuf` with dma-buf `EXPORT_SYNC_FILE` (73-3) is the one interface the software stack probes, in 17 of the 26 runs. Without it lavapipe offers 177 extension lines against 180, dropping `VK_KHR_external_semaphore_fd`, `VK_KHR_external_fence_fd` and `VK_EXT_image_drm_format_modifier`; dma-buf export is import-only (features 0x4 against 0x6) and sync-file export has features 0 against 0x3; EGL lacks `EGL_ANDROID_native_fence_sync` and `EGL_MESA_image_dma_buf_export`; and Weston on GL turns off explicit synchronization. Sealed memfds, which udmabuf wraps, already exist. This argues for building udmabuf first in 73-3, ahead of syncobjs, since it serves llvmpipe with no DRM driver at all; the plan already names it for Rach2 as a Venus host in 80-2.
4. Wine's gaps lie outside the graphics plan. On ext2, 6,400 extended-attribute calls for `user.DOSATTRIB` fail with EOPNOTSUPP where Debian's give ENODATA; 183 writes of thread names to `/proc/PID/task/TID/comm` are refused with EACCES; 1,156 opens of CPU cache, NUMA, firmware (DMI) and clock-source files in sysfs fail; libudev lacks a mount id (`STATX_MNT_ID`, `fdinfo`'s `mnt_id`, `name_to_handle_at`); and with no `/sys/class/input`, Wine finds no input device, where Debian's Wine opened four event devices and two mice. This adds one task to 77 (estimated at 1,500 to 2,700 lines, not measured) and makes 77-4 wait on 73-9's input entries.
5. Small differences from Linux hit most programs and belong to no task: `rseq` is ENOSYS for every glibc process, `SCHED_BATCH` is refused with EINVAL 17 times in 16 runs (the kernel's own comment in `sched_set` says Linux grants it to anyone), `MADV_GUARD_INSTALL` is EINVAL for glibc 2.44, and `/proc/sys/kernel/cap_last_cap` is missing for Weston. Also, in QEMU under KVM in WSL the vDSO clock is off (`no tsc at one rate was measured`), so 578,436 time calls entered the kernel. The first four make one small task of 73 (estimated at 800 to 1,300 lines with `get_mempolicy` and `process_vm_readv`); the clock is for 73-16's profile, which should try `-cpu max,+invtsc`.

## Files

* On the Mac, under `build/flow/coordinator/graphics-research/run2/step8/` (39 MB in all):
  * `scripts/`: every script and probe source of the step (200 KB). `suite.sh` (SHA-256 7017eb46... as run in the `wine-wayland` part; the five parts ran the same script before that part was added), `analyze.py`, `unreached.py`, `enosys.py`, `glprobe.c`, `vkprobe.c`, `tri.vert`, `tri.frag`, `d3d11probe.c`, and the `wsl-*`, `g-*`, `r-*` scripts that set up the guest, the root and the disk.
  * `out/rach2/` (21 MB) and `out/debian/` (17 MB): one directory per program, with `strace.txt.gz`, `out.txt`, `rc.txt` and `dmesg.txt`, and `weston.log` for each Weston. `analyze.py` and `unreached.py` read the gzipped traces.
  * `out/analyze.txt`, `out/unreached.txt`, `out/enosys.txt`, `out/rach2-syscalls.txt`, `out/debian-syscalls.txt`, `out/unistd_64.h` (WSL's, for the numbers), `out/eglx/` (full `eglinfo` of both systems), the console logs (`rach2-first`, `rach2-boot2-dup`, `rach2-boot3`, `deb8.try1`, `deb8`), and the setup logs named above.
* On the testing PC, in WSL, under `/home/testing/rach2/build/cache/run2/step8/` (14 GiB on disk):
  * `rach2/kernel` (2,845,120 bytes) and `rach2/os-real-gnu.ext2` (4 GiB, 3.4 GiB on disk), the release copy's, with `rach2/SHA256SUMS`.
  * `rach2/gnu-deb.disk`: the booted disk, an MBR disk of 12,886,999,040 bytes (7.1 GiB on disk) holding the grown root with Debian's packages, the probes in `/root/bin`, the results in `/root/st8` and the Wine prefix `/root/st8-prefix`. `e2fsck -fn` after the last power-off: clean (`out/e2fsck-final.log`).
  * `deb/deb8.qcow2` (3.3 GiB): the Debian guest's overlay on step 1's `prep.qcow2`, with the probe build and the reference results in `/root/st8`.
  * `debs/` (291 MB, 316 packages), `manifest.txt`, `debs.sha256`, `requested.txt`, `closure-sim.txt`, `staged.txt`, `skipped.txt`, `payload.sha256`.
  * `bin/` (432 KB): `glprobe`, `vkprobe` and `d3d11probe.exe` as built, with their sources and `suite.sh`.
  * `seed/`: the step's own SSH key, whose public half is in the disk's `/root/.ssh/authorized_keys`.
* Removed: the work directory `/home/testing/rach2/build/run2/step8/`, the unpacked stage (1.4 GiB), the tar of it (1.4 GB) and the packages' control files, all remade from `debs/` by `scripts/wsl-stage.sh` and `scripts/wsl-image.sh`; and `D:/rach2/build/run2/step8/`. Both QEMUs were powered off from inside; no process of the step is left in WSL or on Windows.

Finished 23:20 on 2026-11-08 (UTC+6).
