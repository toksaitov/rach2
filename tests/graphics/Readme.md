# Graphics tests

The machines of the graphics tests: the QEMU display profiles that Rach2 and its Linux reference boot alike, the accelerators they run under, the Debian 13 reference and builder guest in the same QEMU, and the environment each run sets inside its guest. Every graphics result of Rach2 is judged against the Debian guest's in the same profile, by case and by count, never by the clock.

## Running

The Debian guest is fetched and prepared once for each clone, and each run boots a new copy of it:

    sh tests/graphics/debian-ref/debian-ref.sh fetch
    sh tests/graphics/debian-ref/debian-ref.sh prepare
    sh tests/graphics/debian-ref/debian-ref.sh run -p virtio-vga -g conformance 'lspci -nn'

`fetch` downloads the image that [`debian-ref/image`](debian-ref/image) pins and checks its size and SHA-512. `prepare` boots it once with a cloud-init seed, whose `network-config` is empty, as [`debian-ref/setup.sh.in`](debian-ref/setup.sh.in) says: it points apt at Debian's snapshot of the pinned date, installs [`debian-ref/packages`](debian-ref/packages), Mesa 25.0.7-2+deb13u1 among them, builds vkms, turns cloud-init off so that every later boot is the same, and powers off; the result is `prepared.qcow2`, read-only. It takes a few minutes on the testing PC, most of them apt's downloads. `run` boots an overlay of the prepared image, copies [`guest-env.sh`](guest-env.sh) to `/tmp/guest-env.sh`, runs the command in sh as the user `gfx` over SSH with that file sourced, powers the guest off, removes the overlay and exits with the command's status. Its options are the display profile (`-p`, `std-vga` by default), the CPUs (`-c`, 8), the memory in MiB (`-m`, 8192), the run's class (`-g`, `performance`), `-d` for a Done's run and `-r FILE` for the record. The images and the key that logs in are in `build/cache/debian-ref` of the main clone, which its worktrees share, or in `DEBIAN_REF_CACHE`.

Rach2's run scripts take the same profiles: `DISPLAY_PROFILE=virtio-gpu ARCH=x86_64 sh rach2/run.sh os-simple` boots with that display in place of QEMU's default VGA ([rach2/qemu_devices.sh](../../rach2/qemu_devices.sh), [rach2/run.Readme.md](../../rach2/run.Readme.md)). A machine case names its profile in its QEMU profile's `display` ([etude's display profiles](../../tools/etude/Readme.md#display-profiles)).

## Results

Each Debian run writes a record, to the file `-r` names or to standard error as `debian-ref: record:` lines, one `NAME VALUE` a line: `profile`, `firmware` (`seabios` or `ovmf`), `accelerator`, `cpu` (QEMU's processor), `cpus`, `memory_mib`, `qemu` (its version line), `image` with its SHA-512, `snapshot`, and the guest's environment as `env NAME=VALUE` lines that `graphics_env` prints. A Rach2 case's history records its accelerator, and its QEMU profile, with the display and the CPUs, is in the profile's digest ([etude's Readme](../../tools/etude/Readme.md#the-history-store)).

## Design

### Display profiles

| Profile | QEMU's device | PCI id and class, as Rach2 and Debian read them | Debian boots by | Use |
| --- | --- | --- | --- | --- |
| `std-vga` | `VGA` | 1234:1111, 0300 | SeaBIOS | bochs-drm, fbdrm through GRUB's mode |
| `virtio-vga` | `virtio-vga` | 1af4:1050, 0300 | SeaBIOS | virtio-gpu with a VGA for the firmware and GRUB |
| `virtio-gpu` | `virtio-gpu-pci` | 1af4:1050, 0380 | OVMF | virtio-gpu alone |
| `virgl` | `virtio-vga-gl` with egl-headless | 1af4:1050, 0300 | SeaBIOS | the host's GL through virglrenderer, on the testing PC |
| `none` | none | none | OVMF | vkms inside the guest, which a run loads with `modprobe vkms` |

Each device has `edid=on,xres=1280,yres=800`: its EDID prefers the window's fixed 1280x800, since QEMU's EDID also offers modes up to 5120x2160 and a program such as glmark2-drm takes the largest; the modes stay listed for the mode tests. The pointer is the PS/2 mouse of the PC's 8042, which Rach2 drives; no USB tablet is attached, since Rach2's USB HID takes the boot protocol only.

Debian's GRUB under SeaBIOS resets the machine at once when the display has no VGA, with `virtio-gpu` and with `none` (measured: QEMU ended on the reset before the kernel started), so the Debian guest boots those two by OVMF, where GRUB draws on the firmware's GOP, and the others by SeaBIOS. Rach2's own GRUB, which carries `vbe`, `video` and `video_fb`, was tried once under SeaBIOS with `virtio-gpu` alone: it boots, and the kernel reaches its login prompt in 7 s, but GRUB finds no VBE mode to set and hands the kernel no framebuffer (`boot: screen: text; the loader described none`), where with `std-vga` it hands 1280x800 at 32 bits. A Rach2 run of the `virtio-gpu` profile therefore draws through Rach2's virtio-gpu driver alone, with no firmware framebuffer, and a direct boot does the same.

### Accelerators

Rach2's own suites run under TCG on the testing PC, the fastest there for their boots: Rach2 reaches its login prompt in 0.51 s under TCG against 2.14 s under KVM, whose 300,000 exits of a boot's PCI searches each cost some 4.6 us at the second level of nesting (run 2, step 1). `tools/ci/site`'s `accelerator tcg`, which the release's run and etude's routine entry read, and `RACH2_QEMU_ACCEL=tcg` in each account's `.bashrc` and in `tools/dev/testing.sh run` choose it ([tools/Readme.md](../../tools/Readme.md#the-testing-host)).

A graphics run asks for KVM where `/dev/kvm` opens for reading and writing, and TCG elsewhere: under KVM llvmpipe and lavapipe run 29 to 34 times as fast as under TCG, and the CPU job 11 times. etude takes it from `RACH2_QEMU_ACCEL=kvm` or `-A kvm`; `rach2/qemu_devices.sh` and the Debian guest choose it themselves, unless `ACCEL` names another. Under KVM the processor is QEMU's `host`, with `invtsc=on`: the named AMD models ask for `pcid`, `pku` and `la57`, which WSL's nested KVM lacks, and the invariant counter lets Rach2 measure the time-stamp counter, so that its vDSO reads the clock without a system call. Measured on the testing PC in each profile: with `invtsc=on` Rach2 logs `vdso: clock: the tsc, 4191900 counts a tick` (4,191,892 to 4,191,994), and without it `vdso: clock: the system call; no tsc at one rate was measured`, as under TCG.

### The reference guest

The guest is Debian 13's generic cloud image of 2026-11-03, the one the second discovery run used, with its packages from Debian's snapshot of 2026-11-08, so that every prepared image has the same versions: Mesa 25.0.7-2+deb13u1, Weston 14.0.2 and Linux 6.12.115. Its machine is q35 with the profile's display, a virtio disk, a virtio network card on QEMU's user network with SSH forwarded from a free port on the loopback address, and a virtio random number generator, on the accelerator and processor above. It serves as the reference of every graphics run and as the builder of packages that Rach2 takes from Debian's sources.

Debian 13's kernel is built without vkms (`# CONFIG_DRM_VKMS is not set` in its configuration), so the preparation builds the module from `linux-source-6.12` of the running kernel's own version, 6.12.115-2, against its headers, and installs it in `updates/`; it stays unloaded until a run of the `none` profile loads it, and then appears as `card0` with a virtual and a writeback connector. The kernel's headers stay installed for the builder.

### The guest's environment

[`guest-env.sh`](guest-env.sh) is sourced in the guest of every graphics run, Rach2's or Debian's, with `GRAPHICS_CLASS` set:

* `conformance`, for dEQP-VK, piglit, the WebGL suite and Wine's tests: `LP_NUM_THREADS=0`. These suites run a process on each CPU already, and llvmpipe's own threads buy nothing alone and cost 4 to 5% with five guests (run 2, step 4).
* `vkd3d`, for vkd3d-proton's suite at its measured setting until it is qualified at 0: llvmpipe's threads at the guest's CPU count, and `VKD3D_JOBS`, the suite's processes at once, at the CPU count in a guest of 16 GiB or more and 4 below, since at 8 jobs in 8 GiB the guest's kernel killed tests for memory.
* `performance`, for glmark2, the WebGL fill and ALU gates, vkcube, the game routes and the comparisons of 77-2 and 77-5: `LP_NUM_THREADS` at the guest's CPU count, which both sides of a comparison set alike.

`GRAPHICS_DONE=1`, `-d` of `run`, empties Mesa's shader cache, `MESA_SHADER_CACHE_DIR`, before the run, so that no shader compiled by an earlier run is reused. `graphics_xvfb` starts Xvfb at 1280x800 with `-noreset`, so that it does not reset when its last client leaves, and `graphics_env` prints the environment for the record.

### virgl on the testing PC

The `virgl` profile draws the guest's GL on the testing PC's RTX 4080 SUPER: QEMU's egl-headless display opens WSL's render node, `/dev/dri/renderD128`, a vgem device that only the group `render` may open, and `GALLIUM_DRIVER=d3d12` with `MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA` in QEMU's environment put its GL on Mesa's d3d12 driver, which reaches the GPU through `/dev/dxg`; without them it renders on llvmpipe. It needs no kernel module. A Debian guest under it reports the renderer `virgl` in `eglinfo`. `sh tools/setup/doctor.sh` checks the host's side for the account that runs it ([tools/Readme.md](../../tools/Readme.md#the-testing-host)).

## Limits

* `virgl` is the testing PC's alone: the render node's path and the adapter's name are its own, QEMU on Windows has no such display, and etude refuses it there.
* Venus's frames are black through d3d12, so Venus on screen is a profile of its own on llvmpipe over vgem, with the udmabuf module of WSL's kernel.
* The Debian guest's packages come from the snapshot of one date; a newer reference takes a new date in `debian-ref/image` and a new `prepare`.
