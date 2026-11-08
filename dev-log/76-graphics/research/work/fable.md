# From no graphics to Crysis under Proton: the Fable report

Written 2026-11-03 against master d0e7c5117, from Rach2's code, the upstream
trees under `build/research/src/` (read, never run) and the web. Paths with
no prefix are in this repository; `src/` means `build/research/src/`. Every
claim ends with its evidence and a confidence: `high` means read in the code
named, `medium` means read in code but with a detail not checked or taken
from a secondary source, `low` means the web or inference. Where I am unsure
the claim names the measurement that settles it.

Two owner decisions arrived while I worked and are folded in: the 32-bit
layer is Linux's full ia32 compat ABI, not the minimum Wine's WoW64 needs
(section 6 sizes it, section 10 places it), and work is sized by scope and
dependencies for nine LLM worker loops, never by human time or dates.

Summary of what I believe:

* Rach2 lacks the whole DRM/KMS layer, device mmap, dma-buf and fences,
  mouse evdev, the sysfs names libdrm reads, a 32-bit code segment and the
  `int 0x80` gate; it has PCI, MSI, PAT, memfd, SCM_RIGHTS, poll on devices,
  namespaces, seccomp, bwrap, OSS audio and a Debian package fetcher.
* The first milestone is a KMS device over the firmware framebuffer with
  dumb buffers, Weston as root with the noop seat backend and libudev-zero,
  and the Debian browsers rendering through llvmpipe. It runs headless in
  QEMU TCG on all three machines and is the fallback on every machine.
* virtio-gpu plus virgl is the second step and the only one that proves
  Wine's Direct3D-to-OpenGL path before the Lenovo exists as a target.
* Haswell is one careful task chain: GGTT only, one render ring, hardware
  contexts, relocations, MSI interrupts, GDRST reset, the firmware's eDP
  mode kept and only the primary plane flipped. The command parser is
  skipped at first by running every batch as secure.
* The ia32 compat layer is its own milestone, independent in files, tested
  by the i686 suites run under the x86-64 kernel.
* Sound is PulseAudio's module-oss and alsa-lib's OSS plugin over the
  existing `/dev/dsp`; no kernel ALSA.
* Unattended Lenovo trials need an iTCO watchdog driver, the existing GRUB
  fallback, the bootlog broadcast and the display engine's pipe CRC.

## 1. What Rach2 has and lacks, layer by layer

### Kernel

PCI. Config access, capability walk, one MSI vector per device and MSI-X
entry 0 exist (`rach2/sys/arch/x86/pciconf.c:73-174`, `rach2/sys/pci.c:344-372`,
`:426-483`, `:496-557`; high). The Lenovo's GPU advertises MSI, 64-bit BARs
at 0xC0000000 (4 MiB registers) and 0xB0000000 (256 MiB aperture), and FLR
(`dev-log/70-wifi-chip/linux/info.txt:17-36`; high). Nothing is missing here.

Interrupts. MSI vectors 48-63, up to four handlers per vector, handlers run
under the big lock, `wq_wake` is safe from a handler
(`rach2/sys/arch/x86/isr.h:95-99`, `rach2/sys/irq.h:27`,
`rach2/sys/arch/x86/isr.c:485-564`, `:694`, `rach2/sys/proc/wait_queue.h:96-106`;
high). There is no kernel-thread API; the wireless code hand-builds one
(`rach2/sys/network/wireless/work.c:180-216`; high). A GPU driver that
completes fences and vblank events from its handler fits this model; the
hang checker can run from the tick.

Device memory. `map_mmio` and `map_mmio_typed` bump-allocate from a 128 MiB
window with no unmap (`rach2/sys/mm/paging.c:874-916`,
`rach2/sys/arch/x86_64/cpu.h:145-146`; high). PAT has WB, WC, UC- and UC
(`rach2/sys/arch/x86/pat.c:12-17`; high). The Haswell aperture alone is 256
MiB and the GGTT lives at BAR0 + 2 MiB (`src/linux/drivers/gpu/drm/i915/gt/intel_ggtt.c:1134-1172`;
high), so the window must grow or the aperture be mapped in pieces on demand.
There is no contiguous frame allocator, no scatterlist and no IOMMU
(`rach2/sys/mm/paging.h:407` is the only DMA-pool mention; high); both GPU
drivers work from lists of 4 KiB frames, which the GGTT and virtio-gpu take
as they are, so this is no obstacle.

User mappings of devices. `mmap` requires `FD_TYPE_FILE`
(`rach2/sys/syscall_proc.c:6005-6015`), `struct file_ops` has read, write,
poll, ioctl and close but no mmap (`rach2/sys/fs/file.h:23-39`), and the only
device mapping is `/dev/mem`: privileged, `MAP_SHARED`, uncached, below 4
GiB (`rach2/sys/syscall_proc.c:6079-6121`; high). `map_user_frame` takes any
PTE flags (`rach2/sys/mm/paging.c:1362-1375`; high). Lacking: an mmap
operation on device files, mappings with WB, WC or UC chosen by the driver,
fault-in of GEM pages, and mmap of dma-buf descriptors. This is the first
piece every upstream driver needs.

Shared memory and descriptor passing. `memfd_create`, `/dev/shm`, SCM_RIGHTS
with up to 253 descriptors (`rach2/sys/syscall_fs.c:11084-11140`,
`rach2/sys/fs/vfs.c:3292-3307`, `rach2/sys/ipc/unix.c:40,74-75`; high).
Wayland, PulseAudio and Chromium need exactly these. Present.

Poll on device descriptors. `poll_fd` calls `ops->poll`
(`rach2/sys/io/poll.c:70-92`; high). Present.

devfs. A static table (`rach2/sys/fs/devfs.c:207-214`, `:1595-1650`) with
`/dev/input/event0-31` as 13:64-95 mode 0600 (`:1619-1628`), `mknod` in `/dev`
refused (`rach2/sys/syscall_fs.c:11989-11992`), bare `fb` and `mouse` names
(`:2575-2589`; high). Lacking: `/dev/dri/card0` (226:0) and
`/dev/dri/renderD128` (226:128), which libdrm finds by `access()`
(`src/libdrm/xf86drm.c:100`, `:1022-1060`; high).

sysfs. Synthetic and read-only: a PCI device directory with `uevent` and
`config` (`rach2/sys/fs/synthfs.c:1045-1066`), `/sys/dev` with block devices
only (`:1280-1308`), classes net, ieee80211, rfkill, rtc and backlight
(`:1348-1376`; high). Lacking everything libdrm, libudev and Weston read:
`/sys/dev/char/226:0`, `/sys/class/drm/card0`, `/sys/class/input/eventN`,
and under each device `subsystem`, `vendor`, `device`, `subsystem_vendor`,
`subsystem_device`, `revision`, `boot_vga`, `uevent` with `DEVNAME`,
`MAJOR`, `MINOR` and `PCI_SLOT_NAME`, and the input `capabilities/*`,
`properties`, `id/*`, `name` files (section 2 cites the readers).

uevents. `NETLINK_KOBJECT_UEVENT` sockets open but hear nothing
(`rach2/sys/network/socket/netlink.h:59-61`; high). Weston and libinput
enumerate at start and need no uevent; hotplug is the only loss.

evdev. Keyboards only (`rach2/sys/input.c:46-69`: keyboards list, a
`mouse_present` flag; `rach2/sys/evdev.c:121-147` registers, `:198-252`
reports with `CLOCK_REALTIME` stamps at `:209-211`, `:363-414` read and poll,
`:485-523` `EVIOCGBIT`, `:541-619` the ioctl set; high). The mouse goes out
as packets on `/dev/mouse` (`rach2/sys/mouse.h:13-35`; high). Lacking: the
mouse and touchpad as evdev devices, `EVIOCGRAB`, `EVIOCREVOKE`,
`EVIOCSCLOCKID`, `EVIOCGPROP`, `EVIOCGKEY/GLED/GSW` state reads, and
`CLOCK_MONOTONIC` stamps, which libinput sets through `libevdev_set_clock_id`
(`src/libinput/src/evdev.c`, two uses; high).

Console. `KDSETMODE` is defined (`rach2/sys/kd.h:30-33`) but not handled
(`rach2/sys/kd.c:547-588`), `TIOCLINUX` only blanks
(`rach2/sys/syscall.c:2020-2043`), there are no `VT_*` ioctls and no
`/dev/tty0` (high). seatd's VT-bound mode needs them
(`src/seatd/seatd/seat.c:68-74`, `:84-110`); section 2 shows how to avoid
them.

Framebuffer. `/fb` has an owner stack with console hand-back and keyboard
state per owner (`rach2/sys/display/fbdev.c:24-45`; high), the console window
is WC (`rach2/sys/arch/x86/boot.c:108-133`; high), the Haswell backlight is
already driven (`rach2/sys/display/intelbl.c:28-47`, `:219-271`; high). The
KMS device must take the screen as an owner would, so the console returns
when the compositor exits.

Sound. An OSS 3 `/dev/dsp` with fragments, `GETOSPACE`, `GETOPTR`,
`GETODELAY`, trigger and kernel mixing of several openers
(`rach2/sys/audio/dsp.c:280-309`, `:798-873`, `:889-943`,
`rach2/sys/audio/vchan.c:110-260`; high), a `/dev/mixer`
(`rach2/sys/audio/mixer.c:269-331`; high). No ALSA, and no OSS 4 information
ioctls (`SNDCTL_SYSINFO`, `SNDCTL_AUDIOINFO`, `SNDCTL_ENGINEINFO`), which only
Wine's `wineoss.drv` wants (`src/wine/dlls/wineoss.drv/oss.c`; high).

x86-64 entry and signals. The GDT is 0x10 kernel code, 0x18 kernel data,
0x2B user data, 0x33 user code, TSS at 0x38, and index 4 (selector 0x23) is
empty (`rach2/sys/arch/x86_64/percpu.h`, `percpu.c:94-105`; high). The user
data descriptor `0x00CFF2000000FFFF` is a flat 4 GiB 32-bit data segment
(G=1, D/B=1), usable by 32-bit code as SS and DS; the user code descriptor
has L=1 and serves 64-bit code only (`percpu.c:77-80`; high). Entry is
`syscall`/`sysretq` only (`percpu.c:139-142`, `isr_handler.s:470-532`); the
IDT has no gate at 0x80 (`idt.c:86-163`; high). Signal return forces CS and
SS back to the 64-bit selectors (`rach2/sys/arch/x86_64/sigframe.c:299-300`,
`:305-311`, `:359-360`; high), which must change for any 32-bit code. The
table has 267 calls; missing among those this stack asks for: `futex_waitv`
(449), `rseq` (334), `modify_ldt` (154), `set_thread_area` (205),
`get_thread_area` (211), `membarrier`, `userfaultfd`, `process_vm_*`,
`openat2`, `memfd_secret` (`rach2/sys/arch/x86_64/systab.c:124-395`; high).
`eventfd` and `eventfd2` exist (`systab.c:359,365`; high), so Wine's esync
has its primitive. `arch_prctl` sets FS and GS
(`rach2/sys/arch/x86_64/tls.c`, 74 lines; medium, the GS half needs a
one-line test, see section 9).

Namespaces, seccomp, bwrap. User, mount, pid and net namespaces, seccomp
filters and the bubblewrap port exist, and Chromium's headless shell already
runs inside them (`rach2/sys/proc/ns.h:23-29`, `rach2/sys/proc/seccomp.c:132-301`,
`vendor/gnu/ports/bubblewrap.sh:2-12`, `vendor/gnu/gnu_rows.sh:1173-1220`;
high). The browsers' GPU and renderer sandboxes need nothing new here.

Clocks. `clock_gettime`, `timerfd`, a vDSO with the monotonic clock
(`rach2/sys/syscall.c:370-558`, `rach2/sys/io/timerfd.c:639-700`,
`rach2/sys/arch/x86_64/vclock.c:163-205`; high). `DRM_CAP_TIMESTAMP_MONOTONIC`
is a capability the new driver answers with 1.

### Userland

The GNU root is 79 ports built from source and Debian trixie packages for
Chromium's headless shell, fetched by a closure solver over the Debian
package index, extracted and installed into the root
(`vendor/gnu/Readme.md`, `vendor/gnu/chromium/fetch_chromium.sh:106-168`
closure, `:170-257` fetch, `:259-301` extract, `:347-436` install;
`vendor/gnu/install_root.sh:200-236`; high). The port builder knows
configure-style builds and not meson (`vendor/gnu/build_port.sh:3-54`; high),
which rules out building Mesa or Weston as ports without new tooling. The
architecture list is in `vendor/gnu/arch.sh:19-47` (high); adding `i386`
there is the start of the multiarch root (section 6).

The i686 build has its own 32-bit call layer, userland and suites
(`rach2/sys/arch/i686/systab.c`, 379 lines, `sys_ia32.c`, 82, `sigframe.c`,
301, `tls.c`, 123; `tests/native/Readme.md:3-81`, `tests/native/musl.cases`;
high). The i686 kernel has no vDSO (`rach2/sys/arch/i686/vdso.c:1-4`; high),
so its programs enter through `int 0x80` alone, which is what the compat
layer offers first.

## 2. The kernel interfaces each upstream piece needs

### libdrm and Mesa's loader

* Nodes `/dev/dri/card%d` and `/dev/dri/renderD%d`, major 226, found by
  `access()` (`src/libdrm/xf86drm.c:100`, `:1022-1060`; high).
* For each node: `readlink /sys/dev/char/M:m/device/subsystem` must end in
  `pci` (a virtio device's parent is used), `.../device/uevent` must carry
  `PCI_SLOT_NAME`, and `.../device/{vendor,device,subsystem_vendor,
  subsystem_device,revision}` are read, falling back to `config`
  (`xf86drm.c:3575-3631`, `:3715-3735`, `:3823-3905`; high).
* `DRM_IOCTL_VERSION` gives the driver name, which chooses the Mesa driver:
  `i915` loads Crocus (`src/mesa/src/intel/dev/intel_kmd.c:33-40`),
  `virtio_gpu` loads virgl (`src/mesa/src/loader/pci_id_driver_map.h:65`),
  and `MESA_LOADER_DRIVER_OVERRIDE` forces one
  (`src/mesa/src/loader/loader.c:679-755`, `:742`; high).
* GBM falls back to `kms_swrast` on a KMS device without a hardware driver
  (`src/mesa/src/gbm/backends/dri/gbm_dri.c:241-299`; high).

DRM core ioctls the stack uses (high unless noted): `VERSION`, `GET_CAP`
(`DUMB_BUFFER`, `PRIME`, `TIMESTAMP_MONOTONIC`, `CURSOR_WIDTH/HEIGHT`,
`ADDFB2_MODIFIERS`, `SYNCOBJ`, `SYNCOBJ_TIMELINE`, `CRTC_IN_VBLANK_EVENT`),
`SET_CLIENT_CAP` (`UNIVERSAL_PLANES`, `ATOMIC`, `ASPECT_RATIO`,
`WRITEBACK_CONNECTORS`), `SET_MASTER`/`DROP_MASTER`
(`src/seatd/common/drm.c:10-21`), `GEM_CLOSE`, `PRIME_HANDLE_TO_FD`,
`PRIME_FD_TO_HANDLE`, the `SYNCOBJ_*` family, and the KMS set:
`GETRESOURCES`, `GETCRTC`, `GETCONNECTOR`, `GETENCODER`,
`GETPLANERESOURCES`, `GETPLANE`, `GETPROPERTY`, `GETPROPBLOB`,
`OBJ_GETPROPERTIES`, `OBJ_SETPROPERTY`, `CREATEPROPBLOB`,
`DESTROYPROPBLOB`, `ADDFB`, `ADDFB2`, `RMFB`, `GETFB2`, `SETCRTC`,
`PAGE_FLIP`, `CURSOR2`, `ATOMIC`, `CREATE_DUMB`, `MAP_DUMB`,
`DESTROY_DUMB`, `WAIT_VBLANK`, and events (`DRM_EVENT_VBLANK`,
`DRM_EVENT_FLIP_COMPLETE` with the CRTC id) read from the card descriptor.
Linux's reference sizes: `drm_ioctl.c` 887, `drm_gem.c` 1535, `drm_prime.c`
1086, `drm_syncobj.c` 1719, `drm_atomic_uapi.c` 1538, `drm_atomic.c` 1861,
`drm_connector.c` 3452, `drm_plane.c` 1795, `drm_property.c` 999,
`drm_framebuffer.c` 1208, `drm_dumb_buffers.c` 151, `drm_vblank.c` 2163,
`drm_file.c` 996 lines (`wc -l` on `src/linux/drivers/gpu/drm/`; high). A
Rach2 core carrying only what is listed above is a third of that.

### llvmpipe and kms_swrast on a dumb-buffer KMS device

`CREATE_DUMB`, `MAP_DUMB` and `mmap` of the returned offset on the card
descriptor, `DESTROY_DUMB`, and PRIME export for handing the buffer to the
compositor (`src/mesa/src/gallium/winsys/sw/kms-dri/kms_dri_sw_winsys.c:195-272`,
`:329`, `:467`; high). GBM creates dumb buffers the same way
(`gbm_dri.c:831-852`; high). The mapping is write-back memory. Weston's
pixman renderer uses only this; its GL renderer on `kms_swrast` adds EGL on
GBM and `zwp_linux_dmabuf` import, so the pixman renderer is the first
target and the GL renderer the second on the same kernel interface.

Mesa's Wayland EGL client side has two paths: dma-buf feedback and
`zwp_linux_dmabuf_v1` for hardware drivers, and `dri2_wl_swrast_*` over
`wl_shm` for software (`src/mesa/src/egl/drivers/dri2/platform_wayland.c`,
165 and 16 uses against 16 swrast functions; high). Weston 14's GL renderer
imports `linux_dmabuf` buffers and has no `wl_drm`
(`src/weston/libweston/linux-dmabuf.c`, no `wl_drm` symbol in
`renderer-gl/gl-renderer.c`; high). So a hardware client needs PRIME export
in the client, PRIME import in Weston, and dma-buf descriptors crossing the
Wayland socket; a software client needs only memfd and `wl_shm`.

### Mesa virgl

* Parameters, in a loop over `DRM_IOCTL_VIRTGPU_GETPARAM`: `3D_FEATURES`
  (required), `CAPSET_QUERY_FIX`, `RESOURCE_BLOB`, `HOST_VISIBLE`,
  `CROSS_DEVICE`, `CONTEXT_INIT`, `SUPPORTED_CAPSET_IDs`
  (`src/mesa/src/gallium/winsys/virgl/drm/virgl_drm_winsys.c:1214-1240`,
  `virgl_drm_winsys.h:69-88`; high).
* Capsets 1 and 2 through `GET_CAPS`, and `CONTEXT_INIT` with the capset id
  (`virgl_drm_winsys.c:53-54`, `:1006-1021`, `:1167-1197`; high).
* `RESOURCE_CREATE` (`:253-300`), `RESOURCE_CREATE_BLOB` with `HOST3D` and
  `MAPPABLE` (`:170-241`), `MAP` then `mmap` of the offset (`:698`, write
  combining for host-visible memory), `TRANSFER_TO_HOST` and `FROM_HOST`
  (`:375`, `:404`), `EXECBUFFER` with `FENCE_FD_IN` and `FENCE_FD_OUT`
  (`:637`, `:973`, sync_file descriptors), `WAIT` (`:124`, `:724`),
  `RESOURCE_INFO` (`:566`), PRIME both ways (`:525`, `:673`; high).
* Linux's driver is `virtgpu_ioctl.c` 735, `vq.c` 1304, `fence.c` 175,
  `object.c` 249, `gem.c` 296, `prime.c` 170, `plane.c` 408, `display.c`
  371, `kms.c` 352, `submit.c` 542, `vram.c` 228 lines (high). Rach2 has no
  virtio transport at all (`grep -rl virtio rach2/sys` is empty; high), so
  a virtio PCI core (modern capabilities, split virtqueues, MSI-X per queue)
  comes first and serves virtio-net and virtio-blk later.

### Mesa Crocus on Haswell

Device information (`src/mesa/src/intel/dev/intel_device_info.c:1866-1960`,
`src/mesa/src/intel/dev/i915/intel_device_info.c:230-630`; high):
`drmGetDevice2` for the PCI revision (so the sysfs `revision` file matters),
`GETPARAM` `CHIPSET_ID` (0x0a16), `REVISION`, `HAS_LLC`, `GET_APERTURE`,
`CONTEXT_GETPARAM` `GTT_SIZE`, `MMAP_GTT_VERSION` >= 4, `HAS_USERPTR_PROBE`,
`HAS_CONTEXT_ISOLATION`; `CS_TIMESTAMP_FREQUENCY`, `QUERY` topology and
memory regions are optional before gen 10. The bit-6 swizzle probe creates a
GEM object and reads its tiling and calls `unreachable()` on failure, so
`GEM_CREATE` and `GET_TILING` must work at screen creation.

Buffer manager (`src/mesa/src/gallium/drivers/crocus/crocus_bufmgr.c`; high):
`GEM_BUSY` (:263), `GEM_MADVISE` (:280), `GEM_CREATE` (:359), `SET_DOMAIN`
(:378-382), `SET_CACHING` (:442), `USERPTR` (:490, only with LLC:
`crocus_screen.c:479`), `GET_TILING` (:583, :1273), `MMAP` (:793),
`MMAP_OFFSET` WB or WC (:805-841), `MMAP_GTT` (:974-977), `GEM_WAIT` (:1138),
`SET_TILING` (:1199), `CONTEXT_CREATE` with `SETPARAM` priority and
recoverable (:1522-1567), `CONTEXT_DESTROY` (:1591); `has_llc`,
`has_tiling_uapi` and `has_mmap_offset` chosen at creation (:1600-1640).
Contexts are required: every batch gets a hardware context id and asserts it
(`crocus_batch.c:209-212`), and a reset replaces the context
(`:776-781`, `:794-830` with `GET_RESET_STATS`; high).

Submission (`crocus_batch.c:841-892`; high): `EXECBUFFER2_WR` with
`I915_EXEC_RENDER | NO_RELOC | BATCH_FIRST | HANDLE_LUT`, `rsvd1` the
context id, `FENCE_ARRAY` of syncobjs, relocation lists with presumed
offsets (`:178-227`, `:376-450`; `EXEC_OBJECT_WRITE`, `NEEDS_GTT` and
`SUPPORTS_48B` cleared). No softpin: Crocus never pins. Only the render
engine is used, by the render and the compute batch alike (`:876`,
`crocus_context.c:333`; high), so the blitter, BSD and VEBOX rings are not
needed. Fences are syncobjs (`crocus_fence.c:39-57`, `:194-201`; high).

The command parser. Crocus never reads `I915_PARAM_CMD_PARSER_VERSION`
(no use anywhere under `src/mesa/src/gallium/drivers/crocus` or
`src/mesa/src/intel`; high). But its batches write registers:
`MI_LOAD_REGISTER_IMM` and `_MEM`, `MI_MATH`, `MI_PREDICATE`, the `CS_GPR`
registers, `SO_WRITE_OFFSET` and the `3DPRIM_*` registers (84 hits in
`crocus_state.c`, 21 in `crocus_query.c`, 6 in `crocus_program.c`, 4 in
`crocus_draw.c`; high). Haswell drops such writes in a non-secure batch;
Linux's parser scans the batch and submits it as secure
(`src/linux/drivers/gpu/drm/i915/i915_cmd_parser.c:44-61`; high). Without a
parser, Rach2 has two choices: non-secure batches, which silently break
transform feedback, conditional rendering, query arithmetic and indirect
draws, or secure batches for everyone, which lets any GPU client reach the
GGTT and the status page. Section 10 picks secure-for-all first and the
parser (1,624 lines of reference) as a later task.

Tiling and swizzle. Scanout buffers are X-tiled and textures Y-tiled through
`SET_TILING`; CPU access to tiled resources goes through Mesa's own tiled
copy using `has_bit6_swizzle` (`crocus_resource.c:1587`; high), so the
kernel must report the real swizzle (Linux derives it from the memory
controller's channel configuration; medium) and fence registers are only
needed for `MMAP_GTT` of tiled objects, which can wait.

### Weston's DRM backend

* Required: `DRM_CAP_TIMESTAMP_MONOTONIC` = 1 and `UNIVERSAL_PLANES`;
  atomic is used when `CRTC_IN_VBLANK_EVENT` and `DRM_CLIENT_CAP_ATOMIC`
  succeed and `WESTON_DISABLE_ATOMIC` is unset, else the legacy
  `drmModeSetCrtc`/`drmModePageFlip` path, which still exists and disables
  overlay planes (`src/weston/libweston/backend-drm/kms.c:1852-1915`,
  `:801-902`, `:1532-1541`, `drm.c:3736-3737`; high). Cursor size from caps,
  default 64 (`kms.c:1864-1875`; high).
* Properties by name (`kms.c:181-231`): connector `EDID`, `DPMS`, `CRTC_ID`,
  plane `type`, `SRC_*`, `CRTC_*`, `FB_ID`, `IN_FORMATS` blob with modifiers
  (`:565-622`); missing optional ones are logged, not fatal (medium). A head
  without EDID gets make and model "unknown" (`modes.c:304-388`, `:624-628`;
  high).
* Device discovery through libudev: subsystem `drm`, sysname `card[0-9]*`,
  the PCI parent's `boot_vga`, `ID_SEAT` defaulting to `seat0`, hotplug
  through a udev monitor (`drm.c:3613-3632`, `:3558-3563`, `:3329-3396`;
  high).
* Seat: `libseat_open_seat` and `libseat_open_device`
  (`src/weston/libweston/launcher-libseat.c:126`, `:250`; high). libseat's
  backends are logind (needs systemd-logind or elogind), seatd (a daemon
  that opens `/dev/tty0` and switches VTs unless `SEATD_VTBOUND=0`,
  `src/seatd/seatd/seat.c:68-74`, `server.c:42`) and noop, which opens the
  device paths directly and is chosen by `LIBSEAT_BACKEND=noop`
  (`src/seatd/libseat/backend/noop.c:106-127`; high). Weston as root with
  the noop backend needs no VT, no daemon and no `KDSETMODE`.
* libudev without udevd: Debian's `libudev1` is systemd's and takes device
  properties from the udev database that `udevd` writes; libinput's udev
  backend requires `ID_INPUT` and `ID_INPUT_*` (listed in its `udev-seat.c`
  and `evdev.c`; high) and ignores devices without them. libudev-zero
  derives these from `/sys/class/input/*/capabilities`
  (`src/libudev-zero/udev_device.c:423-513`), enumerates from `/sys/dev/char`
  (`udev_enumerate.c:281`), reads `uevent` for `DEVNAME`, `MAJOR`,
  `SUBSYSTEM` (`udev_device.c:85-127`, `:342-370`) and listens on
  `NETLINK_KOBJECT_UEVENT` (`udev_monitor.c:126`, `:227`; high). It replaces
  `libudev.so.1` in the root, which is its purpose.
* libinput needs libevdev and mtdev (`src/libinput/meson.build:154-156`;
  high) and these ioctls through libevdev: `EVIOCGBIT`, `EVIOCGABS`,
  `EVIOCGNAME`, `EVIOCGPHYS`, `EVIOCGUNIQ`, `EVIOCGID`, `EVIOCGPROP`,
  `EVIOCGKEY`, `EVIOCGLED`, `EVIOCGSW`, `EVIOCSCLOCKID` (62 uses of
  `libevdev_has_event_code`, 18 of `libevdev_get_abs_info`, 2 of
  `libevdev_set_clock_id` in `src/libinput/src/evdev*.c`; high).

### Chromium and Firefox on Wayland

Debian trixie ships `chromium` 156.0.8078.79-1~deb13u1 depending on
`libpulse0`, `libasound2t64`, `libgtk-3-0t64` and `libgbm1`, and
`firefox-esr` 153.6.0esr-1~deb13u1 depending on `libasound2t64`,
`libgtk-3-0t64`, `libdbus-1-3` and X11 libraries, with `pulseaudio` only
suggested (packages.debian.org/trixie/chromium and /firefox-esr, read
2026-11-03; high for the versions and lists). Both are Wayland-capable:
Chromium with `--ozone-platform=wayland`
(chromium.googlesource.com docs/ozone_overview.md; high), Firefox by default
when `WAYLAND_DISPLAY` is set, or with `MOZ_ENABLE_WAYLAND=1` (medium).

What they need from the kernel and the compositor (medium, from the
browsers' documented behaviour; the measurement is `chrome://gpu` and
`about:support` on Rach2):

* Wayland `xdg-shell`, `wl_shm`, `wl_seat` with keyboard and pointer,
  `xdg-decoration` optional, `zwp_linux_dmabuf_v1` for hardware buffers.
* The GPU process opens `/dev/dri/renderD*` before its seccomp sandbox and
  reads the sysfs files libdrm reads; renderers run in user and pid
  namespaces (already proven for the headless shell).
* Software GL is on both blocklists: Chromium needs
  `--ignore-gpu-blocklist` to composite on llvmpipe or virgl and
  `--use-gl=egl` (or ANGLE's GL backend) to pick EGL; WebGL on llvmpipe
  needs the same, or SwiftShader with `--enable-unsafe-swiftshader`.
  Firefox refuses WebGL on llvmpipe unless `webgl.force-enabled` is true
  (bugzilla 1438144 and support.mozilla.org 1347475; medium).
* Shared memory through memfd and SCM_RIGHTS; `dma-buf` mmap and
  `DMA_BUF_IOCTL_SYNC` when GPU memory buffers are native.

### Wine and Proton

* WoW64 on Linux: `alloc_fs_sel` switches to the 32-bit stack, builds a
  `user_desc` with `entry_number`, base, limit ~0 and flags 0x41, and calls
  `set_thread_area` (0xf3) with `int $0x80` from 64-bit code
  (`src/wine/dlls/ntdll/unix/signal_x86_64.c:102-117`; high). The 32-bit
  code selector is hard-coded `0x23` (`:2898`; high). At start it allocates
  with entry -1 and later threads reuse the entry (`:2899-2905`, `:2985`;
  high). The TEB goes in GS with `ARCH_SET_GS`, FS is read back with
  `ARCH_GET_FS` (`:2968-2985`; high). LDT entries are only set on macOS; on
  Linux `ldt_set_entry` prints "No LDT support" and exits (`:2718-2735`;
  high), so the new WoW64 needs no `modify_ldt`. Linux v6.12 accepts
  `int 0x80` from 64-bit user code; its handler checks only user mode and
  that the interrupt is not external (`src/linux/arch/x86/entry/common.c:210-258`;
  high), so Rach2's gate must do the same.
* Reservations: the preloader keeps 0-0x10000, 0x10000-0x110000,
  0x110000-0x68000000, 0x7f000000-0x82000000 and 0x7ffffe000000 +
  0x1ff0000 free (`src/wine/loader/preloader.c:111-122`), and ntdll
  reserves the same ranges with `PROT_NONE`, `MAP_NORESERVE`, trying
  `MAP_FIXED_NOREPLACE` first (`dlls/ntdll/unix/virtual.c:296-305`,
  `:778-779`, `:936-938`; high). Rach2 has `MAP_FIXED_NOREPLACE` and
  accepts `MAP_NORESERVE` (`rach2/sys/syscall_proc.c:5842-5866`,
  `:6170-6193`; high); whether a multi-gigabyte `PROT_NONE` mapping costs
  page tables up front is the measurement in section 9.
* fsync probes `futex_waitv` (449) and turns itself off on `ENOSYS`
  (`dlls/ntdll/unix/fsync.c:64-70`, `:166-177`; high); esync uses eventfd,
  which exists. `wineserver` shares memory through `shm_open` in `/dev/shm`
  (`fsync.c:577`; high). Proton's script sets `WINEESYNC`/`WINEFSYNC`
  unless `PROTON_NO_ESYNC`/`PROTON_NO_FSYNC` (`src/proton/proton:1643-1667`;
  high). `futex_waitv` over Rach2's futex (`rach2/sys/proc/futex.c:223-289`,
  `:355-441`) is about 150 lines.
* Signals: handlers for SIGSEGV, SIGILL, SIGBUS, SIGTRAP, SIGFPE, SIGINT,
  SIGQUIT, SIGUSR1, SIGABRT with `SA_SIGINFO | SA_RESTART | SA_ONSTACK`
  (`signal_x86_64.c:2932-2960`; high): the frame must carry the XSTATE
  with `FP_XSTATE_MAGIC1`, `si_code` and `si_addr` for faults, and CS as
  it was when a 32-bit thread faulted (Rach2 forces 64-bit selectors today,
  section 1).
* Audio: `wineoss.drv` needs OSS 4 headers at build time and is not built
  on Linux (`src/wine/configure.ac:1700-1720`; high); Linux builds carry
  `winepulse.drv` and `winealsa.drv`, so Wine's sound is PulseAudio or ALSA
  (section 7).
* Display: `winewayland.drv` needs `xdg-shell`, `viewporter`,
  `xdg-output`, `relative-pointer` and `pointer-constraints` and uses EGL
  (`src/wine/dlls/winewayland.drv/*.xml`, `opengl.c`; high); `winex11.drv`
  needs Xwayland. Proton 10 builds both i386 and x86_64 Unix sides and a
  `bin-wow64` set (`src/proton/Makefile.in:77`, `:520`, `:568-572`; high).

## 3. The minimum Haswell bring-up

The Lenovo's GPU is 8086:0a16 rev 0b, Haswell ULT GT2, with LLC, aliasing
PPGTT of 31 bits, RC6, RPS and per-engine reset
(`dev-log/70-wifi-chip/linux/info.txt:17-36`,
`src/linux/drivers/gpu/drm/i915/i915_pci.c:311-324`, `:388-405`; high).
Linux's i915 drove this panel on the same machine
(`dev-log/70-wifi-chip/linux/dmesg.txt`; medium). What follows is the least
that makes Crocus and Weston work, in the order the pieces depend on each
other, with Linux's file as the reference for each.

Registers and forcewake. BAR0 is 4 MiB of registers; GT registers on
Haswell sit behind a power well and need the multithreaded forcewake
handshake before access (`intel_uncore.c`, 2892 lines; medium: the exact
register pair is checked by the first register dump on the machine). Map
BAR0 whole (4 MiB) and the aperture on demand.

GGTT. The page table is the upper half of BAR0 (`gt/intel_ggtt.c:1134-1172`:
`gen6_gttadr_offset` is half the 4 MiB BAR), its size comes from the GMCH
control word in PCI config (`:1100-1126`), entries are 32-bit PTEs written
with `iowrite32` and flushed through `GFX_FLSH_CNTL_GEN6` (`:590-658`,
`:188-194`), unused entries point at a scratch page (`:724-736`; high).
Haswell PTEs carry a cache level: LLC-cached for ordinary objects and
uncached for scanout, which is why `SET_CACHING` exists (medium). The
firmware framebuffer is in stolen memory and already mapped at the start of
the GGTT, where `DSPSURF` points today (`rach2/hardware.md:237-240`,
`dev-log/43-machine/notes.md:300-303`; medium: the dump confirms the
offset); keep those entries until the first flip. Stolen memory itself is
never allocated from.

PPGTT. Linux runs Haswell with an aliasing PPGTT that mirrors the GGTT and
is shared by every process (`i915_pci.c:323-324`,
`gt/gen6_ppgtt.c:30-46`, `:291`; high). It gives no isolation, so the
minimum is GGTT only: `RING_MODE_GEN7` without the PPGTT bit
(`gt/intel_ring_submission.c:158-170`; high). Every object is bound at its
first use and unbound when destroyed, with no eviction; the 2 GiB GGTT and
Crysis at low settings leave room (medium: the live-USB measurement in
section 9 reads the game's object total).

Render ring. One ring in a GGTT object: `RING_START`, `RING_HEAD`,
`RING_TAIL`, `RING_CTL` with the valid bit, the status page through
`RING_HWS_PGA`, `MI_MODE` with `STOP_RING` cleared
(`gt/intel_ring_submission.c:76-139`, `:192-270`; high). Batches start with
Haswell's `MI_BATCH_BUFFER_START` form (`:1191`; high). Each request ends
with a `PIPE_CONTROL` that writes the sequence number to the status page
and an `MI_USER_INTERRUPT` (`:1175-1176`, `:1315-1344`; high). Waits and
`GEM_BUSY` compare the status page's number; the interrupt wakes the wait
queue. No blitter, BSD or VEBOX ring (section 2).

Contexts. Crocus needs real context ids and the pipeline state kept between
batches, so switching is `MI_SET_CONTEXT` with the Haswell flags, followed by
`MI_NOOP`, into a per-context image of `context_size` bytes
(`:759-769`, `:945-952`, `:533`; high). The first use of a context saves
whatever state the ring has (`MI_RESTORE_INHIBIT`); the golden render state
(`:477`) and the GPR-clearing switch batch (`:1245-1290`) are mitigations
and can be skipped (medium).

Interrupts. MSI from the device (present on the Lenovo), the display master
enable in `DEIER`, display events acknowledged in `DEIIR`, GT events in
`GTIIR` with `GT_RENDER_USER_INTERRUPT` enabled through `GTIER` and `GTIMR`,
PCH events in `SDEIIR` (`i915_irq.c:422-482`, `gt/intel_gt_irq.c:382-401`,
`:522-554`; high). Vblank is `DE_PIPE_VBLANK_IVB` for the pipe
(`display/intel_display_irq.c:36-68`, `:753-800`; high).

Hang detection and reset. A tick-driven check that the status page's number
advances while the ring has work; on a stall, a full domain reset through
`GEN6_GDRST` waited for with a register poll (`gt/intel_reset.c:269-339`;
high), the ring re-initialised, the context that was running marked guilty
so `GET_RESET_STATS` makes Crocus replace it
(`src/mesa/.../crocus_batch.c:794-830`; high). Rach2 has no heartbeat
requests and needs none: the check runs from the tick under the big lock.

Memory objects. Shmem-like page lists, `GEM_CREATE`, the CPU `MMAP` and
`MMAP_OFFSET` (write-back, since the LLC keeps CPU and GPU coherent;
write-combining when asked), `SET_DOMAIN` as a no-op with a flush,
`SET_CACHING` rewriting the object's GGTT entries, `SET_TILING` and
`GET_TILING` with the real swizzle, `MADVISE`, `USERPTR` pinning user pages
into the GGTT, PRIME export and import (section 2 lists Crocus's calls;
high). Fence registers only when `MMAP_GTT` of a tiled object is needed
(medium: Crocus's tiled CPU copies avoid it).

Execution. `EXECBUFFER2_WR`: bind each object, honour `NO_RELOC` by
checking presumed offsets and patching the few that differ, `HANDLE_LUT`
indexing, `BATCH_FIRST`, the syncobj fence array in and out, and the
secure bit set on every batch at first (section 2, the parser).

Parameters. `GETPARAM` answers for Crocus: `CHIPSET_ID` 0x0a16, `REVISION`
0x0b, `HAS_LLC` 1, `NUM_FENCES_AVAIL` 32, `HAS_EXEC_NO_RELOC`,
`HAS_EXEC_HANDLE_LUT`, `HAS_EXEC_BATCH_FIRST`, `HAS_EXEC_FENCE_ARRAY` 1,
`MMAP_GTT_VERSION` 4, `HAS_USERPTR_PROBE` 1, `HAS_CONTEXT_ISOLATION` 0,
`HAS_GPU_RESET` 1, `CMD_PARSER_VERSION` 0, and `-EINVAL` for the rest
(`i915_getparam.c:27-192`; high for the names, medium for the values
Crocus tolerates). `CONTEXT_GETPARAM` `GTT_SIZE` is the GGTT size.

Display, keeping the firmware's mode. Yes: the plane's state is readable
from `DSPCNTR`, `DSPSTRIDE`, `DSPSURF` and `PIPESRC` (`display/i9xx_plane.c:1085-1129`
does this readout in Linux; high), and a flip is a write of `DSPSTRIDE`,
`DSPCNTR` (format `BGRX888`, the `DISP_TILED` bit for X-tiled scanout) and
`DSPSURF`, which arms at the next vblank (`:153-215`, `:419-506`; high).
The maximum stride for an ILK-class primary plane is given by
`ilk_primary_max_stride` (`:716`; high). Nothing else is programmed: no
mode set, no link training, no PLL, no panel power sequencing, no DDI.
That skips `intel_display.c` 8388, `intel_dp.c` 7004, `intel_ddi.c` 5211
and `intel_pps.c` 1812 lines of reference (high). The one mode Weston sees
is the firmware's 1366x768 at 60 Hz, synthesised without EDID.

Vblank events come from the interrupt above; `WAIT_VBLANK` and the page
flip event are completed from it with monotonic stamps. The cursor is the
hardware cursor plane: `CURCNTR` with the 64, 128 or 256 ARGB mode, `CURPOS`
and `CURBASE` written in that order (`display/intel_cursor.c:397-415`,
`:645-712`; high), its image an uncached GGTT object. The backlight is
`intelbl.c` as it is, exposed as the connector's backlight through the
existing `/sys/class/backlight` (high). DPMS can stay unimplemented at
first; blanking through the existing `blank.c` path is enough.

Two firmware states can defeat the flip: panel self refresh and framebuffer
compression, both of which the firmware may have left on (low). The first
register dump reads `SRD_CTL` and the DPFC control; if either is on, the
driver clears it before the first flip.

Frequency. Without RPS the GPU runs at whatever the firmware left, often the
lowest step (medium). Writing one fixed request through the RPS request
register with forcewake held is a small step (`gt/intel_rps.c`, 2905 lines
of reference, of which the fixed-frequency write is under 60; medium) and
belongs in the ring task as its last subtask; the dump reads the current
frequency status first.

Sizes for Rach2, in C89 (estimates from the reference files above and
Rach2's style; medium): registers, forcewake, GGTT and stolen 500 lines;
objects, mappings, caching, tiling, userptr 900; ring, status page,
interrupts, waits 500; contexts 400; execbuf and relocations 600; reset and
hang check 300; parameters and context ioctls 300; plane flip, vblank,
cursor, pipe CRC 600; sysfs identity 100. About 4,200 lines in a new
`rach2/sys/drm/i915/` beside the DRM core of section 10.

## 4. How the userland gets in

The rule I apply: a Debian trixie binary whenever the package exists and
the kernel is the thing under test, a port only where Debian's binary
cannot work on Rach2.

| Piece | Source | Why |
|---|---|---|
| Mesa 25.0.7-2+deb13u1 (`libgl1-mesa-dri` with crocus, virtio_gpu and swrast, `libgbm1`, `libegl-mesa0`, `libegl1`, `libgles2`, `libwayland-egl1`, `libllvm19`) | Debian | meson and LLVM builds are outside `build_port.sh`; the binary is what Linux users run |
| libdrm 2.4.124-2 | Debian | Crocus uses its own buffer manager; only the core library is needed |
| Weston 14.0.2-1, libinput 1.28.1-1+deb13u1, libwayland 1.23, libxkbcommon, pixman, libdisplay-info, libseat1, seatd 0.9.1-1 | Debian | same; seatd is installed but unused with the noop backend |
| libudev | port of libudev-zero (plain Makefile, no dependencies) installed as `libudev.so.1` | systemd's libudev needs udevd's database; libudev-zero reads sysfs (section 2) |
| Chromium 156.0.8078.79-1~deb13u1 (`chromium`, `chromium-common`, `chromium-sandbox`) | Debian | the fetcher exists for the headless shell; the full browser is the same closure plus GTK |
| Firefox ESR 153.6.0esr-1~deb13u1 | Debian | same fetcher |
| PulseAudio 17.0 with `module-oss`, `libasound2`, `libasound2-plugins` | Debian | section 7 |
| Xwayland | Debian | Wine's X11 driver, section 5 |
| Wine 10.0~repack-6 (`wine64`, `wine32:i386`, `libwine`, `fonts-wine`) | Debian, the i386 half after section 6 | old WoW64 layout, needs the i386 root; proves the compat ABI |
| Proton | GE-Proton release tarball pinned by SHA-256, with Debian's `python3` | not in Debian; Valve's build needs Steam; section 5 |
| i386 multiarch root (`libc6:i386`, `libstdc++6:i386`, `libgl1-mesa-dri:i386`, `libpulse0:i386`, `libasound2:i386`, ...) | Debian i386 packages through the same fetcher | section 6 |

The fetcher. `fetch_chromium.sh` already solves a closure over the package
index, fetches, extracts `data.tar` and installs into the root
(`vendor/gnu/chromium/fetch_chromium.sh:106-168`, `:170-257`, `:259-301`,
`:347-436`; high). Generalising it to `fetch_debs.sh SET` with a package
list per set and an architecture argument (`vendor/gnu/arch.sh:19-47`) is
the whole change; the Chromium closure file is not in the checkout
(`vendor/gnu/chromium/Readme.md:3-33` describes it; medium), so each set
records its closure beside its list. Maintainer scripts never run, so the
GTK browsers need the chores Debian's postinst does: `glib-compile-schemas`,
the gdk-pixbuf loader cache, `fc-cache`, `ldconfig`, `/etc/machine-id` for
D-Bus, the xkb data path (medium; each is one command in `install_root.sh`).

Why not build Mesa or Weston as ports: both are meson projects with LLVM
and many optional dependencies; the GNU root's tooling builds on the Mac
(`vendor/gnu/Readme.md`; high) and has no meson; the time goes to tooling
rather than the kernel, and the result is the same code Debian built. The
exception stands only if a Debian binary cannot run on Rach2 because of a
kernel interface the owner decided not to add, and none is known.

## 5. Proton outside Steam, and Crysis

Ways to run Proton without Steam, from the sources:

* Valve's Proton is a Steam depot, so it is reachable only through a Steam
  client (high). Its `proton` script needs `STEAM_COMPAT_DATA_PATH` and
  exits without it (`src/proton/proton:1987-1996`), reads
  `STEAM_COMPAT_CLIENT_INSTALL_PATH` (`:286-289`, `:949`), and takes the
  verbs `run` and `waitforexitandrun` (`:2009-2016`); `PROTON_USE_WINED3D`
  selects wined3d (`:1638-1639`); `WINEDLLPATH` and the GStreamer plugin
  paths point inside the Proton tree (`:1536-1539`; high).
* GE-Proton is the same tree built in Valve's sniper SDK
  (`src/proton/Makefile.in:27`; high) and published as tarballs; it runs
  with the three variables set by hand. Its Unix side links against the
  sniper runtime's libraries (Debian 11 era), which a Debian 13 root
  satisfies forward (medium: the first run on Rach2 is the measurement).
* umu-launcher wraps GE-Proton in Valve's Steam Linux Runtime through
  pressure-vessel, a bubblewrap container with bind mounts; it needs user
  namespaces or a setuid bwrap and the runtime download (umu-launcher
  FAQ and the Manjaro thread on pressure-vessel; low). Rach2 runs bwrap
  with user namespaces already (section 1), so this is the fallback when
  GE-Proton's libraries clash with the root's.
* Debian's Wine 10 with wined3d is the same Direct3D 9 code as Proton's
  with wined3d forced, minus Valve's patches, and installs from the fetcher
  with no Steam variables (high for the packaging, medium for the claim
  that Crysis needs none of Valve's patches).

My order: Debian's Wine first, because it proves the kernel (compat ABI,
`futex_waitv`, signals, GL through Mesa, PulseAudio) with the least
packaging; then GE-Proton run directly with the variables set, to meet the
owner's "Proton" target; umu and pressure-vessel only if GE-Proton's
libraries fail on the root. The owner's decision stands either way; the
first two share every kernel piece.

Kernel demands common to all three: `futex_waitv` for fsync (else fsync
turns itself off, section 2), eventfd for esync (present), `shm_open` in
`/dev/shm` (present), `MAP_FIXED_NOREPLACE` and large `PROT_NONE`
reservations (present, cost unmeasured), `int 0x80` from 64-bit code and
`set_thread_area` (section 6), signal frames that keep CS, `arch_prctl`
GS, `/proc/self/maps` and `exe`, Xwayland or the Wayland protocols listed
in section 2, and PulseAudio or ALSA.

Crysis itself:

* Edition: the GOG release (1.2.1), DRM-free, with `Bin32` and `Bin64`;
  the 64-bit build is `Bin64/Crysis.exe` and the DirectX 9 renderer is
  chosen with `-dx9` (GOG forum threads on the black-screen start and the
  dx9 switch; medium). Under Wine the DirectX 10 path hangs at load
  without `d3dx10_43` (dxvk issue 783; medium), which `-dx9` avoids; Wine's
  built-in `d3dx9` serves the DX9 path, with winetricks `d3dx9` as the
  fallback (medium).
* c1-launcher (github.com/ccomrade/c1-launcher) is a replacement
  executable that fixes the known start-up crashes and works with the GOG
  files; optional, kept in reserve (medium).
* Audio is FMOD through DirectSound, which Wine routes to its sound
  driver, so PulseAudio (section 7) carries it (medium). Input is
  DirectInput for keyboard and mouse; mouse look needs relative pointer
  motion, which `winex11.drv` gets from XInput 2 under Xwayland and
  `winewayland.drv` from `relative-pointer` (high for the protocols,
  medium for the game). Saves go to `Documents/My Games/Crysis` inside the
  prefix (medium).
* Known Wine and Proton reports: a late-game crash cured by DX9 mode and a
  low frame rate with the 64-bit build (protondb.com/app/17300; low).
* Playability on HD 4400 through wined3d is the open question of goal 2.
  The cheapest measurement is Linux itself on the Lenovo: a Debian live
  USB with i915, Crocus and Wine, `Bin64/Crysis.exe -dx9` at Low, 1366x768
  and 1280x720, with `WINEDEBUG=+fps` printing the frame rate. That number
  is the ceiling Rach2 can reach; it costs one flash-drive write, which the
  owner has authorised, and one trial boot. It should be the first
  Lenovo task of the whole programme (section 10).

## 6. 32-bit on 64-bit

### What Wine's new WoW64 needs (the minimum)

From the code in section 2 (high): a 32-bit user code segment at selector
0x23 (GDT index 4, empty today), `int 0x80` dispatching to the i386 table
from any user code segment, `set_thread_area` and `get_thread_area` with
per-thread GDT entries reloaded at every context switch, `iret` into a
32-bit code segment with SS 0x2b, signal frames that save and restore the
interrupted CS and SS instead of forcing them, and `arch_prctl` GS. No
`modify_ldt`, no 32-bit ELF loading, no compat structures: 32-bit code
never enters the kernel in that mode. About 600 lines, all in
`rach2/sys/arch/x86_64/`.

### What Linux's full ia32 compat ABI is (the owner's decision)

Linux's parts and sizes (`wc -l` in `src/linux`; high): `entry_64_compat.S`
299 lines with the SYSENTER entry (`:50-134`), the SYSCALL-from-32-bit entry
used on AMD (`:183-285`) and the `int 0x80` entry (`:294`);
`syscall_32.tbl` with 452 numbered calls, 99 of them routed to a
`compat_sys_*` wrapper; `signal_32.c` 507; `ldt.c` 696; `tls.c` 296; the
32-bit vDSO (`vdso32/system_call.S` chooses `sysenter` or `syscall` by CPU
feature with `int $0x80` as the base, `:52-60`; `sigreturn.S` 140,
`vclock_gettime.c`, `note.S`). Beyond `arch/x86`, the compat wrappers live
in the subsystems (`kernel/compat.c`, `net/compat.c`, `fs/select.c`,
`kernel/signal.c`, `fs/stat.c`, `ipc/compat.c`, `drm_ioc32.c`,
`i915_ioc32.c`), about 3,000 lines together (medium, from the files'
sizes in a full tree).

The 99 wrappers, by what they translate (from the table; high): pointer
width in `ioctl`, `fcntl64`, `select`, `pselect6`, `ppoll`, `readdir`,
`getdents`, `execve`, `execveat`, `open`, `openat`, `ptrace`, `keyctl`,
`kexec_load`, `open_by_handle_at`, `fanotify_mark`, `arch_prctl`;
structures with `long` or pointers in `stat`, `lstat`, `fstat`, `stat64`
family, `fstatat64`, `statfs`, `fstatfs`, `statfs64`, `ustat`, `sysinfo`,
`times`, `getrlimit`, `setrlimit`, `ugetrlimit`, `getrusage`, `wait4`,
`waitid`, `gettimeofday`, `settimeofday`, `setitimer`, `getitimer`,
`sched_setaffinity`, `sched_getaffinity`, `sendfile`, `truncate`,
`ftruncate`, `lseek`, `mmap`, `clone`; signals in `sigaction`,
`rt_sigaction`, `sigprocmask`, `rt_sigprocmask`, `sigpending`,
`rt_sigpending`, `rt_sigtimedwait`, `rt_sigqueueinfo`, `rt_tgsigqueueinfo`,
`rt_sigsuspend`, `sigaltstack`, `sigreturn`, `rt_sigreturn`, `signalfd`,
`signalfd4`; sockets in `socketcall`, `sendmsg`, `recvmsg`, `recvfrom`,
`recvmmsg`, `sendmmsg` (the `cmsghdr` with 32-bit lengths matters for
SCM_RIGHTS from 32-bit Wayland and PulseAudio clients); vectors in
`preadv`, `pwritev`, `preadv2`, `pwritev2`; time64 variants of
`pselect6`, `ppoll`, `recvmmsg`, `rt_sigtimedwait`, `io_pgetevents`,
`epoll_pwait2`; SysV IPC in `ipc`, `semctl`, `shmctl`, `shmat`, `msgsnd`,
`msgrcv`, `msgctl`; `set_robust_list`, `get_robust_list` (walked at exit);
`timer_create`, `mq_open`, `mq_notify`, `mq_getsetattr`, `io_setup`,
`io_submit`. The rest of the 452 pass through with zero-extended 32-bit
arguments and the i386 argument order for `clone`.

Rach2's assets. The i686 build already has the 291-call table and numbers
(`rach2/sys/arch/i686/sysnr.h`, `systab.c`), the i386 `clone` order and
the `set_thread_area` entry handling (`sys_ia32.c`, `tls.c:79-101`, one GDT
entry today), the 32-bit signal frame layout with the FXSAVE area
(`sigframe.c`, 301 lines) and the ELF32 loader paths of the shared
`rach2/sys/proc/elf.c` and `uimage.c` (high). These are lifted, not
rewritten; what is new is every translation between a 32-bit structure
and the 64-bit kernel's, and the layout rules of a 32-bit process on a
64-bit kernel.

What it takes in Rach2, as tasks (sizes medium):

| Task | Files | Size | Content |
|---|---|---|---|
| C1 entry | `arch/x86_64/{percpu.c,idt.c,isr_handler.s,tls.c,switch.s}`, new `compat_entry.s` | 700 lines | GDT 0x23 and TLS entries 12-14 per thread, `int 0x80` gate, `sysenter` on Intel with its MSRs, the `syscall`-from-32-bit entry on AMD, `set/get_thread_area`, `modify_ldt` with a per-process LDT, `in_compat_syscall()` |
| C2 process | `proc/elf.c`, `proc/uimage.c`, `syscall_proc.c`, new `compat_vdso.s` | 700 | ELF32 loading, 32-bit stack and auxv, personality `PER_LINUX32` and `uname` i686, mmap and brk below 4 GiB, vdso32 with `__kernel_vsyscall`, `__kernel_sigreturn`, `__kernel_rt_sigreturn` and the clock functions |
| C3 signals | new `arch/x86_64/sigframe32.c`, `arch/x86/fpu.c` | 800 | the two 32-bit frames with the 32-bit FP state, compat `siginfo` on delivery and in `waitid`, `rt_sigaction` and `sigaltstack` compat, `sigreturn`, restorers in vdso32 |
| C4 files and time | new `compat_fs.c`, `compat_time.c`, driver `compat_ioctl` hooks | 1,200 | `stat64`, `statfs64`, `flock64`, `iovec`, `select`, `ppoll`, `pselect6`, `epoll_pwait2`, the old 32-bit and the time64 timespec calls, `fcntl64`, split 64-bit arguments (`pread64`, `truncate64`, `fadvise64_64`, `fallocate`, `sync_file_range`), `mmap2`, `_llseek`, `sendfile64`, ioctl pointer translation for DRM (`VERSION`, `GET_UNIQUE`, `WAIT_VBLANK`, `MODE_ADDFB2`), i915 `GETPARAM`, evdev's 16-byte 32-bit `input_event` |
| C5 sockets, ipc, misc | new `compat_net.c`, `compat_sys.c` | 900 | `msghdr` and `cmsghdr`, `socketcall`, `sendmmsg`, `recvmmsg`, SysV `ipc` and its structures, `rlimit`, `rusage`, `sysinfo`, `times`, `itimerval`, affinity masks, robust lists, `futex` with 32-bit timespec, `clone` order |
| C6 root and tests | `vendor/gnu/` fetcher with `i386`, `tests/` rows | 400 | the i386 multiarch root (`ld-linux.so.2`, `/usr/lib/i386-linux-gnu`, `ldconfig`), `wine32:i386` and i386 Mesa; the i686 `tests/native` and `tests/musl` binaries run under the x86-64 kernel as the compat suite, plus i386 `eglinfo` on `kms_swrast` |

About 4,700 lines of kernel code in some 15 files, six workers wide, no
dependency on the graphics milestones (different files), C2 and C3 needing
C1's entry to run but writable before it. The test corpus exists on day
one: the i686 userland and suites the owner named, which make the pass
count the milestone's measure. `sysenter` is a performance step: the vdso32
can carry `int $0x80` alone first, since glibc and musl call whatever
`__kernel_vsyscall` the kernel maps and fall back to `int $0x80` without
one (high: `src/linux/arch/x86/entry/vdso/vdso32/system_call.S:52-60` shows
`int $0x80` as the base sequence).

## 7. Sound for the browsers and Wine

What the consumers speak (high for the package facts): Firefox ESR depends
on `libasound2t64` and only suggests `pulseaudio`, so its ALSA backend is
built in and PulseAudio is loaded at run time when present; Chromium
depends on both `libpulse0` and `libasound2t64` and prefers PulseAudio;
Wine on Linux has `winepulse.drv` and `winealsa.drv`, and `wineoss.drv`
is not built without OSS 4 headers (`src/wine/configure.ac:1700-1720`).
SDL games take PulseAudio or ALSA too.

What Debian trixie ships (high, from the package file lists read
2026-11-03): PulseAudio 17.0 with `module-oss.so` and `liboss-util.so`, and
`libasound2-plugins` with `libasound_module_pcm_oss.so`,
`libasound_module_ctl_oss.so` and the `50-oss.conf` configuration.

The routes:

* alsa-lib's OSS plugin: an `/etc/asound.conf` naming `type oss device
  /dev/dsp` as the default PCM sends every ALSA client to Rach2's `/dev/dsp`
  with no daemon and no kernel change; the kernel's mixing lets several
  clients play at once (`rach2/sys/audio/vchan.c:110-260`; high). This is
  the fallback that always works.
* PulseAudio with `module-oss` on `/dev/dsp`: `write` with
  `SNDCTL_DSP_SETFRAGMENT`, `GETOSPACE` and `GETODELAY`, all of which
  `rach2/sys/audio/dsp.c:280-309`, `:798-853` provide (high for the kernel
  side, medium for the module's exact calls: the measurement is `paplay`
  under QEMU judged by `wavcheck`). It gives Chromium its preferred path,
  Wine its preferred driver and per-stream volume. It needs SCM_RIGHTS,
  memfd and `/dev/shm` (present) and no D-Bus.
* PipeWire: no OSS sink; it needs ALSA in the kernel. Rejected.
* ALSA in the kernel: `pcm_native.c` 4138 lines plus the control device,
  the mmap'd status and control pages, `/dev/snd` nodes and `/proc/asound`
  (`wc -l src/linux/sound/core/pcm_native.c`; high). Nothing above needs
  it. Rejected for now; it can come later for programs that insist on
  `hw:0`.
* OSS 4 information ioctls in `dsp.c`: only `wineoss.drv` wants them and
  nobody ships it. Skipped.

Decision: PulseAudio with `module-oss` as the session's sound server, the
alsa-lib OSS plugin configured as the default PCM for clients that bypass
it. The harness already captures QEMU's audio and judges it
(`tests/cases/machine.audio.json`, `tools/etude/qemu_profile.c:857-895`,
`tests/system/runner/wavcheck.c`; high), so a browser row playing a tone is
a normal case.

## 8. Testing

What QEMU proves, headless, with TCG, on all three machines (high):

* `-device virtio-vga` (2D): the DRM core, dumb buffers, page flips, the
  cursor plane, vblank events, Weston on pixman and on llvmpipe, both
  browsers in software rendering, evdev input through QEMU's USB tablet
  and keyboard, PulseAudio over `/dev/dsp`. The harness captures the screen
  with QMP `screendump` and judges it with `screentext --colors`
  (`tests/system/runner/screentext.c:118-178`, cases such as
  `tests/cases/machine.screen.json:72-75`; high), and judges the serial
  console (`tools/etude/include/etude/scenario.h:146-358`; high). Two knobs
  are missing: `-display none` is fixed in the profile and devices pass
  through as `-device` (`tools/etude/qemu_profile.c:18-23`, `:1274-1293`;
  high), and the accelerator is pinned to TCG with KVM available
  (`qemu_profile.c:819-845`; high).
* `-device virtio-vga-gl` with a GL display (3D): proven on the testing
  host from the Windows desktop session with `-display sdl,gl=on` under
  WSLg, Mesa's d3d12 driver on the RTX, `-accel kvm`, reporting
  `virgl (D3D12 (NVIDIA GeForce RTX 4080 SUPER))` in the guest
  (Codex session rollout-2026-10-06T21-02-38 lines 1298-1307; high).
  `-display egl-headless` fails there for want of a DRM render node
  (same session; high). `tools/Readme.md:62` says WSLg is off since
  833bbdc5a, which the probe contradicts; the GL profile needs WSLg on and
  QEMU started from the user's session. Whether `screendump` reads back a
  GL scanout under SDL is unknown (the egl-headless display copies back
  for this purpose; SDL may not; low). The robust judgment is in the guest:
  the test client reads its frame back with `glReadPixels` and prints a
  checksum and a frame count to the serial console, which the harness
  already judges. A 50-line QEMU patch making egl-headless use EGL's
  surfaceless platform would make virgl fully headless on the testing host
  (low; measure after the SDL route works).
* Wine and wined3d under virgl: Direct3D 9 to OpenGL can be proven in QEMU
  before the Lenovo exists as a target (high that the path is the same,
  medium that Crysis's shaders behave alike on virgl and Crocus).
* The Mac mini: TCG only, every 2D case; virgl needs host GL through
  `-display cocoa,gl=on`, unverified (low; one command measures it).

What only the Lenovo proves (high): Crocus on real Haswell, the GGTT and
stolen layout, the firmware's plane state, PSR and FBC, the DIMM swizzle,
the GT frequency, interrupts and reset, the panel and backlight, the
touchpad and its evdev description, HDA with the browsers and Wine, and
Crysis's frame rate.

Unattended Lenovo work today: trial boots through `bootctl` and the GRUB
fallback (`man/man8/bootctl.8:140-145`, `tools/image/mkdisk_grub.sh:275-324`;
high), the kernel's bootlog broadcast of its records to the network
(`rach2/sys/network/netconsole.h:10-12`, `man/man4/bootlog.4:17-18`; high),
native cases and acceptance on the trial boot (`tests/native/Readme.md`,
`tests/system/accept/metal_accept.sh:7-10`; high). Missing: a reset when
the machine hangs, a watchdog, any view of the screen.

What makes it possible without the owner at the machine:

* An iTCO watchdog driver for the PCH (8086:9C43,
  `dev-log/70-wifi-chip/linux/info.txt:268-274`; TCO base 0x1860 per
  Linux's own boot on the machine, `dev-log/70-wifi-chip/linux/dmesg.txt`;
  medium): arm it when a trial boot starts, let the test runner feed it,
  and a hang reboots the machine into the fallback entry. About 150 lines,
  with the NO_REBOOT bit cleared through the PCH's root complex base
  (medium: Linux's `iTCO_wdt` for Lynx Point-LP is the reference).
* The GPU reset path of section 3 for hangs that stop only the GPU.
* The display engine's pipe CRC as the Lenovo's `screendump`: Haswell
  computes a CRC of every scanned-out frame into `PIPE_CRC_RES` when
  `PIPE_CRC_CTL` is set (the IGT tests use it through i915's debugfs;
  medium). A test draws a pattern through the known `/fb` path and records
  its CRC, then draws the same through the stack under test and compares.
  This judges what left the pipe, by a counter, with no camera. About 60
  lines of driver and a `/proc` file.
* The register dump first: a user program on a trial boot maps BAR0 through
  `/dev/mem` and prints `DSPCNTR`, `DSPSTRIDE`, `DSPSURF`, `PIPESRC`,
  `PIPECONF`, `SRD_CTL`, the DPFC control, `GFX_MODE`, `GAM_ECOCHK`, the
  DIMM registers, the GMCH control word, the forcewake acknowledge and the
  frequency status to the bootlog (high that `/dev/mem` allows it:
  `rach2/sys/syscall_proc.c:6079-6121`). Every Haswell task reads this
  before writing a line.
* A camera on the Mac pointed at the panel is the last resort for what a
  CRC cannot say, such as a frozen but unchanged frame, and needs the
  owner once to place it (low value; not planned).

## 9. Risks, ranked, with the cheapest measurement

1. Haswell ring and context bring-up with the screen as the only output.
   Measure first: the register dump on a trial boot, then a batch of one
   `MI_STORE_DATA_IMM` to the status page, polled without interrupts,
   reported through the bootlog. Do this before any KMS code for Haswell.
2. Crysis's frame rate on HD 4400 through wined3d. Measure: the Debian live
   USB run of section 5 with `WINEDEBUG=+fps`, and the game's GPU object
   total from i915's debugfs for the GGTT question. One flash-drive write
   and one boot; it decides goal 2's settings and resolution.
3. The firmware's display state: PSR, FBC, pipe and plane choice, tiling,
   stride. Measure: the same register dump.
4. Running batches secure without a parser (section 2). Measure in
   QEMU: nothing; on the Lenovo: a transform-feedback and an occlusion
   query sample under each policy. The owner decides the posture after
   the numbers.
5. Weston without VT, udevd and logind. Measure on day one of the first
   milestone in QEMU: `LIBSEAT_BACKEND=noop weston --backend=drm` with
   libudev-zero, `libinput list-devices`.
6. The browsers' GPU process on llvmpipe and virgl: blocklists, sandbox
   reads of `/dev/dri` and sysfs. Measure: `chromium --ozone-platform=wayland
   --ignore-gpu-blocklist --enable-logging=stderr` and `about:support`
   in QEMU 2D first.
7. virgl capture and headless operation on the testing host. Measure:
   `screendump` during a virgl render under SDL; if blank, the in-guest
   checksum stands and the egl-headless patch is tried.
8. The compat layer's breadth (452 calls, 99 wrappers). Measure: the i686
   suites under the x86-64 kernel; the pass count is the progress meter.
9. PulseAudio's module-oss on Rach2's `/dev/dsp` timing. Measure: `paplay`
   in QEMU judged by `wavcheck`.
10. Wine's reservations: multi-gigabyte `PROT_NONE` `MAP_NORESERVE`
    mappings. Measure: a ten-line program mapping 0x7ffffe000000 and the
    low ranges, reading `/proc/self/status` for page-table memory.
11. `int 0x80` from a 64-bit code segment and the per-thread TLS entry.
    Measure: a program that repeats `alloc_fs_sel` and reads `%fs:0` from
    32-bit code after a far jump to 0x23.
12. GE-Proton's libraries on a Debian 13 root. Measure: `proton run
    winecfg` on Rach2 once Wine works; the fallback is umu.
13. Stolen memory and the GGTT size. Measure: the GMCH control word in the
    dump and `rach2/hardware.md:237-240`.
14. Disk on the testing host: Mesa with LLVM, two browsers, Wine, i386
    packages add about 2 GiB to a root that is already large
    (`AGENTS.md`, GNU root 35 GiB; medium). Measure: `status.sh`.

## 10. The milestone split

Sized by scope and dependencies for nine worker loops, with no dates. Two
goals: G1, the browsers with GPU acceleration in QEMU and on the Lenovo;
G2, Crysis under Proton on the Lenovo. Every task below is one issue with
its subtasks, documents and tests, owns its files, and compiles alone.

### M-A: display, input and the browsers in software (G1 base)

Headless in QEMU TCG on every machine; the fallback stack of every machine.

| Task | Owns | Size | Depends on |
|---|---|---|---|
| A0 interface header | `rach2/sys/drm/drm.h`, `dma.h` | 200 lines | nothing; written first, alone, by one worker, before the fan-out |
| A1 device mmap and dma-buf | `rach2/sys/mm/`, `fs/file.h`, new `rach2/sys/dma/` (dma-buf, dma-fence, sync_file, syncobj) | 1,100 | A0 |
| A2 DRM core and KMS | new `rach2/sys/drm/` (nodes, dispatch, GEM handles, dumb, PRIME, master, caps, events, mode objects, properties, blobs, atomic and legacy, ADDFB2, cursor, vblank) | 2,500 | A0; A1 at merge |
| A3 fbdrm | `rach2/sys/display/` (the simpledrm-like device over the firmware framebuffer: one connector, CRTC and primary plane, flips by copy at a 60 Hz tick, vblank from the tick, console hand-back as an `/fb` owner) | 600 | A0; A2 at merge |
| A4 sysfs and uevents | `rach2/sys/fs/synthfs.c`, `procfs.c`, `network/socket/netlink.c` (`/sys/dev/char`, `/sys/class/drm`, `/sys/class/input`, device files, uevent emission) | 700 | nothing |
| A5 evdev completion | `rach2/sys/evdev.c`, `input.c`, `mouse.h`, `usb/hid.c` (mouse and touchpad as evdev, grab, revoke, clock, properties, state reads, monotonic stamps) | 600 | nothing |
| A6 GNU root sets | `vendor/gnu/` (`fetch_debs.sh`, the Mesa, Weston, browser, sound sets, libudev-zero port, postinst chores, kiosk start script, `asound.conf`, `default.pa`) | 900 lines of shell | nothing |
| A7 harness | `tools/etude/`, `tests/cases/` (display and accelerator knobs, `virtio-vga`, KMS, Weston, browser and audio cases, the in-guest GL readback client) | 700 | A2, A3 at run time |
| A8 documents | `rach2/sys/drm/Readme.md`, `man/man4/dri.4`, `drm.4`, `input.4`, `dev-log/` | 400 | all, last |

Eight loops wide after A0. Careful work: A0 and the A1/A2 join. Done:
Weston's kiosk shell shows Chromium and Firefox rendering a WebGL sample
through llvmpipe with keyboard, mouse and sound, in QEMU TCG on the three
machines, judged by `screendump` colours, serial counters and `wavcheck`.

### M-B: virtio-gpu and virgl (G1 in QEMU)

| Task | Owns | Size | Depends on |
|---|---|---|---|
| B1 virtio core | new `rach2/sys/virtio/` (PCI modern capabilities, split virtqueues, MSI-X) | 800 | nothing |
| B2 virtio-gpu 2D | new `rach2/sys/drm/virtio/` (resources, scanout, flush, cursor, EDID, the KMS plane) | 1,200 | B1, A2 |
| B3 virtio-gpu 3D | same directory (capsets, contexts, execbuffer with fences, blob resources and host-visible mapping, transfers, parameters) | 1,000 | B2, A1 |
| B4 harness GL profile | `tools/etude/`, `tests/cases/` (SDL GL under WSLg, KVM, Weston GL on virgl, accelerated browser cases, a WebGL game case with frame counters) | 500 | A7, B3 |

Three to four loops. Done: both browsers report hardware GL on virgl, a
WebGL game runs with a frame counter above a threshold the owner sets.

### M-C: the ia32 compat ABI (G2 prerequisite, independent)

The six tasks of section 6, C1 to C6, 4,700 lines, six loops, no file in
common with M-A or M-B, so it starts on day one with whatever loops M-A
leaves free and fills in as M-A tasks close. Done: the i686 suites pass
under the x86-64 kernel; `wine32:i386` runs a 32-bit program; i386 Mesa
renders on `kms_swrast`.

### M-D: Haswell (G1 on the Lenovo)

| Task | Owns | Size | Depends on |
|---|---|---|---|
| D0 measurements (`owner-only`) | a dump program, the live-USB run of section 5, notes in `dev-log/` | 200 | nothing; first of all Lenovo work |
| D1 registers, forcewake, GGTT, stolen, sysfs identity | new `rach2/sys/drm/i915/` | 600 | A2, D0 |
| D2 objects, mappings, caching, tiling, userptr, PRIME | same | 900 | A1, D1 |
| D3 ring, contexts, interrupts, execbuf, reset, parameters, frequency | same | 1,800 | D1, D2; one worker, serial, with the trial boots one at a time |
| D4 display: flip, vblank, cursor, pipe CRC, backlight property | same, `rach2/sys/display/` | 600 | D1, A2 |
| D5 unattended trials | new `rach2/sys/watchdog/itco.c`, `tools/dev/machine_update.sh`, `tests/native/` GPU cases | 500 | D0 |
| D6 command parser (optional, after D3 works) | same directory | 1,000 | D3; the owner's decision on the security posture |

Four loops after D0, D3 alone on the hardware. Done: Weston's GL renderer
on Crocus, both browsers accelerated on the Lenovo, the WebGL sample, and
`glmark2` within a factor the owner accepts of the Linux number from D0.

### M-E: Wine, Proton and Crysis (G2)

| Task | Owns | Size | Depends on |
|---|---|---|---|
| E1 Wine from Debian | `vendor/gnu/` sets, `rach2/sys/proc/futex.c` (`futex_waitv`), signal CS fix if not in C1, Xwayland in the kiosk, PulseAudio; proven in QEMU under virgl with a Direct3D 9 sample | 600 | M-A, M-B, C1 |
| E2 Proton | `vendor/gnu/` (GE-Proton tarball pinned by SHA-256, `python3`, a `proton-run` wrapper setting the Steam variables, `PROTON_USE_WINED3D=1`; umu as fallback) | 300 | E1 |
| E3 Crysis on the Lenovo (`owner-only`) | the prefix, `-dx9`, Low preset, saves, sound, input, `WINEDEBUG=+fps` in the trial's log | 200 | M-D, E2, C6 for the 32-bit half |

Three loops. Done: Crysis at Low settings on the Lenovo with sound, input
and saves, its frame rate logged; the owner judges playability.

### Order and width

Day one: A0 alone; then A1 to A7 (seven) plus C1 and C4 (two) fill nine.
As M-A tasks close, B1, C2, C3, C5, C6 take their loops; B2 and B3 follow
A2. D0 runs as soon as the owner's flash drive can carry the dump program
and the live USB, independent of everything else, and feeds D1 to D4. M-D
starts when A2 merges and D0 has reported. E1 starts when M-B's virgl is
up; E3 waits for M-D. Where to slow down: A0 (one author, one interface),
the A1/A2 join, D3 (one worker, one machine, one trial at a time), C3 (the
32-bit frames and FP state), and the C5 socket translations.

Contracts for the later driver VM: buffers are GEM handles per descriptor
and cross process boundaries only as dma-buf descriptors; fences are
sync_file descriptors and syncobjs; device loss is `GET_RESET_STATS`, `EIO`
on the descriptor and a uevent; the card node has one master. virtio-gpu
is exactly the protocol a driver VM would speak, so M-B's interface is the
one to keep stable.

## Top ten decisions

1. A KMS device over the firmware framebuffer with dumb buffers, Weston
   on pixman, then llvmpipe, as the first milestone. Rejected: starting
   with virgl or Haswell. The fallback runs headless on all three machines
   in TCG and every later driver plugs into the same core.
2. Weston as root with `LIBSEAT_BACKEND=noop` and libudev-zero in place
   of systemd's libudev. Rejected: VT ioctls, `/dev/tty0`, seatd, logind
   and a udev database. The kiosk needs none of them and each is a
   subsystem of its own.
3. Debian trixie binaries for Mesa, Weston, Chromium, Firefox, Wine and
   PulseAudio through a generalised fetcher; a port only for libudev-zero.
   Rejected: building Mesa and Weston from source. The kernel is the
   variable; the binaries are what Linux users run.
4. Sound through PulseAudio's `module-oss` and alsa-lib's OSS plugin on
   the existing `/dev/dsp`. Rejected: ALSA in the kernel and PipeWire.
   Both browsers and Wine are served with no kernel work.
5. The full ia32 compat ABI as its own milestone from day one, lifted from
   the i686 build and tested by the i686 suites. Rejected: the WoW64
   minimum alone. The owner decided it, and it brings Debian's i386
   packages, old games and `wine32` for nothing more than translation code.
6. Haswell with GGTT only, one render ring, hardware contexts, relocations
   with static placement and no eviction, MSI, GDRST reset. Rejected:
   aliasing PPGTT, eviction and the other rings up front. Linux shares the
   aliasing PPGTT across processes anyway, Crocus uses the render ring
   alone, and eviction waits for a measured need.
7. Every batch runs as secure without a command parser at first, with the
   parser a later scoped task. Rejected: non-secure batches, which break
   transform feedback and queries silently, and the parser first, which
   is 1,600 lines before anything draws. The owner sets the posture once
   the numbers are in.
8. Keep the firmware's eDP mode and only flip the primary plane, with the
   cursor plane, vblank interrupts and the existing backlight. Rejected: a
   display bring-up with mode setting, link training and PLLs, some
   22,000 lines of reference for one panel at one mode.
9. Unattended Lenovo trials by an iTCO watchdog, the GRUB fallback, the
   bootlog broadcast and the pipe CRC as the screen judge. Rejected: a
   camera and waiting for sittings. The CRC is a counter the kernel
   reads; the watchdog turns a hang into a reboot into the known system.
10. Debian's Wine first and GE-Proton run directly with the Steam
    variables second, under Xwayland, with umu only as a fallback, and
    wined3d proven under virgl in QEMU before the Lenovo. Rejected: Steam
    itself, building Proton in Valve's SDK, and winewayland alone. The
    kernel work is the same for all; the packaging is the smallest.
