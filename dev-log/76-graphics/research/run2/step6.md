# Step 6: Venus on the host's lavapipe with the udmabuf module, in WSL

Written as the step ran, from 08:02 on 2026-11-09 (UTC+6). The brief is `run2/brief-step6.md`; the rules are `run2/common.md`. Raw outputs are in `run2/step6/out/`, the scripts in `run2/step6/scripts/` (`look*.sh` and `wsl-*.sh` run in WSL, `g-*.sh` in the guest). WSL's clock reads UTC+5 and the guest's UTC; times here are UTC+6 unless marked.

## The host at the start

* 08:03 (`scripts/look0.sh` as root, `out/look0.txt`): WSL up 14 h 39 min, load average 0.02, 0.33, 1.92 (falling from step 5-7). No QEMU, make, compiler, `tests/run`, etude, worker loop, Claude or Codex process of any account; the five team accounts run nothing; `ghrunner` runs only its two idle runner listeners. The ledger `/var/lib/rach2-etude/ledger/claims` holds only its header line, `etude-ledger 1`. 903 GiB free on the root, 124 GiB of memory free.
* The kernel: `6.18.54.1-microsoft-standard-WSL2 #1 SMP PREEMPT_DYNAMIC Tue Oct  6 19:13:07 UTC 2026`, built by `gcc (GCC) 13.2.0, GNU ld (GNU Binutils) 2.41`. No `/sys/kernel/security/lockdown`. `CONFIG_MODULES=y`, `CONFIG_MODVERSIONS=y` with genksyms (not gendwarfksyms), `CONFIG_MODULE_SIG` off, `CONFIG_MODULE_FORCE_LOAD=y`, `CONFIG_RANDSTRUCT_NONE=y`, `CONFIG_LOCALVERSION="-microsoft-standard-WSL2"` without `LOCALVERSION_AUTO`, retpolines and return thunks on, `CONFIG_CALL_PADDING=y`. So an unsigned module loads, and its symbol versions must match the running kernel's.
* `# CONFIG_UDMABUF is not set`, `# CONFIG_SW_SYNC is not set`, `# CONFIG_DMABUF_HEAPS is not set`; `CONFIG_DMA_SHARED_BUFFER=y`, `CONFIG_SYNC_FILE=y`, `CONFIG_MEMFD_CREATE=y`, `CONFIG_SHMEM=y`, `CONFIG_HUGETLBFS=y`. No `/dev/udmabuf`, no udmabuf module.
* What a udmabuf module would call, from `/proc/kallsyms`: `memfd_pin_folios`, `unpin_folio`, `unpin_folios`, `shmem_mapping`, `vmap`, `vm_map_ram`, `dma_buf_export` and `misc_register` are exported (each has a `__ksymtab_` entry); `memfd_fcntl` exists but is not exported; `vmap_pfn` does not exist at all (`CONFIG_VMAP_PFN` is off); `hugetlbfs_file_operations` is a data symbol, not exported.
* Modules come from an overlay mounted on `/usr/lib/modules/6.18.54.1-microsoft-standard-WSL2` (744 `.ko` files, `kvm.ko`'s vermagic `6.18.54.1-microsoft-standard-WSL2 SMP preempt mod_unload modversions`). The headers in `/usr/src/linux-headers-6.18.54.1-microsoft-standard-WSL2` are only `include/`, 9.5 MiB: no `Makefile`, `scripts/` or `Module.symvers`, so nothing can be built against them.
* Tools in WSL: gcc 15.2.0 (the default) and gcc-14, no gcc-13; make, flex, bison, bc, pahole (dwarves 1.31), perl, openssl, libelf-dev and libssl-dev present; no zstd or cpio.
* The graphics stack: QEMU 10.2.1 (Debian 1:10.2.1+ds-1ubuntu3.3), libvirglrenderer1 1.2.0-2ubuntu2, Mesa 26.0.8-1ubuntu0.3 (lavapipe's ICD `lvp_icd.json` among eight), libvulkan1 1.4.341. `/dev/kvm` root:kvm 660, `/dev/dri/card0` root:video, `/dev/dri/renderD128` root:render, `/dev/dxg` 666. `testing` is in `kvm` (991) and `render` (990).

## Microsoft's kernel source

* The release `linux-msft-wsl-6.18.54.1` on `github.com/microsoft/WSL2-Linux-Kernel`, published 2026-10-06 22:28 UTC, commit d504d40ab83839cb61942a812a99d183c42227b9 ("Update to stable kernel version v6.18.54"; its notes say DRM and VGEM are re-enabled). Its only assets are GitHub's source archives (`.zip` and `.tar.gz`); the release publishes no hash (`out/look2.txt`), so the archive's SHA-256 is recorded here instead.
* Downloaded at 08:05 in WSL (`scripts/wsl-dl.sh`, `out/` has none of it; the log is in the cache): `https://github.com/microsoft/WSL2-Linux-Kernel/archive/refs/tags/linux-msft-wsl-6.18.54.1.tar.gz`, 256,238,870 bytes in 47.6 s, SHA-256 5f9752ec753f6c0facae7f999660e475b13a45c849eef7e2ea9e39453c947d45, kept in `/home/testing/rach2/build/cache/run2/step6/`. `git get-tar-commit-id` reads d504d40ab83839cb61942a812a99d183c42227b9 from the archive, the release's commit (`out/unpack.txt`). The top `Makefile` gives 6.18.54 with `EXTRAVERSION = .1`.
* A first detached start of the download died with the SSH session before its shell had opened its log; the second, with `setsid -f` and the redirection inside the child, ran.

## What the module needs from the source

* `drivers/dma-buf/Kconfig:33-34`: `config UDMABUF` is `bool`, so Kbuild cannot build it as a module in the tree; it is built out of the tree from a copy (`mod/udmabuf.c`, `mod/Kbuild` with `obj-m := udmabuf.o`).
* In this version `vmap_udmabuf` maps with `vm_map_ram` (`udmabuf.c:110-134`), which is exported; it does not call `vmap_pfn`. So the brief's `vmap_pfn` concern does not arise in 6.18.54.1 (`vmap_pfn` is built only with `CONFIG_VMAP_PFN`, selected by i915 and xe, and it is exported when built, `mm/vmalloc.c:3617`).
* What it does need: `check_memfd_seals` calls `memfd_fcntl(memfd, F_GET_SEALS, 0)` (`udmabuf.c:304`), which `mm/memfd.c:320` defines without an export. The file has no `MODULE_LICENSE`, so its GPL-only imports (`memfd_pin_folios`, `unpin_folio`, `shmem_mapping`, `vmf_insert_pfn` and the dma-buf calls) would be refused, and `dma_buf_export`, `dma_buf_fd` and `dma_buf_put` are exported in the `DMA_BUF` namespace (`dma-buf.c:760`), which a module must import.
* The patch, `run2/step6/udmabuf-module.diff` (SHA-256 80e50c7b53939f301a52695bc0ac9f4306033c5ad66f6cf1a8bf09f03c931ab4, 3 hunks): a static `udmabuf_get_seals()` that reads the seals as `memfd_get_seals()` and `memfd_file_seals_ptr()` in `mm/memfd.c:211-222, 313-318` do (`SHMEM_I(inode)->seals` for shmem, `HUGETLBFS_I(inode)->seals` for hugetlbfs), called in place of `memfd_fcntl`; and `MODULE_DESCRIPTION`, `MODULE_LICENSE("GPL")` and `MODULE_IMPORT_NS("DMA_BUF")`. Nothing else changes; the seal check still runs under the inode's shared lock, as in the tree.

## The kernel build, once, for `Module.symvers`

* The source configured from the running kernel's `/proc/config.gz` with `make CC=gcc-14 HOSTCC=gcc-14 olddefconfig` (`scripts/wsl-config.sh`, `out/config.txt`). `make kernelrelease` gives `6.18.54.1-microsoft-standard-WSL2`, equal to `uname -r`.
* gcc 13.2.0, which built the running kernel, is not in WSL; gcc-14 (Ubuntu 14.3.0-14ubuntu1) is the nearest, and nothing was installed. `scripts/diffconfig` between the running config and the configured one lists only what follows from the compiler: the version lines (`CC_VERSION_TEXT`, `GCC_VERSION`, `AS_VERSION`, `LD_VERSION` 2.41 to 2.46, `PAHOLE_VERSION` 1.25 to 1.31), five `CC_HAS_*` capabilities gcc 14 has and 13.2 lacks (asm goto with outputs, tied outputs, KASAN software tags, the two function-alignment ones), `GCC_ASM_GOTO_OUTPUT_BROKEN` gone (`init/Kconfig:92-97` sets it for gcc 13.0 to 13.2), and `GCC_PLUGINS` gone (Ubuntu's gcc-14 has no plugin headers) together with four plugin options that were already off (`RANDSTRUCT_FULL`, `RANDSTRUCT_PERFORMANCE`, `GCC_PLUGIN_LATENT_ENTROPY`, `KSTACK_ERASE`). `RANDSTRUCT_NONE=y` holds in both.
* First build: `make CC=gcc-14 HOSTCC=gcc-14 -j32 vmlinux modules`, 08:08 to 08:11, 3 min 20 s wall and 4,468 s of user time, exit 0 (`scripts/wsl-build.sh`).
* The module built from it (`scripts/wsl-mod.sh`, `out/mod.txt`): `make -C src CC=gcc-14 M=mod modules`, no warning; `modinfo` gives vermagic `6.18.54.1-microsoft-standard-WSL2 SMP preempt mod_unload modversions`, the same as the running kernel's `kvm.ko`, `license: GPL`, `import_ns: DMA_BUF`, the two parameters `list_limit` (1024) and `size_limit_mb` (64).
* The check that matters is the symbol versions, since the running kernel compares each import's CRC with its own. The running kernel's CRCs are readable from its 744 installed modules, which load: `modprobe --dump-modversions` on each lists every import with the CRC it was built against. Against the first build's `Module.symvers`, 16,603 of their 31,941 imports from vmlinux had the same CRC and 15,338 a different one, `module_layout` among them (ours 0x0f19fee5, the kernel's 0x1fa88b5b). So that module would have been refused at load ("disagrees about version of symbol"): a clean refusal, not a fault, but no module.
* The cause, traced (`scripts/wsl-crc.sh`, `scripts/wsl-crc2.sh`, `out/crc-printk*.txt`): genksyms puts `__attribute__` clauses into the text it hashes. `_printk` has the running kernel's CRC 0x122c3a7e only when declared with the format attribute alone; with `__cold` too, as our build saw it, the tree's genksyms gives our 0x92997ed8. `include/linux/compiler_types.h:111-115` defines `__cold` as `__attribute__((__cold__))` only when `CONFIG_CC_HAS_SANE_FUNCTION_ALIGNMENT` is set, which `arch/Kconfig:1820-1830` derives from gcc's `-fmin-function-alignment`, new in gcc 14; under gcc 13.2 `__cold` is empty. The same capability lines decide other attributes (gcc 15 adds `__counted_by__` to `mem_cgroup_threshold_ary`, seen in the symtypes). So a module's symbol versions depend on the capabilities of the compiler the kernel was configured with, not only on the source and the visible options.
* The fix, in the work tree only: `kconfig-gcc13.diff` (SHA-256 026ebc698b4018a3e593bb11c8fc7a33ccad3b9303ad7418cdfdb02078c43195) sets the three tests to gcc 13.2's answers: `CC_HAS_MIN_FUNCTION_ALIGNMENT` n (so `CC_HAS_SANE_FUNCTION_ALIGNMENT` n), `GCC_ASM_GOTO_OUTPUT_BROKEN` y (so both asm-goto-output options n), `CC_HAS_KASAN_SW_TAGS` n. Configured again (`scripts/wsl-reconf.sh`, `out/reconf.txt`), the config differs from the running one only in the version lines and in `GCC_PLUGINS` with its four unused options; `GCC_PLUGINS` adds only `-DGCC_PLUGINS`, which `include/linux/compiler-version.h` reads for rebuild tracking.
* Second build, 08:14 to 08:18 (3 min 26 s), and the module again (`out/mod2.txt`): `_printk` now has the running kernel's CRC, but against the 744 installed modules 17,472 of 31,941 imports were equal and 14,469 different, `module_layout` still among them. By distinct symbol, 1,642 of the 4,954 vmlinux symbols the installed modules import were equal and 3,312 different (`scripts/wsl-crc3.sh`, `out/crc3.txt`).
* To find what else differs, the tree was rebuilt with `KBUILD_SYMTYPES=1` from clean objects (`scripts/wsl-symtypes.sh`), so each object leaves the text genksyms hashed, and `hsrc/crcdiff.py` compared the closures: a symbol whose CRC equals the running kernel's proves every type text it uses; one that differs uses at least one text that differs. 20 differing symbols had exactly one unproven text, and every one of them is a per-CPU variable or pointer whose text carries `__seg_gs` (`out/crcdiff.txt`): `__preempt_count`, `cpu_number`, `this_cpu_off`, `free_percpu ( void __seg_gs * )`, `pcpu_alloc_noprof`. The common unproven types (`percpu_counter` with `t#s32 __seg_gs * counters`, `task_struct`, `page`) carry it too.
* The cause: `arch/x86/include/asm/percpu.h:90-95` qualifies every `__percpu` pointer with the `__seg_gs` address space only when `USE_TYPEOF_UNQUAL` is defined, and `include/linux/compiler-gcc.h:145` defines `CC_HAS_TYPEOF_UNQUAL` as `(__GNUC__ >= 14)`, a test in a header, not in Kconfig. gcc 13.2 has no `__typeof_unqual__`, so the running kernel's per-CPU pointers are plain pointers. Either way a per-CPU pointer has the same size and value, so the layouts agree; only the text genksyms hashes, and the instructions that access per-CPU data, differ.
* The work tree was given gcc 13's answer too, `CC_HAS_TYPEOF_UNQUAL 0`; with the Kconfig change this is `gcc13-view.diff` (SHA-256 44577bb4fb36d26b3197a4bd690fd71c7eece838c70df540ccecc1159e774db8).
* Third build, from clean objects with `KBUILD_SYMTYPES=1`, 08:22 to 08:26 (3 min 24 s; `scripts/wsl-gcc13view.sh`, `out/wsl/build-tail.txt`, the whole log in `out/wsl/build.log.gz`). Against it every one of the 4,954 distinct vmlinux symbols the installed modules import has the running kernel's CRC, and all 31,941 imports of the 744 modules agree (`out/crc3-v3.txt`, `out/mod3.txt`). So, as genksyms sees types, this tree is the running kernel's. The module built from it: 39 imports, 35 of them checked against an installed module's CRC (`module_layout` 0x1fa88b5b among them); `memfd_pin_folios`, `shmem_mapping`, `unpin_folio` and `vmf_insert_pfn` are imported by no installed module, so only the kernel's own check at load covers them.
* What is kept, in `/home/testing/rach2/build/cache/run2/step6/` in WSL: `udmabuf.ko` (533,712 bytes, SHA-256 4b41ddd7830b30324f433726c33787d70f9f3639f802b22b7891a2b003ef1ce3), `udmabuf-module.diff`, `Module.symvers` (1,080,434 bytes, SHA-256 a11c69b8a768e63597bb9fd78ac09e3a2004fe0dc67fe38ac4ca52a611b83159), the config the tree was built with, `config-6.18.54.1-gcc14` (SHA-256 b7bd5a7015b89f3fa6161f399704025bdcf4aaf99346d5d2a2cf6960aef8ef21), `kconfig-gcc13.diff`, `gcc13-view.diff` and the source archive. On the Mac: `run2/step6/udmabuf-module.diff`, `run2/step6/kconfig-gcc13.diff`, `run2/step6/gcc13-view.diff` and `run2/step6/mod/`. To rebuild: unpack the archive, apply `gcc13-view.diff` (which holds the Kconfig change), copy `config-6.18.54.1-gcc14` to `.config`, `make CC=gcc-14 HOSTCC=gcc-14 -j32 vmlinux modules`, then `make -C src CC=gcc-14 M=mod modules` with the patched `udmabuf.c`.
* Which compiler built what: the running kernel, gcc 13.2.0 with binutils 2.41 (`/proc/version`); the module and the tree it was checked against, gcc-14 14.3.0 (Ubuntu 14.3.0-14ubuntu1) with binutils 2.46, configured to gcc 13.2's capabilities as above.

## Loading the module

* A fault in a module would take WSL down: the running kernel has `CONFIG_PANIC_ON_OOPS=y` (`/proc/sys/kernel/panic_on_oops` reads 1) and `panic=-1` on its command line, so any oops panics the WSL virtual machine and it reboots at once, for every account.
* 08:27:25, as root (`scripts/wsl-load.sh`, `out/load.txt`): load average 6.20 (falling from this step's builds), no QEMU, make, compiler, `tests/run`, etude, worker loop, Claude or Codex process of any account, the five team accounts with no process at all, the ledger only `etude-ledger 1`, no `udmabuf` loaded, taint 0. Then `insmod /home/testing/rach2/build/cache/run2/step6/udmabuf.ko`: exit 0. The kernel logged one line, `udmabuf: loading out-of-tree module taints kernel.`, and the taint became 4096 (out-of-tree module) only; no symbol-version, BTF or other complaint. `/proc/modules`: `udmabuf 12288 0 - Live`.
* `/dev/udmabuf` appeared as `crw-rw---- root kvm 10, 261` at once, by systemd's stock rule `/usr/lib/udev/rules.d/50-udev-default.rules:125` (`KERNEL=="udmabuf", GROUP="kvm"`), which was there before this step. So no hand change was needed, and none was made: the node and its group go with the module. The module was loaded with `insmod` from the cache, not installed under `/lib/modules`, so nothing loads it at boot.
* From user space as `testing` (`hsrc/udmabuf-test.c`, `scripts/wsl-hosttest.sh`, `out/hosttest.txt`): a 16 MiB buffer from a memfd sealed against shrinking, mapped both ways, each side reading the other's 4 Mi words exactly, `DMA_BUF_IOCTL_SYNC` start and end, a two-range list of 3 MiB; and the refusals the driver must give: a memfd without the shrink seal and one sealed against writing (EINVAL, the patched seal check), 65 MiB above `size_limit_mb` 64 (EINVAL), a file that is no memfd (EBADFD). All 13 checks passed; nothing in the kernel log.
* The host's lavapipe (Mesa 26.0.8, read with the guest's `vulkaninfo` 1.4.304 binary, `out/wsl/host-lvp-nomod.txt` and `out/wsl/host-lvp-mod.txt`): before the module 194 extensions and Vulkan 1.4.335; with it 197, the three new ones exactly `VK_EXT_image_drm_format_modifier`, `VK_KHR_external_fence_fd` and `VK_KHR_external_semaphore_fd`, which the first run predicted from Mesa's source (`tonight/qemu-wsl.md`: llvmpipe exports dma-bufs only through `/dev/udmabuf`).

## Venus on the host's lavapipe, with the module

The guest: `s6.qcow2`, an overlay of step 5's `s5base.qcow2` (Debian 13.7, kernel 6.12.115+deb13.1, Mesa 25.0.7-2+deb13u1, vulkan-tools 1.4.304, libvulkan1 1.4.309, Weston 14.0.2, Xwayland 24.1.6, Chromium 156.0.8078.79, GE-Proton11-12), with seatd 0.9.1 and libvulkan-dev added. The host: QEMU 10.2.1 (Ubuntu 1:10.2.1+ds-1ubuntu3.3) with KVM, `-cpu max -smp 8 -m 8G` on a shared memfd, `-device virtio-vga-gl,venus=on,blob=on,hostmem=4G -display egl-headless,rendernode=/dev/dri/renderD128`, libvirglrenderer1 1.2.0-2ubuntu2, Mesa 26.0.8 with `VK_DRIVER_FILES` set to lavapipe, QEMU's GL on llvmpipe (vgem). Started at 08:29:37 as `testing` (`step6/scripts/vm6.sh start s6 ... lp`); ssh answered after 11.1 s. That `testing` may open `/dev/udmabuf` comes from its `kvm` group.

### What the guest sees

`step6/out/guest/mod-lp/` against the same guest and host before the module (`step6/out/wsl/pre-nomod/`):

* `VN_DEBUG=init`: connected to the renderer, wire format 1, vk xml 1.3.269, Venus protocol 2, renderer instance version 1.4.341, "supports multi-plane wsi format modifiers: no"; the same as before.
* The Venus device is now "Virtio-GPU Venus (llvmpipe (LLVM 21.1.8, 256 bits))" at **apiVersion 1.3.269** (1.2.0 before), driver venus 25.0.7, conformance 1.3.0.0, device type CPU.
* 109 device extensions (100 before). The nine it gains are exactly `VK_KHR_swapchain`, `VK_KHR_swapchain_mutable_format`, `VK_KHR_incremental_present`, `VK_KHR_synchronization2`, `VK_KHR_external_memory_fd`, `VK_EXT_external_memory_dma_buf`, `VK_EXT_image_drm_format_modifier`, `VK_KHR_external_semaphore_fd` and `VK_KHR_external_fence_fd`; it loses none. This is what the first run read from Mesa's source (`tonight/qemu-wsl.md`, "Venus on the host's lavapipe": synchronization2 and the 1.3 clamp hang on sync-file import, external memory on DRM format modifiers, the swapchain on both).
* Against the first run's level 3 guest under TCG, whose Venus host was the level 2 Debian guest (Mesa 25.0.7 on both sides, virglrenderer 1.1.0, 108 extensions, `tonight/qemu-wsl/guest-venus-host/l3-vulkaninfo-full.txt`): the same set, plus `VK_EXT_fragment_shader_interlock`, which the host's Mesa 26.0.8 lavapipe adds. So the WSL host with the module now gives a level 2 guest what only a level 3 guest under TCG had before.
* Still absent: `VK_KHR_load_store_op_none` (only `VK_EXT_load_store_op_none`), `VK_KHR_maintenance6`, `VK_EXT_map_memory_placed`, `VK_EXT_external_memory_host`, `VK_KHR_present_wait` and `VK_KHR_present_id`, `VK_EXT_swapchain_maintenance1`, mesh shaders, ray queries, `VK_EXT_shader_object`. `VK_KHR_maintenance5`, `VK_EXT_graphics_pipeline_library`, `VK_EXT_robustness2`, transform feedback, dynamic rendering, descriptor indexing and timeline semaphores are there, as before.
* Memory: one heap of 125.79 GiB, the WSL VM's memory as the host's lavapipe reports it, not the guest's 8 GiB; one memory type, DEVICE_LOCAL, HOST_VISIBLE, HOST_COHERENT and HOST_CACHED; `maxMemoryAllocationSize` 2 GiB; geometry and tessellation shaders, BC textures and float64.
* Under Weston, the Venus surface offers MAILBOX, FIFO and IMMEDIATE (`vk-wayland.txt`).

### A picture

Weston 14 on the guest's virtio-gpu (DRM backend, GL renderer on `virgl (LLVMPIPE (LLVM 21.1.8, 256 bits))`, GLES 3.2, "dmabuf support: modifiers", kiosk shell, `step6/gsrc/g-sess.sh`), and `vkcube --wsi wayland --gpu_number 0` on Venus:

* `weston-screenshooter` at 08:30, while vkcube ran: `step6/out/guest/shots/venus-cube.png`, 1280x800, the textured LunarG cube, whole and correct. So Venus's frames went the whole way: rendered by the host's lavapipe into blob memory exported through udmabuf, handed to Weston as dma-bufs, imported by Weston's GL on virgl, composed and scanned out. In the first run's level 3 test the same handoff failed in the level 2 virglrenderer ("failed to create egl image") and no picture was taken.
* QMP's `screendump` answered `{"error": {"class": "GenericError", "desc": "no surface"}}`, as in the first run. Once Weston scans out a blob, QEMU's console holds a dma-buf scanout and no `DisplaySurface`, and `screendump` reads only the latter. So on this host a picture of a guest's 3D output comes from the guest's own capture (or, for Rach2, from a readback in the test), not from QMP.

### Frame rates

`vkcube --c N --present_mode M` under the guest's Weston, timed by the guest's wall clock from start to exit, so the times include vkcube's start (about 0.3 s; `step6/out/guest/cube/`). vkcube keeps two frames in flight: before recording a frame it waits on the fence of the frame two back (its `FRAME_LAG` of 2, known from vulkan-tools' `cube.c` and not read again in this step). In FIFO, Mesa's Wayland WSI also waits for the compositor's frame callback before presenting again. The Venus device has no `VK_KHR_present_wait`.

| GPU | present mode | frames | seconds | frames a second |
|---|---|---|---|---|
| Venus on the host's lavapipe | FIFO | 300 | 2.725 | 110 |
| Venus | FIFO | 1500 | 11.884 | 126 |
| Venus | MAILBOX | 1500 | 11.623 | 129 |
| Venus | IMMEDIATE | 1500 | 11.649 | 129 |
| Venus, two vkcubes at once | IMMEDIATE | 1500 each | 10.961 and 11.183 | 137 and 134, 271 together |
| the guest's own lavapipe | FIFO | 300 | 3.246 | 92 |
| the guest's own lavapipe | FIFO | 1500 | 14.690 | 102 |
| the guest's own lavapipe | MAILBOX | 1500 | 1.003 | about 1500 |
| the guest's own lavapipe | IMMEDIATE | 1500 | 0.996 | about 1500 |

* Weston's own pace is about 100 frames a second: virtio-gpu has no vblank, so a page flip completes when QEMU retires the flush's fence, and FIFO on the guest's lavapipe follows it at 102.
* On Venus all three present modes give the same 126 to 129 frames a second, about 7.8 ms a frame, while the guest's own lavapipe in MAILBOX or IMMEDIATE runs at about 1500. Two Venus vkcubes at once each keep their rate (134 and 137), so together they draw twice as many frames: the limit is a wait in each frame, not the host's rendering.
* The wait is QEMU's fence retirement (an inference from QEMU's source and the numbers, not traced per frame). QEMU 10.2.1 calls `virgl_renderer_poll()` when the guest kicks the control queue and then every 10 ms while fences are pending (`hw/display/virtio-gpu-gl.c:101`, `hw/display/virtio-gpu-virgl.c:1110-1119`), and it starts virglrenderer with neither a fence thread nor async fence callbacks (`VIRGL_RENDERER_VENUS | VIRGL_RENDERER_RENDER_SERVER` only, `:1148-1161`). Each Venus present turns a semaphore into a sync file, a guest dma-fence that signals only when QEMU next polls; so each frame waits up to 10 ms for a kick or the timer, and the other traffic (Weston's flips, the second vkcube) brings the average down to 7 to 8 ms. A program that waits on every frame through a virtio-gpu fence on this host runs at about 100 to 130 frames a second at most, whatever it draws; step 10 measured about 11 ms a frame unthrottled (87 to 93 FPS) on the Lenovo's Venus over hasvk, with QEMU 10.0.13, whose poll is the same (`virtio-gpu-virgl.c:1144` there), and did not split it.
* The host during the 54 s of the six 1500-frame runs: QEMU used 173 s of CPU (3.2 CPUs on average), the WSL load average 2.5 to 3.

### Direct3D through DXVK, vkd3d-proton and WineD3D on Venus

Step 2's six probes in the guest's `~/probes`, GE-Proton11-12 (Wine 11.0 Staging, DXVK v3.1.1-174-gf86377680, vkd3d-proton v3.1-1-g8293130eb) from the guest's `~/ge/GE-Proton11-12-x86_64`, `proton run` outside Steam with `PROTON_USE_WOW64=1` and `PROTON_LOG=1`, a new prefix `~/cd-s6`, under the guest's Xwayland in Weston (`step6/gsrc/g-proton.sh`, 08:35; raw output in `step6/out/guest/proton-a/` and `proton-b/`). `VK_DRIVER_FILES=/usr/share/vulkan/icd.d/virtio_icd.json` leaves Venus the guest's only Vulkan device, so whatever Vulkan a run used was Venus; the rows without it say so.

| Probe and path | Result on Venus over the WSL host's lavapipe |
|---|---|
| DXVK, Direct3D 11 64-bit, `DXVK_FILTER_DEVICE_NAME=Venus` | no adapter, "d3d11probe: no DXGI adapter", exit 1; the guest's lavapipe skipped by the filter |
| DXVK, Direct3D 11 64-bit, Venus only | "Skipping: Device does not support required feature 'khrLoadStoreOpNone' (extension: VK_KHR_load_store_op_none)", "No adapters found", exit 1 |
| DXVK, Direct3D 9 64-bit, `DXVK_FILTER_DEVICE_NAME=Venus` | no adapter, exit 3 |
| vkd3d-proton, Direct3D 12 64-bit and 32-bit, Venus only | factory fails, hr=0x80004005: its DXGI is DXVK's, which skips Venus as above |
| vkd3d-proton behind Wine's DXGI (`WINEDLLOVERRIDES=dxgi=b`), 64-bit | `D3D12CreateDevice` hr=0x80070057: "maintenance5 and/or maintenance6 not supported by this implementation. This is required for correct operation." |
| WineD3D on Vulkan (`PROTON_USE_WINED3D=1 WINE_D3D_CONFIG=renderer=vulkan`), Direct3D 11 64-bit | draws right (center 255 128 0 255, corner 0 0 128 255), feature level 11_0, adapter "Virtio-GPU Venus (llvmpipe (LLVM 21.1.8, 256 bits))", vendor 0x0005, 128,813 MiB "dedicated" |
| WineD3D on Vulkan, Direct3D 9 64-bit | draws right, vs and ps 3.0, 4,095 MiB |
| WineD3D on Vulkan, Direct3D 11 and 9, 32-bit (WoW64) | device made, then exit 5: "win32u_vkMapMemory2KHR returned mapping 0x7f88... does not fit 32-bit pointer", "Failed to map chunk memory, vr VK_ERROR_OUT_OF_HOST_MEMORY" |
| Wine's own d3d12 and vkd3d (`PROTON_USE_WINED3D=1`), 64-bit, Venus only | draws right: highest feature level 11_1, shader model 6.2, binding tier 3, tiled tier 0, heap tier 2, typed UAV loads 1 |
| the same, 32-bit | device made, then "d3d12_heap_init Failed to map memory, vr -1" and the same 32-bit pointer line; the probe's fence wait timed out, exit 1 |
| WineD3D on OpenGL (virgl, the host's llvmpipe), Direct3D 11, 64-bit and 32-bit | both draw right at 11_0, adapter "Red Hat VirtIO GPU" 0x1af4/0x1010 |

* Each run took 1.6 to 2.7 s in the guest (the first, which made the prefix, 4.1 s; the 32-bit vkd3d run 11.9 s, its fence wait). On the Lenovo step 10 measured about 12 s a probe, with Wine read over 9p; here Proton sits on the guest's own disk.
* WineD3D on OpenGL reports 352,620,360 MiB of dedicated video memory for 64-bit programs (4,095 for 32-bit), as nonsensical as the first run's 16,430,080 MiB; on Vulkan it reports lavapipe's heap, capped at 4,095 for 32-bit.

### Chromium's WebGPU and WebGL

Chromium 156.0.8078.79 in the guest, fullscreen under the guest's Weston with `--ozone-platform=wayland`, a new profile each run, on step 5's probe page, `chrome://gpu` saved over the debugging port (`step6/gsrc/g-browser.sh`, 08:36 to 08:37; `step6/out/guest/br-a/`):

| Switches | WebGL 1 and 2 | WebGPU's adapter |
|---|---|---|
| `--enable-unsafe-webgpu --ignore-gpu-blocklist` | both draw right on "ANGLE (Mesa, virgl (LLVMPIPE (LLVM 21.1.8 256 bits)), OpenGL ES 3.2)": fill 9,321 and 8,948 Mpix/s, ALU 20.3 and 21.3 ms, 20,000 draws 13.0 and 12.3 ms; `requestAnimationFrame` 76 Hz | SwiftShader, `isFallbackAdapter` true; compute right in 100 ms, render right |
| the same and ANGLE on Vulkan (`--use-angle=vulkan --enable-features=Vulkan,DefaultANGLEVulkan,VulkanFromANGLE`) | none: the GPU process died six times (exit codes 6 and 8704), then software compositing | `requestAdapter` null |

* Dawn's list in `chrome://gpu`, with the module as without it: SwiftShader, "Virtio-GPU Venus (llvmpipe (LLVM 21.1.8, 256 bits))" and the guest's lavapipe, each `<CPU>` and each "Blocklisted - crbug.com/40057808: CPU adapters not fully tested or conformant". Venus relays lavapipe's device type, CPU, and Chromium 156 offers no switch that lifts that entry: among the binary's switch and feature names there is no WebGPU blocklist control (`--enable-unsafe-webgpu` and `--ignore-gpu-blocklist` were both given), only `use-webgpu-adapter`, `use-webgpu-power-preference`, `enable-dawn-features` and the like. So on a lavapipe host a page never gets Venus; it gets SwiftShader as the fallback, and the module changes nothing here.
* ANGLE on Vulkan: the guest's Venus driver logs "stuck in ring seqno wait", "aborting on ring fatal error" and "vn_ring_submit abort on fatal", step 10's signature. Chromium also says "'--ozone-platform=wayland' is not compatible with Vulkan", as in the first run. The host's side is in "QEMU's own log" below.

### Blob memory through udmabuf

`blobtest Venus 64` (step 10's program, built in the guest; `step6/out/guest/blob/`), against the guest's own lavapipe on guest memory in the same run, and step 10's Venus on the Lenovo's hasvk:

| Measure (64 MiB buffers unless named) | Venus on the WSL host's lavapipe | the guest's own lavapipe | step 10, Venus on hasvk, QEMU 10.0.13 |
|---|---|---|---|
| Memory type | 0: device-local, host-visible, coherent, cached | the same | 1: the same flags |
| The guest's mapping | `rw-s ... /dev/dri/renderD128` (a blob) | `/memfd:allocation fd` | `/dev/dri/renderD128` |
| Allocate, 4 KiB to 64 MiB | 0.03 to 0.06 ms | 0.003 to 0.005 ms | 0.04 to 0.06 ms |
| Map and first touch, each blob freed before the next | 7.2 ms (4 KiB) to 7.9 ms (1 MiB), 14.9 ms (16 MiB), 31.0 ms (64 MiB) | 0.001 to 0.006 ms | 52.5 to 58.1 ms at every size |
| Unmap and free, same loop | 2.4 to 3.6 ms | 0.002 to 0.018 ms | 8.9 to 10.9 ms |
| 32 maps of 64 KiB held | mean 1.69 ms, first 12.0 ms | mean 0.010 ms | mean 5.05 ms, first 51.9 ms |
| 32 unmaps and frees, then one map | 7.5 ms, then 19.2 ms | 0.07 ms, then 0.003 ms | 15.9 ms, then 116.8 ms |
| CPU write, read, `memcpy` through the mapping | 20,243, 38,217, 15,423 MB/s | 20,163, 34,899, 14,697 MB/s | 5,781, 9,328, 8,209 MB/s |
| GPU copy, first / best of 8 | 26.0 ms / 6.08 ms (11,030 MB/s) | 5.1 ms / 4.56 ms (14,710 MB/s) | 37.7 ms / 8.83 ms |
| GPU fill | 2.61 ms | 2.48 ms | 8.82 ms |
| Words wrong, after the copy, the fill and four rounds of CPU write, GPU copy, CPU read | 0 of 16,777,216 each time | 0 | 0 |

* The blob path is coherent and correct over udmabuf, and the CPU reads and writes the host's memory through it at the speed of the guest's own memory; the host's lavapipe copies at 75 percent of the guest's own after the first copy. A mapping costs 7 to 8 ms plus the first touch of its pages, and an unmap 2 to 4 ms, about seven times less than step 10 measured with QEMU 10.0.13 on the Lenovo, where an unmap held the queue some 50 ms in QEMU's RCU thread. Whether QEMU 10.2.1 changed that path or the testing PC's QEMU simply has more RCU work queued was not traced. For Rach2's driver the rule is step 10's: map a blob once and keep it.

### QEMU's own log: ANGLE on Vulkan, and vkcube

With `-daemonize` QEMU sends its stderr, and virglrenderer's, to `/dev/null`, so the first boot left an empty log. `vm6.sh` gained `KEEPLOG=1`, which starts QEMU with `setsid -f` instead and keeps its stderr in `out/qemu-NAME.log`; the guest was powered off and started again at 08:39, with `VIRGL_LOG_LEVEL=info` in QEMU's environment (`step6/out/wsl/qemu-s6-2-lp-angvk.log`).

* vkcube on Venus, 300 frames in FIFO in 2.864 s: no virglrenderer line at all. The first run's "failed to create egl image" does not occur with QEMU's GL on llvmpipe.
* The ANGLE on Vulkan run (`step6/out/guest/br-c/`), three times, once for each GPU process that got as far: "proxy: exported res 41 to unexpected fd_type -1", then "vkr: failed to import resource: invalid res_id 41", "vkr: vkAllocateMemory resulted in CS error", "vkr: ring_submit_cmd: vn_dispatch_command failed" (res 52 and 82 the other times); and from QEMU eight times "virtio_gpu_virgl_process_cmd: ctrl 0x209, error 0x1200", `RESOURCE_UNMAP_BLOB` answered `ERR_UNSPEC` as the dead contexts' blobs were unmapped. These are step 10's lines with Debian's virglrenderer 1.1.0, now with Ubuntu's 1.2.0-2ubuntu2 (which links libgbm): a resource made by virgl's GL renderer cannot be exported as a dma-buf, so the Venus context cannot import it, and ANGLE on Vulkan imports one at start. Whether Ubuntu builds 1.2.0 without `minigbm_allocation` was not read in its packaging; the behaviour is step 10's.

### With QEMU's GL on the RTX 4080

The same guest started a third time at 08:40 with QEMU's GL on Mesa's d3d12 driver (`GALLIUM_DRIVER=d3d12 MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA`, `vm6.sh start ... d3d12`, `KEEPLOG=1`, `VIRGL_LOG_LEVEL=info`; `step6/out/wsl/qemu-s6-3-d3d12.log`). Venus still runs over the host's lavapipe; only the GL that QEMU and the guest's GL use moves to the RTX.

* The guest's GL is "virgl (D3D12 (NVIDIA GeForce RTX 4080 SUPER))", and Weston's GL renderer runs on it (OpenGL ES 3.1).
* vkcube on Venus exits 0: FIFO 300 frames in 2.401 s and 1500 in 10.186 s (147 a second), IMMEDIATE 1500 in 9.901 s (152); the guest's own lavapipe IMMEDIATE 1500 in 1.006 s.
* But nothing reaches the screen: `weston-screenshooter` gives an all-black 1280x800 picture (`step6/out/guest/shots/d3d12-venus-cube.png`), and virglrenderer logs "vrend_renderer_pipe_resource_set_type: failed to create egl image", "context 2 failed to dispatch PIPE_RESOURCE_SET_TYPE: 22" and "vrend_decode_ctx_submit_cmd: context error reported 2 \"weston\" Illegal command buffer 655409": the first run's level 3 failure, word for word. Weston's GL context is dead from then on. vrend makes an EGL image of each Venus buffer's dma-buf on QEMU's EGL display; llvmpipe on vgem imports it, Mesa's d3d12 driver does not (the import's failure inside Mesa's d3d12 driver was not traced).
* So on the testing PC Venus presents only when QEMU's GL is llvmpipe. The fast GL path (virgl on the RTX, step 5) and Venus with a picture cannot be had in one QEMU; a test that needs both GL speed and Venus's frames on screen has no single profile here.

## Step 10's findings, here

Versions here: the guest's Venus driver Mesa 25.0.7-2+deb13u1 (vk.xml 1.3.269); the host's lavapipe Mesa 26.0.8-1ubuntu0.3 (Vulkan 1.4.335) and virglrenderer 1.2.0-2ubuntu2 (renderer instance 1.4.341), QEMU 10.2.1. Step 10's: the same guest Mesa, the host's Mesa 25.0.7 (hasvk and lavapipe) and virglrenderer 1.1.0, QEMU 10.0.13.

* DXVK and vkd3d-proton refuse Venus for `VK_KHR_load_store_op_none`: holds. DXVK logs the same line; vkd3d-proton behind DXVK's DXGI finds no adapter for that reason, and behind Wine's DXGI it stops on the next requirement, `maintenance6`. Both are the guest driver's gaps: the host's lavapipe 26.0.8 offers `VK_KHR_load_store_op_none`, `VK_KHR_maintenance5` and `VK_KHR_maintenance6` (`step6/out/wsl/host-lvp-mod.txt`), and Venus 25.0.7, whose protocol stops at vk.xml 1.3.269, passes only `maintenance5` and the EXT form of load-store-op-none. No host change can fix this; a newer Venus in the guest can.
* 32-bit WineD3D on Vulkan needs `VK_EXT_map_memory_placed`: holds, for Direct3D 11 and 9, and the same mapping failure stops Wine's own vkd3d for 32-bit Direct3D 12. Wine's other route, `VK_EXT_external_memory_host`, the host's lavapipe has but Venus does not pass; the host's lavapipe lacks `VK_EXT_map_memory_placed` itself. Whether a newer Venus would offer the placed mapping over such a host was not checked.
* Chromium lists Venus but gives pages SwiftShader: holds in effect, for another reason. On the Lenovo Venus over hasvk was an "Available" integrated GPU and the reason no page got it stayed open; here Venus over lavapipe is a CPU adapter, and Chromium's own blocklist (crbug.com/40057808) withholds every CPU adapter but its fallback.
* ANGLE on Vulkan dies for lack of `minigbm_allocation`: holds with Ubuntu's virglrenderer 1.2.0 as with Debian's 1.1.0, the same host lines.

## Against the first run's Debian-VM host

The first run (`tonight/qemu-wsl.md`, "A Venus host inside the Debian guest") made the level 2 Debian guest the Venus host (Debian's QEMU 10.0.13 and virglrenderer 1.1.0, Mesa 25.0.7, `/dev/udmabuf` from Debian's kernel, QEMU as root) for a level 3 guest under TCG. Here the WSL host is the Venus host for a level 2 guest under KVM.

| | First run: level 3 guest under TCG, Debian VM as host | Here: level 2 guest under KVM, WSL with the module as host |
|---|---|---|
| Venus | 1.3.269, 108 extensions | 1.3.269, 109 extensions (and `VK_EXT_fragment_shader_interlock`) |
| vkcube | 300 frames in 4.24 s, about 70 a second; not shown: the host's GL refused the frames ("failed to create egl image"), and llvmpipe on vgem crashed QEMU | 300 in 2.7 s, 1500 at 126 to 129 a second; shown, and a picture taken; refused the same way when QEMU's GL is on the RTX |
| Direct3D 11 | Debian's DXVK 2.6.0 drew at 11_0 on Venus | the pinned GE-Proton's DXVK 3.1.1 refuses Venus; WineD3D on Vulkan draws at 11_0 (64-bit) |
| Direct3D 12 | not run | Wine's vkd3d draws at 11_1 (64-bit); vkd3d-proton refuses |
| Speed of the guest | TCG: Wine's first start 30.5 s, a probe 4.7 s | KVM: a probe 1.6 to 2.7 s |
| What the host needs | a VM, QEMU as root inside it | an out-of-tree module loaded by root in WSL, the kernel tainted, nothing on Windows |

So the module gives at level 2 everything the Debian VM gave at level 3, adds a display that shows Venus's frames, and keeps KVM. DXVK's result differs because the DXVK differs: 2.6.0 asks less than the pinned 3.1.1.

## Unloading the module, and what is left

* 08:42:39, as root (`scripts/wsl-unload.sh`, `out/unload.txt`), with the guest powered off: no QEMU and no render server running; `rmmod udmabuf` exit 0; `udmabuf` no longer in `lsmod`, `/dev/udmabuf` gone, no new kernel line. The taint stays 4096 (an out-of-tree module was loaded) until WSL next starts; it changes no behaviour. Nothing names udmabuf under `/lib/modules/6.18.54.1-microsoft-standard-WSL2`, in `/etc/modules`, `modules-load.d`, `modprobe.d` or `/etc/udev/rules.d`, so nothing loads it at boot. The ledger holds only `etude-ledger 1`.
* WSL never went down: `uptime -s` still reads 2026-11-08 16:23:46 (WSL's clock, UTC+5), from before this step. The module was loaded from 08:27:25 to 08:42:39, through the host test, three QEMU boots, vkcube, Proton, Chromium and blobtest, with no kernel message beyond the taint line.
* Removed at 08:43 (`out/end-state.txt`): the work directory `/home/testing/rach2/build/run2/step6/` (9.2 GiB: the kernel tree and its builds, logs, a copy of the scripts), and the host-side shader caches. The outputs were copied first to `run2/step6/out/wsl/`.
* Kept in `/home/testing/rach2/build/cache/run2/step6/` (1.1 GiB): `linux-msft-wsl-6.18.54.1.tar.gz` (256,238,870 bytes) with `dl.log`; `udmabuf.ko` (533,712); `udmabuf-module.diff` (1,232); `kconfig-gcc13.diff` (1,073); `gcc13-view.diff` (1,372); `Module.symvers` (1,080,434); `config-6.18.54.1-gcc14` (195,220); `s6.qcow2` (847,249,408 bytes on disk), the guest's overlay on `/home/testing/rach2/build/cache/run2/step5-7/s5base.qcow2`, which now holds seatd, libvulkan-dev, `~/blobtest`, the Proton prefix `~/cd-s6` and the guest's `~/out`. No base image was written.
* No process of this step is left: `testing` has only its systemd user manager and the two shells it had before the run; no QEMU, render server, make, compiler or download runs in WSL. 902 GiB free on WSL's root. Nothing was changed on Windows, and nothing system-wide in WSL but the module's load and unload.

## Decision A

Decision A asks which host carries Venus with presenting, for DXVK, vkd3d-proton and WebGPU in QEMU. The answer from this step: **the testing PC's WSL itself, with Microsoft's own kernel left in place and udmabuf loaded into it as a module**, a fourth option the plan did not list. Option 2's premise, that udmabuf "cannot be built as a module ... so it is the whole kernel", holds only inside the tree: built out of the tree from Microsoft's source for the exact release, with a three-hunk patch, it loads into the stock kernel and works.

* What it gives, measured: a level 2 guest under KVM gets Venus at Vulkan 1.3.269 with 109 extensions, the swapchain, external memory and synchronization2 among them; vkcube's frames reach the screen and a picture of them was taken; blob memory is coherent and as fast for the CPU as the guest's own; WineD3D on Vulkan and Wine's vkd3d draw on Venus for 64-bit programs. That is more than option 1 (a Debian VM as host: level 3 under TCG and no display) and the same as option 2 without a change to the owner's Windows setup.
* What it costs: root loads it after each start of WSL (or it is set to load at boot, which the brief forbade here and which is the owner's decision); the kernel is tainted; any oops in it panics the whole WSL machine for every account, as the running kernel has `panic_on_oops` and `panic=-1` (none in 15 minutes of use here); and each WSL kernel update needs the module rebuilt from that release's source, configured to the capabilities of the compiler that built the kernel (gcc 13.2 now, so `gcc13-view.diff` with gcc 14; about 4 minutes on 32 threads) and checked against the installed modules' CRCs before it loads.
* Its limits, measured: Venus's frames show only with QEMU's GL on llvmpipe, not on the RTX; every frame waits on QEMU's 10 ms fence poll (126 to 129 frames a second whatever the present mode); DXVK 3.1.1 and vkd3d-proton 3.1 still refuse Venus, because the guest's Venus (Mesa 25.0.7) lacks `VK_KHR_load_store_op_none` and `maintenance6`, which the host has; 32-bit Vulkan programs under WoW64 fail for lack of `VK_EXT_map_memory_placed`; and WebGPU stays on SwiftShader, since Venus over lavapipe is a CPU adapter.
* So for 74-4 the answer is this host, now. For 74-6, the host is no longer the blocker: DXVK and vkd3d-proton on Venus wait on the guest's Venus driver, which is Rach2's own Mesa port and must be newer than the pinned 25.0.7 (one whose Venus passes `VK_KHR_load_store_op_none` and `maintenance6`; which release first does was not read). WebGPU on Venus still needs a GPU Vulkan host, as the plan says.

## What this changes in the plan

* Decision A: add the module as the first option and recommend it; option 2's line on udmabuf is corrected (it builds as an out-of-tree module); option 1 drops to a fallback for a WSL whose kernel will not take the module.
* 74-4: presenting through Venus is measured on the testing PC at level 2: vkcube shows its frames, with QEMU's GL on llvmpipe. The test profile for Venus with a picture uses `-display egl-headless` on the vgem render node with llvmpipe, and reads its picture from the guest (QMP's `screendump` answers "no surface" once the scanout is a dma-buf). Virgl on the RTX and Venus on screen are two profiles.
* 74-4 and 74-6, frame rates: on QEMU 10.2.1 a guest's frame waits up to 10 ms for QEMU to retire its fence; Linux gets 126 to 129 frames a second in every present mode. Gates count frames and correctness and compare with Linux's rate under the same QEMU, never an absolute rate.
* 74-6: the pinned Mesa 25.0.7 is what keeps DXVK and vkd3d-proton off Venus, on this host and on the Lenovo; the plan's Mesa pin for Rach2's Venus driver must move to a release whose Venus passes `VK_KHR_load_store_op_none` and `maintenance6` before 74-6 can run them. Until then WineD3D on Vulkan (64-bit) and Wine's vkd3d are the Direct3D paths on Venus.
* 77: 32-bit Direct3D on Venus needs `VK_EXT_map_memory_placed` in the guest's Venus, the same pin question.
* Blob memory (the plan's blob task): map a blob once and keep it, as step 10 said; here a map costs 7 to 8 ms and an unmap 2 to 4 ms under QEMU 10.2.1.
* Needs: the testing PC's machinery would carry the module, its diff and the rebuild steps, and a check that nothing runs before root loads it.

## Files

On the Mac, `build/flow/coordinator/graphics-research/run2/step6/`:

* `udmabuf-module.diff`, `kconfig-gcc13.diff`, `gcc13-view.diff`; `mod/` (the patched `udmabuf.c`, `Kbuild`); `src-read/` (the source files read).
* `scripts/`: `wsl.sh`, `push.sh`, `guest.sh`, `gfetch.sh`, `vm6.sh`, `look*.sh`, `wsl-*.sh` (the build, CRC checks, load, host test and unload), `c-*.sh` (the command lists run in the guest).
* `hsrc/`: `udmabuf-test.c`, `crcdiff.py`. `gsrc/`: the guest scripts and `blobtest.c`.
* `out/`: the build's CRC checks and analyses (`crc*.txt`, `mod*.txt`, `reconf.txt`, `config.txt`), `load.txt`, `hosttest.txt`, `unload.txt`, `end-state.txt`; `out/wsl/` (the build's tail and gzipped log, CRC lists, symtypes, the host's lavapipe with and without the module, the guest before the module, QEMU's logs and the guest consoles of the three boots); `out/guest/` (Vulkan facts, the screenshots, vkcube logs, the Proton runs, the browser runs, blobtest).
