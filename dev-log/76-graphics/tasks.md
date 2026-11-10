# Graphics milestones 76 to 83: tasks and subtasks

Every task and subtask of the graphics plan (`plan.md` beside this file, draft 6i), in milestone order, for the manager to file. Revised for draft 6g after the four reviews of draft 6f, and for draft 6h after Codex's and Fable's confirmation of 6g; what was done with each finding is in `review-6f/disposition.md`. Merged on 2026-11-09 from three splits made against draft 6e, which stay beside it as `tasks-76.md`, `tasks-77-79.md` and `tasks-80-83.md`. Where the splits disagreed, this file settles it, and `plan.md`'s section "Problems the task split found" lists every problem the splits raised with what was done about it. "Problems item N" in a task names item N of its own split's list, which `plan.md` keeps under that split's name.

Totals: 8 milestones, 141 tasks, 670 subtasks, 76-20's task and five subtasks among them only if the owner takes it (140 and 665 without). By milestone: 76, 39 tasks and 205 subtasks; 77, 11 and 52; 78, 31 and 135; 79, 16 and 78; 80, 14 and 70; 81, 12 and 53; 82, 5 and 25; 83, 13 and 52. 29 tasks are `lenovo` (76-10c, 76-11b, 76-15c, 76-17b, 77-10, 78-1b, 78-4b, 78-5c, 78-6c, 78-10d, 78-11b, 78-11c, 78-12b, 78-13b, 78-14b, 80-2, 80-5, 80-6, 80-7b, 80-9, 81-3b, 81-6d, 81-7, 82-4b, 83-2b, 83-5b, 83-6b, 83-6c, 83-7) and 46 are `linux-host` (76-13a, 76-13b, 76-14a, 76-14b, 76-17a, 76-17b, 76-20, 76-21, 76-22b, 76-22c, 77-2, 77-3, 77-4a, 77-4b, 77-5, 77-6, 77-7a, 77-7b, 77-8, 77-9, 78-14a, 79-6c, 79-8b, 79-8c, 79-8d, 80-1, 80-3, 80-4, 80-7a, 80-7b, 80-7c, 80-8b, 81-2, 81-6b, 81-6c, 82-3, 82-4a, 83-1a, 83-1b, 83-2a, 83-2b, 83-3b, 83-4, 83-5a, 83-6a, 83-6c). The Sitting at the end adds four tasks and seven subtasks, filed after the release as milestone 84; 84-4 joined it when 78-1b's expiry left the Lenovo down. 84-1.2 joined it when 76-11b's keystroke check, which needs someone typing, moved to the sitting.

## Conventions

* All eight milestones form one stack on one branch, `milestone/76-display`. Every task rebases and pushes onto it, so the branch stays linear. A wait on another task, in any milestone, is an ordinary blocker: `code` when the task needs the blocker's code on the branch, `evidence` when it needs a measurement, a reference, a rule or a record and imports no code. The owner's approval of the plan answers decisions A to I: every task follows the plan's recommendation of each, and the plan and these tasks are revised before filing if the owner picks another option. The one `decision` wait left is the downloads card (decision G), filed as an owner card that blocks the tasks it names. The fixtures of decision H are the manager's commit when the branch is cut, and no task waits on it on the board.
* `lenovo` marks a task whose last subtask is a step on the Lenovo. Any loop may take it and does all the work before that step. It prepares the step whole on the issue, as the `machine` skill asks: the commit, the trial kernel or trial root built from it with its SHA-256 and where the Mac fetches it, the exact commands, what to read and what passes; for hardware code that never ran there, the review note (registers checked line by line against Linux 6.12, every wait bounded, interrupts masked until their handler is ready, the host test that runs the very path) and the manager's go; `mixer vol 1`; and a "Lenovo notice:" on the issue ten minutes before a step that plays sound or takes the screen. It then hands the task back with `sh tools/flow/board.sh release N owner-only`, or with `release N trial` when the step boots code never run on the Lenovo and the go is not yet given. A loop of the owner's on the Mac takes it under the lock `build/flow/lenovo` and runs the step, with `mixer vol 1` after every boot. Code never waits on a Lenovo step: a task whose code another task needs and that ends in a Lenovo step is split into a code task that closes on merge and a proof task that carries the step (76-10b and 76-10c, 76-11a and 76-11b, 78-4a and 78-4b, 78-11a and 78-11c, 78-12a and 78-12b, 81-3a and 81-3b, 82-4a and 82-4b). A wait that needs only the code names the code task; a wait on a step's result is `evidence`.
* The Lenovo serves one task at a time. Every step that boots a root of the work boots a trial root from 76-15's slot, since the installed system takes only the one release at the end. A step that takes the screen or the GPU, or ends the boot by a reset, has a boot of its own, Rach2's or Debian's; several cases inside one step's boot are fine, and silent steps that take neither may share one boot of the same kernel, which the manager runs (the `machine` skill, "Taking the Lenovo"). Debian on p5 boots only for the sessions plan.md's decision 11 names.
* `linux-host` marks a task that builds the GNU root, which builds only on an x86-64 Linux host (`vendor/gnu/Readme.md:166`), or needs virgl or Venus, which only the testing PC's WSL has. `release N owner-only` removes the label, since the step left runs on the Mac.
* Two tasks are `owner-only` from the start, as their Labels say: 80-2 and 83-6b (Debian sessions on the Lenovo). 76-16b carries no such label: its root steps in WSL are the manager's setup, done at once after the owner's approval and recorded on the task, and the rest any loop does.
* A subtask's check is one that the Testing section of `AGENTS.md` allows during the work: the hooks, a new test run alone by its author, or the worker's own boot in QEMU or trial boot on the Lenovo. No existing suite runs during the milestone. An acceptance task builds its runner, its case list and the stored Linux results, and runs the runner once on a small part. Each new runner registers its case family: a `tests/cases/` file of its own, its line in `tests/registry` and its block in `tests/system/suites.mk`, naming tools, architectures, memory and per-case `requirements.hosts`, so that a checkpoint slice, which each milestone's section ends with, names small cases and the release runs the families whole. A Done says what the task's own check proves, and where it matters what the slice and the release prove.
* The steps that need the owner at the machine (the touchpad's finger check, the HDMI monitor, Crysis's whole first level) are in the Sitting section at the end, filed after the stack's release as milestone 84, which has no branch; nothing of 76 to 83 waits on them.
* Linux 6.12 is the reference source (`build/research/clone.log:118`), the kernel Debian 13 ships; its `syscall_32.tbl` ends at 462, `mseal`.
* "new" marks a file that does not exist today.

## Names across the splits

The later splits were written beside 76's and sometimes name a task of 76 or 78 whole. In their waits:

* 76-2 means 76-2b, which follows 76-2a.
* 76-3 means 76-3b for syncobjs, and 76-3a where only dma-buf, sync files or udmabuf are needed.
* 76-4 means 76-4a, and 76-4b where PRIME is named.
* 76-5 means 76-5b.
* 76-9 means 76-9a for DRM's `/sys` entries, uevents and the registration interface (76-9a.6), and 76-9b for input.
* 76-10 means 76-10a; 76-10b is the touchpad's code and 76-10c its Lenovo step.
* 76-11 means 76-11a in a `code` wait and 76-11b as `evidence`.
* 76-13 means 76-13a, and 76-13b where IGT's tests are named.
* 76-14 means 76-14b.
* 76-15 means 76-15b in a `code` wait (the data partition and the trial root slot), with 76-15c as `evidence` for a Lenovo step (the partition made on the Lenovo), and 76-15a for the forced ext2 test.
* 76-16 means 76-16a, which holds the virgl profile too, and 76-16b for the Venus profile.
* 76-17 means 76-17a.
* 76-18 means 76-18a for debugfs, and 76-18b for pipe CRCs, EDID override and connector forcing.
* 76-22 means 76-22a for admission and capture, 76-22b for the browser runners and the common record, and 76-22c for the IGT, piglit and glmark2 runners and the fuzzers.
* 78-8 means 78-8a and 78-8b; 78-10 means 78-10c; 78-11 means 78-11a in a `code` wait, 78-11c as `evidence` and 78-11b for presenting; 78-12 means 78-12a in a `code` wait and 78-12b as `evidence`; 78-13 means 78-13b; 78-14 means 78-14b.
* 79-1 means 79-1a and 79-1b; 79-8 means 79-8b for the i386 libraries, and 79-8c and 79-8d where the end of 79 is meant.
* 80-7 means 80-7a for the layer and innoextract, 80-7c for the native games' packages and replays, and 80-7b for their Lenovo step.
* 81-3 means 81-3a in a `code` wait and 81-3b as `evidence`.
* 81-6 means 81-6a to 81-6c, and 81-6d for the Lenovo's walk and VT-d units.
* 82-4 means 82-4a in a `code` wait and 82-4b as `evidence`.
* 83-2 means 83-2a on the testing PC and 83-2b on the Lenovo.

## Shared files

The rule, with every shared file, its owner and the order of the others, is in `plan.md` under Shared files. A task's Files line names the task it edits a shared file after, and that task is among its waits, unless plan.md names the two hunks far apart. Case families, package lists and Readme or manual sections are a file or a section per task. The HD 4400 model is a core and a block file per task: `tests/host/i915_model.c` 78-2a; `i915_model_base.c` 78-2b; `i915_model_ring.c` 78-4a, then 78-8a; `i915_model_reset.c` 78-6a; `i915_model_rc6.c` 78-6b; `i915_model_pipe.c` 78-9; `i915_model_sink.c` 78-10b; `i915_model_ddi.c` 78-10c; 78-4b edits the block that holds a difference, after its owner.

## 76 display

### 76-1 Interface header

The shared contracts for buffers, sharing, fences, device loss and display state, their ownership and lock order in the DRM Readme, each driver's identity as Linux reports it, and the skeleton that gives every later task files of its own.

* Files:
  * `rach2/sys/drm/uapi.h` (new): Linux's DRM, KMS, dma-buf, sync_file, udmabuf and syncobj ioctl numbers and structures, in C89, with Linux's layout and ioctl encoding for each ABI: most structures have one layout, and those with a `size_t` or a pointer (`drm_version` and `drm_unique` among them, build/research/src/linux/include/uapi/drm/drm.h:139, :156, :1043) take each ABI's native sizes, which 79-6b's compat path translates.
  * `rach2/sys/drm/drm.h` (new): the device, driver identity and features, buffer object, fence, display state, device loss and aperture handoff contracts, and the driver hooks that 76-18b reads (connector force, EDID override, CRC source).
  * `rach2/sys/drm/Readme.md` (new).
  * `rach2/sys/drm/sources.mk`, `rach2/sys/dmabuf/sources.mk`, `rach2/sys/virtio/sources.mk` (new; the last empty, for 76-7): the shared DRM core, vkms, virtio-gpu, dma-buf and the virtio transport, which every architecture compiles.
  * `rach2/sys/drm/pc.mk` (new): the drivers that reach PC-only code, `fbdrm.c` (the boot framebuffer from the boot information), `bochs.c` and `drm_client.c` (the console's `fbcon.h`), included from `rach2/sys/arch/x86/sources.mk` into `PC_C_SRC`, since `check-arch` compiles shared files for ARM and AArch64 and refuses in them inline assembly, headers under `sys/arch/` and headers of PC files (tools/verify/archcheck.sh, run by quickcheck.sh:197-213 on every commit); `rach2/sys/arch/arch.h` (one hook, through which `drm_start()` calls the PC drivers' probes).
  * Stub files that compile, each later owned by the task named: `rach2/sys/drm/drm.c`, `drm_gem.c`, `drm_auth.c` (76-4a); `drm_prime.c` (76-4b); `drm_mode.c`, `drm_atomic.c`, `drm_legacy.c`, `drm_edid.c` (76-5a); `drm_vblank.c` (76-5b); `drm_syncobj.c` (76-3b); `drm_client.c` (76-11); `fbdrm.c`, `bochs.c` (76-6); `virtio_gpu.c` (76-8); `vkms.c` (76-12); `rach2/sys/dmabuf/dmabuf.c`, `fence.c`, `sync_file.c`, `udmabuf.c`, `sw_sync.c` (76-3a); `rach2/sys/fs/debugfs.c` (76-18a); `rach2/sys/proc/kcmp.c` (76-4a); `rach2/sys/proc/rseq.c`, `rach2/sys/proc/process_vm.c`, `rach2/sys/mm/mempolicy.c` (76-19). All new.
  * `rach2/sources.mk`, `rach2/sys/arch/x86/sources.mk` (one include line) and `rach2/guest.mk:63`, for the sub-manifests and the stubs outside them, so that no other task of 76 edits them.
  * `rach2/sys/fs/file.h` and `rach2/sys/fs/Readme.md`: `mmap` and `llseek` operations added at the end of `struct file_ops` (file.h:23-39), so the positional initializers elsewhere need no edit.
  * `rach2/sys/fs/devfs.c`, `devfs.h`: a registry for nodes made at run time (`dri/cardN`, `dri/renderDN`, `udmabuf`), beside the static `dev_table[]` (devfs.c:1595); 78-1a's `/dev/watchdog`, 81-1a's `/dev/kvm`, 82-2's `/dev/vfio` and 83-3b's `/dev/vgpu-shm` use it too, so that no later task edits `devfs.c`.
  * `rach2/sys/fs/synthfs.c`, `synthfs.h`, `synthfs.Readme.md`, and `rach2/sys/syscall_fs.c`'s listing of `/dev` (:13909): `/dev`'s subdirectories made one table that both read, where `input` is a special case today (synthfs.c:1543), with `dri` its new row.
  * `rach2/kernel.c`: one `drm_start()` call after the PCI drivers start (kernel.c:3830-3990).
  * `tests/host/drm_abi_hosttest.c` (new); `tests/host/devfs_hosttest.c` and `synthfs_hosttest.c` (cases for the registry and `/dev/dri`).
* Waits on: nothing.
* Labels: none.
* Subtasks:
  * 76-1.1 `uapi.h` from Linux 6.12's uapi headers, the kernel Debian 13 ships, which Mesa 25.0.7 and IGT 2.0 build against. Check: compiles with `-ansi` for both ARCH.
  * 76-1.2 `drm.h` and `rach2/sys/drm/Readme.md`: who holds each reference and what close, fork and exit drop; the lock order under the big kernel lock and what may sleep; device loss; the aperture handoff by which bochs, virtio-vga or a later i915 removes fbdrm and tells the console; each driver's name, description, version, features and bus as Linux 6.12 reports them for i915, virtio_gpu, bochs-drm, simpledrm and vkms; devices found by identity, never by minor number. Check: the Readme names every contract `drm.h` declares.
  * 76-1.3 Directories, sub-manifests, stubs and the includes in `rach2/sources.mk` and `guest.mk:63`. Each stub defines the ioctl handlers its owner will fill, declared in `drm.h` and returning what Linux returns for a missing feature, so that 76-4a's dispatch table names every handler once and later tasks edit only their own files. Check: `make` builds both ARCH, the hook's `check_sources.sh` (`quickcheck.sh:272-273`), which reads `make print-c-src` (`rach2/Makefile:207`), finds every new file listed, and the hook's `check-arch` accepts the shared ones, reading the included sub-manifests once process fix 4 (plan.md, After your approval) is on master, which it is before the branch is cut.
  * 76-1.4 The `mmap` and `llseek` members of `file_ops`, with their contract in `file.h` and `rach2/sys/fs/Readme.md`. Check: both ARCH build with every existing `file_ops` initializer unchanged.
  * 76-1.5 The devfs registry of run-time nodes, present through `devfs_entry_present`, and the table of `/dev`'s subdirectories with `input` and `dri` its rows, so that 82-2's `vfio` directory is a row. Check: cases in `devfs_hosttest.c` and `synthfs_hosttest.c` register a node and find it by path and by device number.
  * 76-1.6 `drm_start()` in kernel.c, calling each driver's probe in the order the handoff needs: bochs and virtio-gpu, then fbdrm on what remains, then vkms when its option is given. Check: an own QEMU boot logs `drm: start` and finds no device, since the probes are stubs.
  * 76-1.7 `tests/host/drm_abi_hosttest.c` and its row in `tests/cases/host.host.json`: the size and offsets of every uapi structure and each ioctl number against Linux's values on both ABIs. Check: the author runs it alone.
* Done: the ABI host test passes on both ABIs, and every later task of 76 compiles against `uapi.h` and `drm.h` while editing only its own files and the row files.

### 76-2a Device mappings

Mapping device and driver memory into programs through `file_ops.mmap`, with lifetime across fork, unmap and exit, revocation, and write-combining. Split from 76-2 because 76-3a, 76-4a and 76-19 need only this half, and the pinning half (76-2b) can follow while they start.

* Files: `rach2/sys/syscall_proc.c` (the mmap path, `mmap_backing` and `mmap_devmem`, 6176-6322); `rach2/sys/mm/vm.c`, `vm.h`, `paging.c`, `paging.h`, `mm.c`, `Readme.md`; `rach2/sys/fs/devfs.c` (`/dev/mem` moved onto the new operation); `man/man2/brk.2` (the page `mmap` resolves to); `man/man4/mem.4`; `tests/host/devmap_hosttest.c` (new); `tests/mmapdevprobe.c` (new).
* Waits on: 76-1 (`code`: the `file_ops.mmap` member and the buffer object contract).
* Labels: none.
* Subtasks:
  * 76-2a.1 `mmap_backing` calls the description's `mmap` operation instead of matching `/dev/mem` by device number (syscall_proc.c:6241), and `mmap_file_check` (syscall_proc.c:6102-6112), which refuses every descriptor but `FD_TYPE_FILE` today, accepts a descriptor of any type whose description has `mmap`, so that DRM nodes, dma-bufs and 81-1a's vCPU descriptors map; `/dev/mem` becomes the first user. Check: own boot, the existing `/dev/mem` probe gives the same output.
  * 76-2a.2 A device region kind in `vm.c` backed by page frames or by physical ranges, with PAT write-combining (`PTE_WC`) where the object asks for it, faulted in on access. Check: host test maps, faults and reads both kinds.
  * 76-2a.3 Lifetime: the region holds a reference on its object; fork copies or drops it by `MAP_SHARED` and `MADV_DONTFORK`; munmap, exec and exit release it; `mremap` follows Linux for device regions. Check: host test counts references through each path.
  * 76-2a.4 Revocation: a driver can zap every program's mapping of an object (device loss, a buffer moved or freed), and a later access faults with `SIGBUS` as on Linux. Check: host test zaps a live mapping and reads the signal.
  * 76-2a.5 `tests/mmapdevprobe.c` over `/dev/mem`, with rows in `tests/Makefile` and `tests/probe-suites`. Check: the author runs it alone in QEMU.
  * 76-2a.6 `rach2/sys/mm/Readme.md`, `brk.2` and `mem.4` describe device mappings and revocation.
* Done: `/dev/mem` maps through `file_ops.mmap`, and the host test passes for fault, fork, unmap, exit and revocation on both ABIs.

### 76-2b Pinning, accounting and windows

The pinning and accounting that GPUs, KVM and VFIO all use, huge pages, large BARs mapped through windows that can be freed, and an i686 display profile it can address.

* Files: `rach2/sys/mm/pin.c`, `pin.h`, `notify.c`, `notify.h` (new); `rach2/sys/mm/vm.c`, `paging.c`, `kheap.c`, `pressure.c`, `pressure.h`, `Readme.md`; `rach2/sys/syscall_proc.c` (the mlock calls); `rach2/sys/arch/x86_64/cpu.h` and `rach2/sys/arch/i686/cpu.h` (the MMIO windows, at :148-149 and :155-156); `rach2/sys/pci.c`, `pci.h` (BAR windows); the man2 page `mlock` resolves to through `man/aliases`; `tests/host/pin_hosttest.c`, `mmnotify_hosttest.c` (new).
* Waits on: 76-2a (`code`: the region kinds and `rach2/sys/mm/` files it rewrites).
* Labels: none.
* Subtasks:
  * 76-2b.1 A pin interface (`pin.h`) that holds page frames of a user range or an object for a device, counted against `RLIMIT_MEMLOCK`, released on unpin, unmap and exit. Check: host test pins past the limit and reads `-ENOMEM` or `-EPERM` as Linux returns them.
  * 76-2b.2 `mlock`, `mlock2`, `munlock`, `mlockall` and `munlockall` lock and account as Linux's, replacing calls that now change nothing. Check: host test reads `VmLck` in `/proc/self/status` after each call.
  * 76-2b.3 Huge pages: 2 MiB mappings of device ranges and pinned objects where both ends align, on x86-64 and on i686 with PAE or 4 MiB pages as the kernel has them. Check: host test maps an aligned and an unaligned range and reads the page sizes used.
  * 76-2b.4 BAR windows: a part of a large BAR at a high address mapped into a window of the kernel's device region and unmapped again, so the region can free a mapping (today it never does, mm Readme:228); the region is `KVA_MMIO`, 128 MiB (cpu.h:148-149). Check: host test maps and frees windows into a 4 GiB BAR placed at 14 TiB, as QEMU places virtio-gpu's.
  * 76-2b.5 The i686 display profile: the largest framebuffer and window i686 can map, recorded in the Readme. Check: an own i686 boot maps std-vga's 16 MiB BAR.
  * 76-2b.6 `rach2/sys/mm/Readme.md` (Limits rewritten) and the `mlock` page.
  * 76-2b.7 The address-space change notice: a callback list on a range of a process's memory, called on unmap, protection change, copy-on-write break, `madvise` drop and truncation, which `rach2/sys/mm/` lacks today (a region's `vm_file` has only `fill`, `share` and `release`, vm.h:65-71). 78-3b's userptr, 81-1b's memory slots, 82-2's DMA maps and 83-5a's shared buffers use it and edit nothing in `rach2/sys/mm/`. Check: `mmnotify_hosttest.c` registers a range and counts each callback.
  * 76-2b.8 A shrinker registration in `pressure`, which today reclaims only the page and buffer caches (pressure.h:7-10), for 78-3b's purgeable buffers. Check: the host test registers a shrinker and sees it called below the low mark.
* Done: the pin, notice and shrinker host tests pass on both ABIs, and the mm Readme no longer lists the MMIO region's missing free as a limit.

### 76-3a Buffers and fences

`/dev/udmabuf` over sealed memfds, dma-buf objects with mmap, size and sync calls, sync files with merge, export and import of sync files on dma-bufs, implicit fences, and the `sw_sync` timeline IGT drives. Split from 76-3: this half is what Debian's software stack probes (17 of 26 programs, step 8), and syncobjs need the DRM core.

* Files: `rach2/sys/dmabuf/dmabuf.c`, `fence.c`, `sync_file.c`, `udmabuf.c`, `sw_sync.c`, `dmabuf.h` (new), `Readme.md` (new); `rach2/sys/syscall_fs.c` (`lseek` and `fstat` on anonymous descriptors through `llseek`); `man/man4/udmabuf.4` (new); `man/man7/dma-buf.7` (new, dma-bufs and sync files); `tests/host/fence_hosttest.c` (new); `tests/udmabufprobe.c` (new).
* Waits on: 76-1 (`code`: the fence and buffer contracts, the devfs registry); 76-2a (`code`: `file_ops.mmap` and device regions).
* Labels: none.
* Subtasks:
  * 76-3a.1 The fence core in `fence.c`: context, seqno, signal, error, callbacks and waits with timeouts, under the big kernel lock. Check: host test signals, fails and waits fences from several contexts.
  * 76-3a.2 dma-buf in `dmabuf.c`: an anonymous descriptor shown as `anon_inode:dmabuf` (procfs.c:2010 prints the name), with mmap, `lseek(SEEK_END)` for the size, a distinct inode in `fstat`, `DMA_BUF_IOCTL_SYNC`, and implicit read and write fences. Check: host test reads size, inode and fences.
  * 76-3a.3 `/dev/udmabuf` in `udmabuf.c`: `UDMABUF_CREATE` and `UDMABUF_CREATE_LIST` over memfds sealed against shrinking, with Linux's errors for an unsealed or misaligned range. Check: `tests/udmabufprobe.c` alone in QEMU.
  * 76-3a.4 Sync files in `sync_file.c`: `SYNC_IOC_MERGE`, `SYNC_IOC_FILE_INFO`, poll, and `DMA_BUF_IOCTL_EXPORT_SYNC_FILE` and `IMPORT_SYNC_FILE` on dma-bufs. Check: the probe exports, merges and waits.
  * 76-3a.5 `sw_sync.c`: a software timeline whose fences a test signals, which 76-18a exposes as `/sys/kernel/debug/sync/sw_sync` (Linux's `CONFIG_SW_SYNC`, linux/drivers/dma-buf/Makefile:7 on hand). Check: host test creates a timeline, a fence and signals it.
  * 76-3a.6 `udmabuf.4`, `dma-buf.7`, `rach2/sys/dmabuf/Readme.md`, rows in `man/aliases`, `tests/Makefile` and `tests/probe-suites`.
* Done: the fence host test passes, and `udmabufprobe` creates, maps, exports and merges in QEMU. The lavapipe and EGL extensions step 8 found missing (`VK_KHR_external_semaphore_fd`, `VK_KHR_external_fence_fd`, `VK_EXT_image_drm_format_modifier`, `EGL_ANDROID_native_fence_sync`, `EGL_MESA_image_dma_buf_export`) are read at the checkpoint.

### 76-3b Syncobjs and producer cases

Binary and timeline syncobjs, and the cases with slow, concurrent and failed producers that prove buffer and fence lifetimes.

* Files: `rach2/sys/drm/drm_syncobj.c`; `man/man7/drm-syncobj.7` (new); `tests/fenceprobe.c` (new); `tests/host/syncobj_hosttest.c` (new).
* Waits on: 76-3a (`code`: fences and sync files); 76-4a (`code`: the DRM file and its handle table); 76-18a (`code`: `sw_sync` in debugfs, which the producer cases drive).
* Labels: none.
* Subtasks:
  * 76-3b.1 Binary syncobjs: create, destroy, handle to and from fd and sync file, wait (all, any, for submit), reset, signal. Check: host test.
  * 76-3b.2 Timeline syncobjs: points, `TIMELINE_WAIT`, `TIMELINE_SIGNAL`, `QUERY`, `TRANSFER`, and the eventfd call. Check: host test over out-of-order points.
  * 76-3b.3 `tests/fenceprobe.c`: a slow producer, eight concurrent producers on one buffer, and a producer that fails with an error, each consumer reading the fence's state and the buffer only after it, over udmabuf, dumb buffers and syncobjs. Check: the author runs it alone in QEMU, with rows in `tests/Makefile` and `tests/probe-suites`.
  * 76-3b.4 The syncobj ioctls and `DRM_CAP_SYNCOBJ` and `DRM_CAP_SYNCOBJ_TIMELINE` answer only for a driver whose identity in `drm.h` lists the feature, as Linux's do (Problems item 23). Check: host test against fake drivers with and without it.
  * 76-3b.5 `drm-syncobj.7` and its rows in `man/aliases`.
* Done: the syncobj host test and `fenceprobe` pass, and a failed producer's error reaches every waiter through the fence.

### 76-4a DRM core

Card and render nodes, master and auth, buffer handles, dumb buffers with their mappings, and `kcmp`. Split from 76-4 because PRIME needs dma-bufs (76-3a), while KMS needs only this half.

* Files: `rach2/sys/drm/drm.c`, `drm_gem.c`, `drm_auth.c`; `rach2/sys/proc/kcmp.c`; `rach2/sys/arch/x86_64/systab.c`, `sysnr.h`, `rach2/sys/arch/i686/systab.c`, `sysnr.h` (the `kcmp` rows); `man/man4/drm.4` (new); `man/man2/kcmp.2` (new); `tests/host/drm_core_hosttest.c` (new); `tests/drmprobe.c` (new).
* Waits on: 76-1 (`code`); 76-2a (`code`: dumb buffers map through `file_ops.mmap`).
* Labels: none.
* Subtasks:
  * 76-4a.1 Nodes `dri/cardN` and `dri/renderD(128+N)` registered per device through the devfs registry, opened as `FD_TYPE_DEVICE`; `DRM_IOCTL_VERSION`, `GET_CAP`, `SET_CLIENT_CAP`, `GET_UNIQUE`, `GET_MAGIC`, `AUTH_MAGIC`, and the render node's refusals as Linux's; the dispatch table names every DRM ioctl's handler from 76-1's stubs. Check: host test calls each through the dispatcher against a fake driver with a card and a render node.
  * 76-4a.2 Master: the first opener of a card becomes master, `SET_MASTER` and `DROP_MASTER` with Linux's rules for root and for the last master, and a notifier the console (76-11) and seatd's revoke path use. Check: host test with two openers.
  * 76-4a.3 Buffer objects and handles: `GEM_CLOSE`, `GEM_FLINK` and `GEM_OPEN` as Linux allows them, dumb buffers `CREATE_DUMB`, `MAP_DUMB` and `DESTROY_DUMB`, mapped write-combined through 76-2a. Check: host test creates, maps, writes and frees a dumb buffer on the fake driver; `tests/drmprobe.c` is written now and first runs on a real driver in 76-6.
  * 76-4a.4 `kcmp` with `KCMP_FILE` and the other kinds as Linux answers them, which Mesa uses to compare descriptors. Check: host test compares a dup, a fork's copy and two opens.
  * 76-4a.5 `drm.4` (nodes, master, auth, dumb buffers), `kcmp.2`, rows in `man/aliases`, `tests/Makefile`, `tests/probe-suites` and `host.host.json`.
* Done: the DRM core host test passes on both ABIs against a fake driver with a card and a render node, including master, auth, dumb buffers and `kcmp`.

### 76-4b PRIME

Buffer export and import through dma-bufs, with import caching, on every DRM driver of 76.

* Files: `rach2/sys/drm/drm_prime.c`; `man/man4/drm.4` (PRIME section); `tests/host/drm_prime_hosttest.c` (new).
* Waits on: 76-3a (`code`: dma-bufs); 76-4a (`code`: handles).
* Labels: none.
* Subtasks:
  * 76-4b.1 `PRIME_HANDLE_TO_FD` and `PRIME_FD_TO_HANDLE`, with the flags Linux accepts. Check: host test exports and imports one buffer.
  * 76-4b.2 Import caching: importing a dma-buf twice into one file gives the same handle, and a self-import gives back the original object. Check: host test, as IGT's `prime_self_import` asks.
  * 76-4b.3 Lifetimes: export after close of the handle, close of the fd while mapped, exit of the exporter. Check: host test counts references to zero.
  * 76-4b.4 Import of a udmabuf and of another device's dumb buffer, where Linux allows it. Check: host test imports a udmabuf; `drmprobe` gains a PRIME row for the checkpoint.
  * 76-4b.5 `drm.4` PRIME section.
* Done: the PRIME host test passes on both ABIs, self-import and udmabuf import among its cases.

### 76-5a Mode objects and atomic commit

Connectors, CRTCs, planes, encoders and properties; atomic check and commit with `TEST_ONLY`, `NONBLOCK` and `ALLOW_MODESET`; the legacy calls over atomic; `ADDFB2` with modifiers and `GETFB2`; the cursor plane; EDID and mode lists. Split from 76-5 so the drivers (76-6, 76-8, 76-12) and the console (76-11) can start while 76-5b adds vblank, events and fences.

* Files: `rach2/sys/drm/drm_mode.c`, `drm_atomic.c`, `drm_legacy.c`, `drm_edid.c`; `man/man7/drm-kms.7` (new); `tests/host/drm_atomic_hosttest.c` (new); `tests/host/drm_edid_hosttest.c` (new).
* Waits on: 76-4a (`code`).
* Labels: none.
* Subtasks:
  * 76-5a.1 Mode objects and properties: `GETRESOURCES`, `GETCONNECTOR`, `GETENCODER`, `GETCRTC`, `GETPLANERESOURCES`, `GETPLANE`, `OBJ_GETPROPERTIES`, `GETPROPERTY`, blobs (`CREATEPROPBLOB`, `DESTROYPROPBLOB`, `GETPROPBLOB`), with the universal-planes and atomic client caps. Check: host test against a fake driver.
  * 76-5a.2 Atomic check and commit in `drm_atomic.c`, with `TEST_ONLY`, `NONBLOCK` and `ALLOW_MODESET` and Linux's errors; `NONBLOCK` completes through a hook 76-5b fills. Check: host test commits, refuses and tests.
  * 76-5a.3 Legacy calls over atomic in `drm_legacy.c`: `SETCRTC`, `PAGE_FLIP`, `CURSOR`, `CURSOR2`, `SETPLANE`, `DIRTYFB`, `OBJ_SETPROPERTY`. Check: host test runs each through the atomic path.
  * 76-5a.4 Framebuffers: `ADDFB`, `ADDFB2` with modifiers and `IN_FORMATS`, `RMFB`, `CLOSEFB`, `GETFB` and `GETFB2`. Check: host test.
  * 76-5a.5 EDID parsing and mode lists in `drm_edid.c`, with the connector's `EDID` blob and preferred mode. Check: `drm_edid_hosttest.c` against QEMU's EDID for std-vga and virtio-gpu, and the Lenovo panel's EDID from the run2 records.
  * 76-5a.6 `drm-kms.7`, rows in `man/aliases` and `host.host.json`.
* Done: both host tests pass on both ABIs against the fake driver; a real driver first runs in 76-6, 76-8 and 76-12.

### 76-5b Vblank, events and fences

Vblank counters and events with monotonic stamps, page-flip and commit events, sequence calls, in and out fences on atomic commits, and the connector force, EDID override and CRC hooks that debugfs (76-18b) exposes.

* Files: `rach2/sys/drm/drm_vblank.c`; `rach2/sys/drm/drm_atomic.c` (fence and event properties, after 76-5a); `man/man7/drm-kms.7` (vblank and fences); `tests/host/drm_vblank_hosttest.c` (new).
* Waits on: 76-5a (`code`); 76-3a (`code`: sync files for `IN_FENCE_FD` and `OUT_FENCE_PTR`).
* Labels: none.
* Subtasks:
  * 76-5b.1 Vblank counters per CRTC, `WAIT_VBLANK`, `CRTC_GET_SEQUENCE` and `CRTC_QUEUE_SEQUENCE`, stamps from `CLOCK_MONOTONIC`, and Linux's off-delay. Check: host test with a timer-driven fake CRTC.
  * 76-5b.2 Events read from the card's descriptor: `FLIP_COMPLETE`, `VBLANK`, `CRTC_SEQUENCE`, with poll. Check: host test reads each event in order.
  * 76-5b.3 `IN_FENCE_FD` on planes and `OUT_FENCE_PTR` on CRTCs, waited before scanout and signalled at the flip. Check: host test with a `sw_sync` fence that signals late.
  * 76-5b.4 Driver hooks for connector force, EDID override and CRC sources, with the state 76-18b reads. Check: host test forces a connector off and back.
  * 76-5b.5 `drm-kms.7` vblank, events and fences.
* Done: the vblank host test passes, and an atomic commit with an in-fence flips only after the fence signals.

### 76-6 fbdrm and std-vga

The firmware's framebuffer as one fixed mode chosen at boot by GRUB's `gfxpayload` (Linux's simpledrm), and std-vga's bochs registers with real modes. Kept as one task: the handoff from fbdrm to bochs is one path, tried as one under OVMF.

* Files: `rach2/sys/drm/fbdrm.c`, `bochs.c`; `man/man4/fbdrm.4` (new); `man/man4/bochs.4` (new); `tests/host/bochs_hosttest.c` (new).
* Waits on: 76-5a (`code`).
* Labels: none.
* Subtasks:
  * 76-6.1 fbdrm: one connector, CRTC and primary plane on the loader's framebuffer, its one mode from the boot information, XRGB8888 and the loader's format, dumb buffers copied or scanned out in place, named `simpledrm` on a platform bus as Linux does. Check: own boot in QEMU under OVMF with bochs's probe off, `drmprobe` sets the mode.
  * 76-6.2 bochs: the DISPI registers through the MMIO BAR, modes from QEMU's EDID (`edid=on`) with a fallback list, pitch and offset for page flips, named `bochs-drm`. Check: host test against a register model.
  * 76-6.3 The handoff: bochs takes the PCI device, removes fbdrm through the aperture contract of 76-1, and leaves the console told. Check: own boot under OVMF, one card left, `bochs-drm`.
  * 76-6.4 A BIOS boot with `gfxpayload=text` gets bochs and no fbdrm, and a UEFI boot on hardware with no driver keeps fbdrm (the Lenovo's case, proved in 76-11b's step). Check: own boot under SeaBIOS.
  * 76-6.5 `fbdrm.4`, `bochs.4` (the fixed mode and how to set it in GRUB's entry), rows in `man/aliases` and `host.host.json`.
* Done: in QEMU, `drmprobe` sets each EDID mode of std-vga through bochs and reads it back; under OVMF with no bochs, fbdrm gives one mode. Debian's `modetest` repeats this at the checkpoint.

### 76-7 virtio core

The virtio 1.x PCI transport: capabilities, queues, reset and MSI-X with a fallback, used by 76-8 and later by other virtio drivers.

* Files: `rach2/sys/virtio/virtio.h`, `virtio_pci.c`, `virtqueue.c`, `Readme.md` (new); `rach2/sys/virtio/sources.mk`; `tests/host/virtqueue_hosttest.c` (new).
* Waits on: 76-1 (`code`: the `sys/virtio` manifest and its include, so that 76-7 edits neither `rach2/sources.mk` nor `guest.mk`).
* Labels: none.
* Subtasks:
  * 76-7.1 PCI discovery of the vendor capabilities (common, notify, ISR, device config) and their BARs, through `pci.h`. Check: own boot with virtio-gpu, the capabilities logged.
  * 76-7.2 Status and feature negotiation, `VIRTIO_F_VERSION_1`, reset with a bounded wait. Check: host test against a device model.
  * 76-7.3 Split virtqueues: descriptor table, available and used rings, indirect descriptors if offered, buffer completion with callbacks. Check: host test fills, wraps and drains a ring.
  * 76-7.4 Interrupts: MSI-X through `pci_msix_enable` with one vector per queue where the table allows, else one shared vector, else the INTx line. Check: own boot with `-device virtio-gpu-pci,vectors=0` takes the INTx path.
  * 76-7.5 `rach2/sys/virtio/Readme.md` and the host test's row.
  * 76-7.6 The transport behind an operations table (configuration access, notify, queue setup, and the memory of queues and of a command's backing), with virtio-pci its first entry, so that 76-8, 77-2 and 77-3 allocate only through it and 83-3b adds its shared-memory transport in a file of its own (80-83 split, Problems item 11). Check: the host test's device model runs through the table.
* Done: the virtqueue host test passes through the transport's table, and an own boot negotiates virtio-gpu's features and logs its queues.

### 76-8 virtio-gpu 2D

Resources, scanout, transfers, cursor, EDID, display-info events and hotplug, and modes.

* Files: `rach2/sys/drm/virtio_gpu.c`, `virtio_gpu.h` (new); `man/man4/virtio-gpu.4` (new); `tests/host/virtio_gpu_hosttest.c` (new).
* Waits on: 76-5a (`code`); 76-7 (`code`); 76-9a (`code`: the uevent a hotplug sends).
* Labels: none.
* Subtasks:
  * 76-8.1 The control and cursor queues, `GET_DISPLAY_INFO`, `GET_EDID`, `RESOURCE_CREATE_2D`, `ATTACH_BACKING`, `SET_SCANOUT`, `TRANSFER_TO_HOST_2D`, `RESOURCE_FLUSH`, with fences on commands. Check: host test against a device model.
  * 76-8.2 KMS: one CRTC and connector per scanout, primary and cursor planes, modes from EDID, dirty rectangles. Check: own boot, `drmprobe` sets the preferred mode.
  * 76-8.3 Hotplug: the config-change interrupt rereads display info and sends a hotplug event (76-9a's uevent). Check: own boot with a GTK or VNC display, a resize of the window changes the connector's modes and a listener hears the uevent.
  * 76-8.4 Identity `virtio_gpu` with a render node and the features Linux's driver lists, under virtio-vga and non-VGA virtio-gpu, and the handoff from fbdrm under virtio-vga. Check: own boot with `virtio-vga` and with `virtio-gpu-pci`.
  * 76-8.5 `virtio-gpu.4`, rows in `man/aliases` and `host.host.json`.
* Done: `drmprobe` sets each EDID mode of virtio-gpu in QEMU and reads it back, and a window resize reaches a listener as a hotplug uevent. Debian's `modetest` repeats the modes at the checkpoint.

### 76-9a Discovery of display devices

The `/sys` entries and uevents that libdrm, Mesa and libudev read for DRM devices, and the empty `/sys/kernel/debug` that debugfs mounts on.

* Files: `rach2/sys/fs/synthfs.c`, `synthfs.h`, `synthfs.Readme.md` (after 76-1); `rach2/sys/fs/procfs.c` (its `/sys` part); `rach2/sys/network/socket/netlink.c` (kernel uevents); `man/man5/sysfs.5`; `man/man7/netlink.7`; `tests/host/procfs_hosttest.c`, `synthfs_hosttest.c` (after 76-1); `tests/sysdrmprobe.c` (new).
* Waits on: 76-1 (`code`: the device registry and identity, and synthfs after 76-1's edit).
* Labels: none.
* Subtasks:
  * 76-9a.1 `/sys/class/drm/cardN`, `renderDN` and `cardN-CONNECTOR` (status, enabled, modes, edid, dpms), with `dev`, `uevent` and the `device` and `subsystem` links. Check: `procfs_hosttest.c` renders them for a registered device.
  * 76-9a.2 `/sys/dev/char/226:N` with its `device`, `subsystem` and `drm` links; the PCI device's `vendor`, `device`, `subsystem_vendor`, `subsystem_device`, `class`, `config` and `boot_vga`; vkms under `/sys/devices/platform/vkms`. Check: `tests/sysdrmprobe.c` walks them as libdrm's `drmGetDevices2` does, in an own boot with whatever driver is on the branch.
  * 76-9a.3 Kernel uevents on `NETLINK_KOBJECT_UEVENT`, which today hears nothing (netlink.c:2807-2808, netlink.7:100-104): add, remove and change with `HOTPLUG=1` for DRM devices, and writable `uevent` files that resend add. Check: a probe hears the add for a device it triggers.
  * 76-9a.4 `/sys/kernel/debug` as an empty directory for 76-18a. Check: `stat` in an own boot.
  * 76-9a.5 `tests/sysdrmprobe.c` with its rows, and `sysfs.5`, `netlink.7`.
  * 76-9a.6 A registration interface by which a driver adds `/sys` attributes, directories and links from its own source, and a DRM device reports a PCI identity: 78-6b's RC6 and frequency files, 78-7's fake device as 8086:0a16, and 82-2's `iommu_group` links, `/sys/kernel/iommu_groups/N/` and `driver_override` use it and edit neither `procfs.c` nor `synthfs.c`. Check: `procfs_hosttest.c` registers an attribute and a link and reads both.
* Done: the host tests render every entry for a registered device, and `sysdrmprobe` finds each card by identity as libdrm does. Debian's libdrm and Mesa's loader confirm it at the checkpoint.

### 76-9b Discovery of input devices

`/sys/class/input` with each device's `uevent` and capabilities, the i8042 serio entries, and 76-19's two `/proc/sys` files, so that 76-19 touches no `procfs.c` or `synthfs.c`.

* Files: `rach2/sys/fs/procfs.c`, `synthfs.c`, `synthfs.h` (after 76-9a); `rach2/sys/network/socket/netlink.c` (input uevents, after 76-9a); `tests/host/procfs_hosttest.c` (after 76-9a); `man/man5/sysfs.5`, `man/man5/proc.5`.
* Waits on: 76-9a (`code`: the same files and the uevent sender); 76-10a (`code`: mice and touchpads on evdev).
* Labels: none.
* Subtasks:
  * 76-9b.1 `/sys/class/input/inputN` and `eventN` with `name`, `phys`, `uniq`, `id/`, `properties`, `capabilities/` (`ev`, `key`, `rel`, `abs`, `msc`, `led`), `uevent` and the `device` link, and `/sys/dev/char/13:N`. Check: `procfs_hosttest.c` renders a keyboard's and a mouse's capability masks as Linux prints them.
  * 76-9b.2 The i8042 serio devices and the USB HID devices as parents, as Linux shows them. Check: own boot, the links resolve.
  * 76-9b.3 Uevents for input devices, added and removed. Check: a listener hears a USB mouse plugged through QMP.
  * 76-9b.4 `/proc/sys/kernel/cap_last_cap` and `/proc/sys/crypto/fips_enabled`, new rows at the end of `sysents`. Check: `cat` gives Linux 6.12's values (40 and 0).
  * 76-9b.5 `sysfs.5` and `proc.5`.
* Done: the host test renders the input entries, and an own boot shows each evdev device under `/sys/class/input` with its parent. Debian's `libinput list-devices` under udevd (76-14a) confirms it at the checkpoint.

### 76-10a Input

Mice through evdev with relative motion and wheels, monotonic times, grab and revoke, and the absolute-axis calls a touchpad needs.

* Files: `rach2/sys/evdev.c`, `evdev.h`, `input.c`, `input.h`, `mouse.c`, `mouse.h`; `rach2/sys/arch/x86/ps2mouse.c`; `rach2/sys/usb/hid.c`; `man/man4/input.4`; `tests/host/evdev_hosttest.c` (new); `tests/evprobe.c` (new).
* Waits on: nothing.
* Labels: none.
* Subtasks:
  * 76-10a.1 `EV_REL`, `EV_ABS`, `EV_SYN` with `SYN_DROPPED`, and the button codes in evdev, which today carries keys only (`evdev.h:36-47`). Check: host test reads a motion event and an overflow.
  * 76-10a.2 The PS/2 mouse and the USB boot-protocol mouse register evdev devices with Linux's names and capabilities (wheel and `REL_WHEEL_HI_RES` where present), while `/dev/mouse` keeps working. Check: own boot, `tests/evprobe.c` reads QMP's mouse events.
  * 76-10a.3 `EVIOCREVOKE`, `EVIOCSCLOCKID` (monotonic, realtime, boottime), `EVIOCGABS`, `EVIOCSABS`, `EVIOCGPROP`, `EVIOCGNAME`, `EVIOCGPHYS`, `EVIOCGUNIQ`, `EVIOCGMASK` and `EVIOCSMASK`, as Linux answers. Check: host test calls each, and a revoked descriptor reads `-ENODEV`.
  * 76-10a.4 Times from the clock the client chose, monotonic by default for libinput. Check: host test compares stamps with `clock_gettime`.
  * 76-10a.5 `tests/evprobe.c` with rows in `tests/Makefile`, `tests/probe-suites` and `host.host.json`; `input.4` (mice, revoke, clocks, and the absolute USB tablet as a limit, Problems item 14).
* Done: in QEMU, `evprobe` reads keys, motion, buttons and wheel from the keyboard and PS/2 mouse, and the evdev host test passes on both ABIs.

### 76-10b Lenovo touchpad

The Lenovo's Synaptics touchpad (PS/2, firmware 8.1) in Linux's absolute mode, against a packet model. Split from its Lenovo step (76-10c), since 76-17b needs the driver's code.

* Files: `rach2/sys/arch/x86/synaptics.c`, `synaptics.h` (new); `rach2/sys/arch/x86/sources.mk` (one row); `rach2/sys/arch/x86/ps2mouse.c` (the detection hook, after 76-10a); `tests/host/synaptics_hosttest.c` (new); `man/man4/input.4`; `rach2/hardware.md` (Input row).
* Waits on: 76-10a (`code`).
* Labels: none.
* Subtasks:
  * 76-10b.1 Detection and setup as Linux's psmouse does it: identify, model, capabilities and extended capabilities queries, absolute mode with W mode, falling back to the plain PS/2 mouse on any failed reply. Check: host test against a packet model of firmware 8.1.
  * 76-10b.2 The evdev device with the capabilities Linux gives this model (positions, pressure, finger width, the multi-finger tools and slots it reports, `INPUT_PROP_POINTER` and, for a clickpad, `INPUT_PROP_BUTTONPAD`). Check: host test compares with the capability bits Debian reports on this Lenovo.
  * 76-10b.3 `input.4` and the `hardware.md` Input row.
* Done: the host tests pass against firmware 8.1's packet model and the capability bits Debian reports for this Lenovo.

### 76-10c Lenovo touchpad on a trial boot

The touchpad driver's first run on the Lenovo, on a kernel trial boot, silent.

* Files: `dev-log/76-display/lenovo-touchpad.md` (new, the record).
* Waits on: 76-10b (`code`).
* Labels: `lenovo`.
* Subtasks:
  * 76-10c.1 Prepare the Lenovo step: the trial kernel from the branch head, `evprobe` built static, the reference capability bits from Debian on p5 (taken by 80-2's session if no record has them), the commands, and the readiness note: every PS/2 exchange waits with the bounded timeout `ps2mouse.c` already uses, a failed query falls back to relative mode, and the handler is in place before the port's interrupt is enabled. The manager reads it and says go, as the `machine` skill asks of new hardware code.
  * 76-10c.2 Lenovo step: `bootctl kernel FILE`, boot, `mixer vol 1`, read the touchpad's capabilities and the setup's log with `evprobe` over SSH, return to the installed kernel. Events from a finger are the sitting's (84-1).
* Done: on the Lenovo's trial boot the touchpad comes up in absolute mode and its evdev capabilities equal Debian's for the same device.

### 76-11a Console on DRM

The console as a DRM client that follows the mode, and the handover to a master and back on drop, close, exit or death. Split from its Lenovo step (76-11b), since 76-14b and 78-9 need the code.

* Files: `rach2/sys/drm/drm_client.c`; `rach2/sys/display/fbcon.c`, `fbdev.c`, `Readme.md`; `rach2/sys/kd.c`, `kd.h`; `man/man4/console.4`; `tests/host/drmcon_hosttest.c` (new).
* Waits on: 76-5a (`code`: atomic commits for the console's own scanout); 76-10a (`code`: evdev revoke); 76-6 (`code`: bochs and fbdrm for its own boots).
* Labels: none.
* Subtasks:
  * 76-11a.1 `drm_client.c`: an in-kernel client that owns a dumb buffer per CRTC and commits it; fbcon draws its cells there and follows mode and hotplug changes. Check: own boot on bochs, the console redraws after a master sets another mode and exits.
  * 76-11a.2 Handover on `SET_MASTER`: the console stops drawing, `/fb` writes are held, the keyboard's mode is saved and set to `K_OFF`; on `DROP_MASTER`, close or exit both are restored and the console redraws. Check: host test with a fake master that exits without dropping.
  * 76-11a.3 The `term` console session and `/fb` keep their behaviour while no master holds the screen. Check: own boot, `term` draws on bochs and on virtio-gpu.
  * 76-11a.4 `console.4` and `rach2/sys/display/Readme.md` (the console as a DRM client, the handover, the old `/fb` limits that remain).
  * 76-11a.5 fbdrm's handover under OVMF with bochs's probe off, the Lenovo's case in QEMU. Check: own boot, `drmprobe` in master mode exits without dropping and the console comes back.
* Done: in QEMU, on bochs, virtio-gpu and fbdrm, a master that exits or dies gives the console back with the keyboard restored.

### 76-11b Console on the Lenovo

fbdrm and the console's handover on the Lenovo's firmware framebuffer, on a kernel trial boot that takes the screen for ten seconds.

* Files: `dev-log/76-display/lenovo-console.md` (new, the record).
* Waits on: 76-11a (`code`).
* Labels: `lenovo`.
* Subtasks:
  * 76-11b.1 Prepare the Lenovo step: trial kernel; `drmprobe` built static in master mode (takes the screen for ten seconds, draws a pattern, exits without dropping); what to read: fbdrm at 1366x768 over GOP, the console back after the exit, keystrokes not echoed while the master held; the readiness note (fbdrm writes only the GOP framebuffer the console already writes, and no register is touched) and the manager's go on the issue; the ten-minute notice, since it takes the screen.
  * 76-11b.2 Lenovo step: trial boot, `mixer vol 1`, `drmprobe` over SSH, the console's state read back over SSH, return to the installed kernel.
* Done: on the Lenovo's trial boot, fbdrm runs at 1366x768 and a master that exits gives the console back with the keyboard restored.

### 76-12 Test display

vkms, a software display with vblank timers and CRCs, so IGT's CRC tests run in QEMU.

* Files: `rach2/sys/drm/vkms.c`; `man/man4/vkms.4` (new); `rach2/cmdline.md` (row `rach2.vkms`); `tests/host/vkms_hosttest.c` (new).
* Waits on: 76-5a (`code`); 76-5b (`code`: the vblank handler its timer calls).
* Labels: none.
* Subtasks:
  * 76-12.1 One CRTC with primary, cursor and overlay planes as Linux's vkms has them, composed in software into an output buffer. Check: host test composes two planes.
  * 76-12.2 Vblank from a kernel timer at the mode's rate, calling 76-5b's handler. Check: host test counts vblanks over a fake clock.
  * 76-12.3 CRCs of the composed output per frame, as the CRC source hook of 76-1 defines. Check: host test, equal frames give equal CRCs.
  * 76-12.4 `rach2.vkms` starts it, on a platform bus, named `vkms`. Check: own boot with the option.
  * 76-12.5 `vkms.4`, the `cmdline.md` row, rows in `man/aliases` and `host.host.json`.
* Done: the vkms host test passes, and with `rach2.vkms` `drmprobe` sets the mode on vkms and flips. IGT's CRC tests run on it at the checkpoint.

### 76-13a Desktop packages

Debian trixie's desktop and test packages pinned by SHA-256, GE-Proton, the WebGL suite's local copy, and the root image grown to fit, brought up first on today's kernel with a headless Weston.

* Files: `vendor/gnu/debian/fetch_debs.sh` (new, generalized from `vendor/gnu/chromium/fetch_chromium.sh`'s index, resolve, fetch, unpack and check steps); `vendor/gnu/debian/*.list` (new: the Mesa, Weston, seat and input, X, browser, sound, font and test sets); `vendor/gnu/proton/fetch_proton.sh` (new); `vendor/gnu/sources-extra.txt`; `vendor/gnu/install_root.sh`; `mk/gnu-extra.mk`; `mk/gnu-image.mk` (the size at lines 75 and 80); `vendor/gnu/Readme.md`; `man/man1/` pages rendered from Debian's (new).
* Waits on: the downloads card (`decision`, Problems item 20).
* Labels: `linux-host`.
* Subtasks:
  * 76-13a.1 The Debian fetcher, from a dated snapshot with the archive keys already in `sources-extra.txt`, every package pinned by SHA-256, reading every list file a layer names, so that each later task adds a list file of its own and at most one call line in `install_root.sh`: 77-7b `debian/venus.list`, 78-14a `debian/va.list`, 79-8b `debian/i386.list`, 80-7a `games/layer.list`, 80-7c `games/native.list`, 80-8b `games/umu.list`, 81-6b `kvm/qemu.list`. Check: a fetch from an empty cache gives the same digests.
  * 76-13a.2 The sets: Mesa 25.0.7-2+deb13u1 (llvmpipe, lavapipe, Crocus, hasvk), Weston 14 with kiosk-shell, seatd, libinput, Debian's `udev` (Problems item 16), Xwayland, Chromium, Firefox ESR, PulseAudio with `module-oss`, ALSA's library with its OSS plugin, fonts; intel-gpu-tools, piglit, glmark2, `libdrm-tests` (modetest), vulkan-tools; and i386 Mesa and modetest for the i686 root (Problems item 24). Check: `install_root.sh` stages them and its SKIP list holds.
  * 76-13a.3 GE-Proton11-12 (566,859,228 bytes): verified once against the release's SHA-512 (`2e374479...bd18e2db31e`), recorded beside its fixed SHA-256 in `sources-extra.txt`, and placed in the root with the libraries it needs. Check: the fetcher refuses a changed byte.
  * 76-13a.4 The WebGL conformance suite's local copy at a pinned commit, with the commit and digest recorded. Check: the fetcher's digest check.
  * 76-13a.5 The root image grown from 4096 MiB (mk/gnu-image.mk:75, :80) to the size the sets need with room, step 8's copy having used 12 GiB. Check: `make gnu-image` builds it.
  * 76-13a.6 Bring-up on today's kernel: headless Weston on Pixman and GL, a browser page, `vulkaninfo` on lavapipe, as step 8 ran them. Check: own boot in QEMU, the three outputs on the issue.
  * 76-13a.7 Pages: Debian's man pages for each new program rendered as the ports' pages are, and `vendor/gnu/Readme.md`. Check: each staged program in the sets has a page under `man/` (Problems item 12).
* Done: the GNU root holds every set with each digest checked, and step 8's headless results repeat on today's kernel.

### 76-13b IGT's tests

IGT's tests built from Debian's 2.0 source, since Debian ships only its tools.

* Files: `vendor/gnu/igt/build_igt.sh` (new); `vendor/gnu/igt/sources.txt` (new); `vendor/gnu/Readme.md` (IGT section, after 76-13a).
* Waits on: 76-13a (`code`: the fetcher and the root); 76-16a (`code`: the Debian 13 builder guest).
* Labels: `linux-host`.
* Subtasks:
  * 76-13b.1 Debian's `igt-gpu-tools` 2.0 source package pinned by digest. Check: fetch and digest.
  * 76-13b.2 The build in the Debian 13 builder guest with Debian's build flags, the tests and their libraries installed under `/usr/libexec/igt-gpu-tools`. Check: the build log ends with the test list.
  * 76-13b.3 Staging into the root through `install_root.sh`, with IGT's test list recorded beside it. Check: `igt_runner --list` in an own boot.
  * 76-13b.4 The Readme section and `man/` pages for `igt_runner` and `igt_resume`.
* Done: the root holds IGT 2.0's tests built from Debian's source, and the list matches the builder guest's.

### 76-14a Session pieces

The session's parts that need no display: an ordinary user, seatd, udevd, Weston's configuration, the init service, and both sound routes, each proven by a tone recorded through the audio loopback.

* Files: `vendor/gnu/desktop/` (new: `weston.ini`, `default.pa`, `asound.conf`, `rach2-desktop`, the start script); `vendor/gnu/install_root.sh` (the user and groups, after 76-13a); `utils/init.c` (the service); `man/man7/desktop.7` (new); `man/man8/init.8`; `tests/cases/machine.desktop.json`, `tests/cases/machine/desktop_scenario.c` (new).
* Waits on: 76-13a (`code`).
* Labels: `linux-host`.
* Subtasks:
  * 76-14a.1 The desktop user, the `seat`, `video`, `render`, `input` and `audio` groups, and seatd as root with `SEATD_VTBOUND=0`, since Rach2 has no VT ioctls (kd.c; comparison.md:34, :57). Check: own boot, `seatd` serves a client.
  * 76-14a.2 Debian's `systemd-udevd` started by init with Debian's rules, so libinput reads `ID_INPUT_*` (fable.md:354-363). Check: `udevadm info` on an input node in an own boot.
  * 76-14a.3 The init service in `utils/init.c`: udevd, seatd, then Weston as the desktop user with kiosk-shell, headless when no card has a connected connector. Check: own boot starts headless Weston.
  * 76-14a.4 Sound: PulseAudio on `/dev/dsp` through `module-oss`, and ALSA's library over its OSS plugin. Check: with `rach2.audio.loopback`, a tone from Chromium and from Firefox through PulseAudio, and one from `aplay`, each recorded and matched.
  * 76-14a.5 `desktop.7` (user, seat, sound, configuration), `init.8`, and the `desktop` family's first case: headless Weston and the three tones.
  * 76-14a.6 If `systemd-udevd` does not run under Rach2's init, found first in the builder guest's root and then in an own boot, Debian's libudev-zero in its place, with `ID_INPUT_*` given by its own rules, as plan.md's problem 16 names. Check: libinput lists the keyboard and the mouse with their `ID_INPUT_*` properties in an own boot.
* Done: an own boot starts headless Weston as the desktop user at boot, and each browser's tone and ALSA's are recorded through the loopback.

### 76-14b Desktop on the display

Weston on DRM at boot: Pixman, then GL on a device with a render node; resolution chosen by configuration and a restart; the console back when Weston dies; the i686 display checks.

* Files: `vendor/gnu/desktop/` (after 76-14a); `man/man7/desktop.7`; `rach2/hardware.md` (Display section, now false at 67-73); `tests/cases/machine/desktop_scenario.c` (after 76-14a).
* Waits on: 76-6, 76-8, 76-11a (`code`: the displays and the console); 76-9a, 76-9b, 76-10a (`code`: discovery and input); 76-4b, 76-5b (`code`: PRIME, events and fences, which Weston's DRM backend and GL use); 76-14a (`code`).
* Labels: `linux-host`.
* Subtasks:
  * 76-14b.1 Weston's DRM backend on Pixman on bochs, virtio-gpu and fbdrm, through seatd. Check: own boot on each display profile, Weston's log names the output and mode.
  * 76-14b.2 GL on virtio-gpu's render node through Mesa's EGL; on bochs, a card-only device, only programs that open the card themselves get GL, as on Linux. Check: `weston-simple-egl` on virtio-gpu in an own boot.
  * 76-14b.3 Resolution: `weston.ini`'s output mode and a restart of Weston, documented. Check: two modes set in turn on virtio-gpu.
  * 76-14b.4 Kill Weston with SIGKILL: the console comes back with the keyboard. Check: own boot, the console's picture checksum after the kill equals the one before Weston started.
  * 76-14b.5 A browser smoke page in Chromium and Firefox under Weston on DRM, with keys and a click injected through QMP. Check: the page records the input.
  * 76-14b.6 i686: bochs `modetest`, `evprobe` and an llvmpipe GL program with a pixel checksum. Check: own i686 boot.
  * 76-14b.7 `desktop.7`, the `hardware.md` Display section, and the `desktop` suite's display cases.
* Done: in QEMU, Weston starts at boot on bochs and on virtio-gpu, a browser draws and takes input, a kill of Weston gives the console back, and the i686 checks pass.

### 76-15a Forced ext2 test

The forced ext2 test, first in 76: matched Rach2 and Debian copies stopped under writes, then checked against an orderly shutdown. 76's checkpoint waits on it, and a fault it finds holds the release.

* Files: `tests/cases/machine.ext2stop.json`, `tests/cases/machine/ext2stop_scenario.c` (new); `tests/system/suites.mk`, `suites.md`, `tests/registry` (rows); `rach2/sys/fs/ext2.Readme.md` (Tests).
* Waits on: nothing. It uses the Debian 13 guest the second run left on the testing PC; 76-16a later formalizes the same guest.
* Labels: none.
* Subtasks:
  * 76-15a.1 Matched images made with the same `mke2fs` options; the mount mode recorded; sentinel files synced; files created, held open and unlinked with their inode numbers recorded; writes under way; QEMU killed; the stopped images kept. Check: the author runs the case alone once.
  * 76-15a.2 The same steps under Debian 13 on a copy of the same image, for Linux's state after the same stop.
  * 76-15a.3 After the stop: Rach2's next writable mount and its own check first, then `e2fsck -fn`, then the sentinels, each against an orderly-shutdown control. Step 8's root lost this order, since `e2fsck -fy` ran before Rach2's check saw it.
  * 76-15a.4 The verdict: only the documented state of an unclean stop (ext2.Readme.md:68, :111: unlinked inodes with zero dtime and their bitmap bits) passes; any other finding is a fault, fixed in this task as a subtask, and it holds the release.
  * 76-15a.5 `ext2.Readme.md` Tests and the suite rows.
* Done: the case leaves only the documented state on Rach2's copy, matching Debian's copy, or the fault it found is fixed and the case passes.

### 76-15b Data partition and trial root, in QEMU

The data partition that keeps games, VM images and caches across updates, and a trial root slot from which an unreleased root boots as an image file through the loop driver, rehearsed in QEMU.

* Files: `utils/rach2-install`; `utils/bootctl.c`; `utils/diskctl.c` (a partition added to an existing table, if it lacks it); `rach2/kernel.c` (root from an image file, after 76-1); `rach2/cmdline.md` (row); `man/man8/rach2-install.8`, `bootctl.8`, `diskctl.8`; `man/man7/disk.7`; `tests/cases/machine.datapart.json`, `tests/cases/machine/datapart_scenario.c` (new).
* Waits on: 76-1 (`code`: 76-1 edits `kernel.c` first).
* Labels: none.
* Subtasks:
  * 76-15b.1 Layout: a new install writes a data partition, GPT entry 6, ext2, labelled `Rach2 data`, and `--update` never writes it and keeps its entry, as it keeps the other partitions. Check: own QEMU install, a file on the data partition, an update, the file read back.
  * 76-15b.2 Root from an image file: `rach2.rootimage=SPEC:/PATH` mounts the partition `SPEC` names, attaches the file to a loop device (`rach2/sys/block/loop.c`) and mounts it as the root; today no root option names a file (cmdline.md:10-16). Check: own boot from an image on the data partition.
  * 76-15b.3 The trial root slot: `bootctl` arms one boot of the trial root image and falls back to the installed root if it does not come up, as a kernel trial boot does. Check: own boot of a broken image falls back.
  * 76-15b.4 Making room on a disk with none: recovery shrinks the root partition offline with `resize2fs`, rewrites its end in the table and adds entry 6, then `--update` grows the root into the smaller partition. Check: run in QEMU on a disk image with the Lenovo's exact table (lenovo-setup.md:9-14).
  * 76-15b.5 `rach2-install.8`, `bootctl.8`, `diskctl.8`, `disk.7`, the `cmdline.md` row, and the `datapart` case.
* Done: in QEMU, a file on the data partition survives an update, a trial root boots from it and falls back when broken, and a disk laid out as the Lenovo's gains the partition.

### 76-15c Lenovo storage

The data partition made on the Lenovo, a trial root booted from it, and a file kept across an update.

* Files: `dev-log/76-display/lenovo-storage.md` (new, the record).
* Waits on: 76-15b (`code`). The `machine` skill's trial-root rule is on master before the step (plan.md, After your approval, item 4); the size is decision F's recommendation unless the owner chose another.
* Labels: `lenovo`.
* Subtasks:
  * 76-15c.1 Prepare: the exact commands for the owner's chosen size, rehearsed in QEMU on 76-15b.4's disk image of the same table with the second run's Debian root on its p5; the trial kernel and an os-real trial root image from the branch head with their SHA-256; the file to keep and how to read it. Why it cannot hang: no new hardware code runs. Debian on p5 does the disk work in one session of its own, since Rach2's installed kernel refuses raw disk writes (`rach2/sys/fs/devfs.c:631-636`) and the released recovery stages `resize2fs` and `diskctl` but neither `sfdisk` nor `mke2fs` (`krffs/manifests/rach2-recovery.manifest:54`, `:70`); p3 is not mounted there (its `/etc/fstab` line is `noauto`, run2/lenovo-setup.md). In that session `e2fsck -f` and `resize2fs` shrink p3's file system, `sfdisk` rewrites p3's end from a dump that keeps the disk's and every partition's GUIDs and adds entry 6, `Rach2 data`, and `mke2fs -t ext2` makes its file system with the features Rach2's ext2 driver mounts; the rehearsal proves the installed root, recovery and a trial boot still start afterwards, and that Rach2 mounts the new partition.
  * 76-15c.2 Lenovo step: boot Debian on p5 through the one-shot entry; shrink p3's file system, rewrite the table and make the data partition as rehearsed; return to the installed root, boot it, and mount the data partition.
  * 76-15c.3 Lenovo step: write the kept file, run `rach2-install --update` with the released image, read the file back.
  * 76-15c.4 Lenovo step: put the trial root on the data partition, trial-boot it with the trial kernel, read its version over SSH, return to the installed root; the record in `lenovo-storage.md`.
* Done: the Lenovo has a data partition, a file on it survives an update, and a trial root booted from it once and the installed root came back.

### 76-16a Test profiles

The QEMU profiles, accelerators and reference guest every graphics run uses.

* Files: `tools/etude/qemu_profile.c`; `tools/etude/Readme.md`; `rach2/qemu_devices.sh`; `tools/setup/doctor.sh` (the render-node check, from 77-1); `tools/Readme.md` (the testing PC's section); `tests/graphics/debian-ref/` (new: the pinned Debian 13 cloud image, the seed with an empty `network-config`, the builder and reference guest setup); `tests/graphics/Readme.md` (new).
* Waits on: the downloads card (`decision`, for the cloud image and Debian's QEMU packages, unless the second run's copies match their digests).
* Labels: none.
* Subtasks:
  * 76-16a.1 Display profiles: std-vga with `edid=on`, virtio-vga under SeaBIOS (Debian's GRUB resets forever with no VGA), non-VGA virtio-gpu, and none for vkms; the window fixed at 1280x800, since QEMU's EDID offers up to 5120x2160; a PS/2 mouse and no USB tablet. Check: own boot of each lists its device.
  * 76-16a.2 Accelerators: TCG for Rach2's own suites through the site profile, since etude takes KVM wherever `/dev/kvm` opens (tools/etude/Readme.md:68, :422); KVM with `-cpu host` for graphics runs where it opens, with `+invtsc` tried for the vDSO clock and the result recorded. Check: one own boot under each.
  * 76-16a.3 The Debian 13 reference and builder guest, in the same QEMU and profiles as Rach2's runs. Check: it boots, answers SSH, and lists the same display devices.
  * 76-16a.4 Rach2's GRUB with non-VGA virtio-gpu, tried once before that profile is used. Check: the outcome on the issue.
  * 76-16a.5 The guest environment of each run: `LP_NUM_THREADS=0` for the conformance suites, vkd3d-proton's suite at its measured setting, the guest's CPU count for performance runs on both sides of a comparison, Xvfb with `-noreset`, and Mesa's shader cache emptied at the start of each Done run. Check: a guest prints its environment.
  * 76-16a.6 `tools/etude/Readme.md` and `tests/graphics/Readme.md`.
  * 76-16a.7 The virgl profile on the testing PC: virgl on the RTX 4080 through QEMU's GL on Mesa's d3d12, which needs no module, for 77-2, 77-3, 79-6c and 80-3. Check: a Debian guest's `eglinfo` under it names virgl.
  * 76-16a.8 A check in `tools/setup/doctor.sh`, from 77-1, that runs `eglinfo -B -p surfaceless` with the virgl profile's environment and requires the renderer "D3D12 (NVIDIA GeForce RTX 4080 SUPER)", never llvmpipe; `-p gbm` would open `card0`, which belongs to the `video` group. Check: doctor prints the renderer for the running account.
  * 76-16a.9 The check run under each test account on the testing PC, the answers on the issue, and the check and the `render` group stated in `tools/Readme.md`'s testing PC section; the manager puts an account outside the group into it, as setup. Check: the answers on the issue.
* Done: every profile boots Rach2 and the Debian guest alike, the profile records accelerator, CPUs, display and environment in each run's record, the virgl profile names virgl in a Debian guest, and doctor names the GPU renderer for every test account on the testing PC.

### 76-16b Venus host on the testing PC

Decision A's udmabuf module for WSL's kernel, its build and load as recorded commands, the Venus profile, and the Windows QEMU checks, for 77's Venus consumers.

* Before it is set Ready: the manager, as root in WSL, builds the module for the running kernel from the second run's patch (`run2/step6/udmabuf-module.diff`), loads it as decision A's recommendation says (after each start of WSL, with the compiler and CRC checks), and puts every test account in `render`, recording each command and its output on the issue. This is the manager's setup, done at once after the owner's approval, and no step of the task.
* Files: `tools/setup/wsl-udmabuf/` (new: the build recipe with its compiler check, the three-hunk patch, the CRC check of the installed modules, the load script); `tools/etude/qemu_profile.c` (the Venus profile, after 76-16a); `tests/graphics/Readme.md` (after 76-16a); `tools/Readme.md` (the module's paragraph in the testing PC's section, after 76-16a).
* Waits on: 76-16a (`code`: `qemu_profile.c`); the downloads card (`decision`: Microsoft's WSL kernel source, 256 MB for 6.18.54.1).
* Labels: none. Any loop takes it; nothing in it needs root.
* Subtasks:
  * 76-16b.1 The recipe committed as the manager ran it: the build refused when the compiler or the kernel's version differs, the CRC check before the load, the load script. Check: the recipe's steps match the manager's record on the issue.
  * 76-16b.2 The Venus profile: Venus on screen through QEMU's GL on llvmpipe over vgem. Check: a Debian guest's `vulkaninfo` under it gives Vulkan 1.3 with a swapchain.
  * 76-16b.3 Windows: the stock QEMU 11.1.0 and the engine's 11.1.2 told apart, and the stock one started once with `venus=on,blob=on,hostmem=1G` and the guest's capsets read. Check: the capsets on the issue.
  * 76-16b.4 The Readme's Venus section and the module's paragraph in `tools/Readme.md`.
* Done: a Debian guest under the Venus profile gets Vulkan 1.3 with a swapchain, as measured in run 2, and the module's build and load are one documented command each.

### 76-17a Browser acceptance in QEMU

Both browsers on llvmpipe under Weston in QEMU against the Debian reference guest: sandboxes, pages, video, the WebGL suite, WebGPU on SwiftShader, sound in several tabs, and a GPU process restart.

* Files: `tests/graphics/browser/` (new: pages served over local HTTPS, the acceptance script, the recorded switches); `tests/cases/machine.browser.json`, `tests/cases/machine/browser_scenario.c` (new), with the `browser` row in `tests/registry`; `tests/graphics/Readme.md` (after 76-16a and 76-22b).
* Waits on: 76-14a (`code`: the session and sound); 76-22b (`code`: the browser runners); 76-16a (`code`: the reference guest).
* Labels: `linux-host`.
* Subtasks:
  * 76-17a.1 Each run records its switches: `--ignore-gpu-blocklist` for WebGL and `--enable-unsafe-webgpu` for WebGPU on a software renderer, each lifting only its own block. Check: the run record lists them.
  * 76-17a.2 Sandboxes read from each browser: Chromium's processes from its own diagnostics against Debian's on the same binaries (renderers and storage at seccomp 2 in their own namespaces, the network service at seccomp 0, the GPU process unsandboxed unless decision D takes 76-20), and Firefox's `about:support`. Check: the two tables side by side on the issue.
  * 76-17a.3 Local pages over HTTPS: forms, storage, downloads and uploads, clipboard and fonts. Check: the page reports each result.
  * 76-17a.4 Video: H.264, VP9 and AV1 at 720p and 1080p in both browsers, drops counted and each decoder named from the browser's log. Check: one line of numbers per clip, Rach2 beside Debian.
  * 76-17a.5 One fixed chunk of the WebGL suite in each browser through 76-22b's runner, failures compared by case with Debian's stored whole run. The whole suite is the release's `webgl` family, where the Chromium GPU-process crash of steps 3 and 4 is named as a flake. Check: the chunk's comparison on the issue.
  * 76-17a.6 WebGPU on SwiftShader: compute and pixels read back, the adapter named, a subset of the WebGPU conformance suite and the WebGPU samples. Check: the readback matches.
  * 76-17a.7 Several tabs with sound, recorded through the loopback, and a GPU process killed and restarted with the page still drawing.
* Done: in QEMU each browser's chunk, pages and clips equal Debian's reference guest by case, drops are no more than Debian's, and the sandboxes match. The slice runs the same small forms; the release runs the `webgl` and `video` families whole.

### 76-17b Lenovo desktop and video

The desktop at boot on the Lenovo's firmware framebuffer and the video acceptance there against Debian on the same machine.

* Files: `dev-log/76-display/lenovo-desktop.md` (new, the record).
* Waits on: 76-17a (`code`: the acceptance script); 76-14b (`code`); 76-10b (`code`); 76-10c, 76-11b (`evidence`: the touchpad and fbdrm booted on the Lenovo); 76-15c (`evidence`: the data partition and trial root on the Lenovo); 80-2 (`evidence`: Debian's drop counts on the Lenovo with Firefox's VA-API off).
* Labels: `lenovo`, `linux-host`. The hand-back removes `linux-host` (Problems item 19).
* Subtasks:
  * 76-17b.1 Build the trial root with the desktop and the trial kernel from the branch head on the testing host; record both digests and the path the Mac fetches them from.
  * 76-17b.2 Prepare the step: copy the root to the data partition, trial boot, wait for Weston on fbdrm at 1366x768 in its log, run the video clips of 76-17a.4 in both browsers at `mixer vol 1`, kill Weston and read the console's state; the ten-minute notice, since sound plays and the screen is taken. The readiness note: the only new kernel code that runs on hardware is fbdrm, which 76-11b booted, and the touchpad driver, which 76-10c booted; the manager's go on the issue.
  * 76-17b.3 Hand back as `owner-only`, removing `linux-host`.
  * 76-17b.4 Lenovo step: run it, compare the drops with 80-2's Debian numbers, write the record, return to the installed root.
* Done: on the Lenovo's trial root Weston starts at boot, video drops no more frames than Debian's browsers on the same path, and killing Weston brings the console back.

### 76-18a debugfs and sw_sync

The debug filesystem at `/sys/kernel/debug`, with `sync/sw_sync` and `sync/info`, which IGT's sync-file tests drive. Split from 76-18 so that 76-3b's producer cases need not wait for KMS.

* Files: `rach2/sys/fs/debugfs.c`; `rach2/sys/fs/debugfs.Readme.md` (new); `tests/host/debugfs_hosttest.c` (new); `rach2/sys/fs/vfs.c` (the type, beside `vfs_sysfs_type` at :3205, and its mount); `man/man5/debugfs.5` (new).
* Waits on: 76-9a (`code`: the mount point); 76-3a (`code`: the `sw_sync` timeline).
* Labels: none.
* Subtasks:
  * 76-18a.1 A small filesystem of kernel-defined files and directories, root-only as Linux mounts it. Check: own boot mounts it and lists it, as root only.
  * 76-18a.2 `sync/sw_sync` (open makes a timeline, `SW_SYNC_IOC_CREATE_FENCE` and `SW_SYNC_IOC_INC`) and `sync/info`. Check: `tests/host/debugfs_hosttest.c` (new) makes, signals and reads a fence through the file.
  * 76-18a.3 `debugfs.Readme.md` and `debugfs.5`.
* Done: the debugfs host test passes on both ABIs. IGT's `sw_sync` runs at the checkpoint.

### 76-18b DRM debugfs

Each device's, CRTC's and connector's debugfs files: the CRC control and data files, connector forcing and EDID override.

* Files: `rach2/sys/fs/debugfs.c` (after 76-18a); `rach2/sys/drm/drm_debugfs.c` (new; a row in `rach2/sys/drm/sources.mk`, under the row rule); `man/man5/debugfs.5`.
* Waits on: 76-18a (`code`); 76-5b (`code`: the hooks).
* Labels: none.
* Subtasks:
  * 76-18b.1 `dri/N/` per device by Linux's index, with `name`, `clients`, `state` and `framebuffer`. Check: own boot lists `/sys/kernel/debug/dri/0` on bochs.
  * 76-18b.2 `crtc-N/crc/control` and `crtc-N/crc/data`, read as Linux's text format. Check: own boot on vkms reads CRC lines.
  * 76-18b.3 Connector `force` and `edid_override`. Check: own boot forces a connector off and loads an EDID.
  * 76-18b.4 `debugfs.5` DRM files.
* Done: every debugfs file that IGT 2.0's CRC and connector-forcing tests open, listed from IGT's source, exists on vkms, bochs and virtio-gpu in an own boot.

### 76-19 Linux's limits

What step 8 found Rach2 refusing where Linux answers: `rseq`, `SCHED_BATCH` and `SCHED_IDLE` for any user, `MADV_GUARD_INSTALL`, `get_mempolicy` and `process_vm_readv`. The two `/proc/sys` files moved to 76-9b.

* Files: `rach2/sys/proc/rseq.c`, `process_vm.c`; `rach2/sys/mm/mempolicy.c`; `rach2/sys/syscall_proc.c` (madvise at 6765-6797, `sched_set` at 7250, after 76-2b); `rach2/sys/mm/vm.c` (guard regions, after 76-2b); `rach2/sys/arch/x86/isr.c` (the rseq hook on the return to user mode); the two `systab.c` and `sysnr.h` (after 76-4a); `man/man2/rseq.2`, `process_vm_readv.2`, `get_mempolicy.2` (new); `man/man2/sched_setscheduler.2`, `brk.2`; `tests/limitsprobe.c` (new).
* Waits on: 76-2b (`code`: the order of edits to `syscall_proc.c` and `rach2/sys/mm/`); 76-4a (`code`: the order of edits to the syscall tables).
* Labels: none.
* Subtasks:
  * 76-19.1 `rseq`: registration and unregistration with Linux's checks, `cpu_id` and `cpu_id_start` kept current on the return to user mode, and a critical section aborted on preemption and signal delivery. Check: `limitsprobe` registers as glibc 2.44 does and reads its CPU after a migration.
  * 76-19.2 `SCHED_BATCH` and `SCHED_IDLE` for any user, which `sched_set` refuses today (syscall_proc.c:7247-7248). Check: probe as an ordinary user.
  * 76-19.3 `MADV_GUARD_INSTALL` and `MADV_GUARD_REMOVE`: an access to a guard page raises `SIGSEGV`. Check: probe touches a guard page in a child.
  * 76-19.4 `get_mempolicy` on a one-node machine, as Linux answers it. Check: probe reads `MPOL_DEFAULT` and the node mask.
  * 76-19.5 `process_vm_readv` and `process_vm_writev` with ptrace's permission rules. Check: probe reads a child's memory and is refused another user's.
  * 76-19.6 Pages, rows in `man/aliases`, `tests/Makefile` and `tests/probe-suites`, and `rach2/syscalls.md` regenerated.
* Done: `limitsprobe` gives the answers of its expected table, taken from Linux 6.12's code and checked against the Debian 13 guest at the checkpoint.

### 76-20 Chromium's GPU sandbox on llvmpipe

Only if the owner takes decision D: the early sandbox's denied load of `libEGL.so.1` and the filter's traps on Mesa's `sched_setscheduler` and `sched_setaffinity`, fixed in Chromium's GPU policy or in Mesa and carried as a rebuilt package.

* Files: `vendor/gnu/rebuilt/` (new: the patch, the rebuild recipe in the Debian builder guest, the pinned output); `vendor/gnu/Readme.md` (its own section).
* Waits on: 76-13a, 76-13b (`code`: the root, with the Weston and Xorg 76-20.1 runs beside Chromium); 76-16a (`code`: the builder guest). Filed only if the owner takes it in decision D; the totals are given both ways.
* Labels: `linux-host`.
* Subtasks:
  * 76-20.1 Trace both failures in the Debian guest on the same binaries. Check: the trace names each denied path and call.
  * 76-20.2 The fix in Chromium's policy or Mesa's threads, whichever the trace shows is smaller, as a patch. Check: the Debian guest's GPU process runs sandboxed with WebGL working.
  * 76-20.3 The rebuilt package beside the pinned ones, digest recorded. Check: the fetcher stages it in place of Debian's.
  * 76-20.4 On Rach2: the GPU process sandboxed with WebGL and WebGPU working on llvmpipe. Check: own boot, Chromium's sandbox page.
  * 76-20.5 The Readme's section on the rebuilt package.
* Done: Chromium's GPU process on Rach2 reports itself sandboxed with WebGL and WebGPU working on llvmpipe.

### 76-21 Direct3D on lavapipe

The pinned GE-Proton's DXVK and vkd3d-proton on Rach2's lavapipe in QEMU, in a new prefix, against a Linux guest in the same QEMU.

* Files: `tests/graphics/d3d/` (new: Direct3D 9, 11 and 12 programs, built with MinGW in the Debian guest; the build recipes for Wine 11.0's Direct3D tests and vkd3d-proton's suite; the runner and its expected-results files); `tests/cases/machine.d3d.json` (the `d3d` family: the three programs), `tests/cases/machine.d3d-wine.json` and `tests/cases/machine.d3d-vkd3d.json` (the two suites' families), `tests/cases/machine/d3d_scenario.c` (new); their three rows in `tests/registry`.
* Waits on: 76-13a (`code`: GE-Proton in the root); 76-16a (`code`: the Debian guest for the builds and the reference); the downloads card (`decision`: Wine's and vkd3d-proton's test sources).
* Labels: `linux-host`.
* Subtasks:
  * 76-21.1 A Direct3D 9, 11 and 12 program, each reading its pixels back. Check: all three draw right in the Debian guest.
  * 76-21.2 The prefix and the runs on Rach2 through DXVK and vkd3d-proton on lavapipe, with no virgl or Venus. Check: the three programs draw right in an own boot.
  * 76-21.3 Wine's Direct3D tests built and registered as the `d3d-wine` family, one unit run on each path by its author; the release runs the family whole, each unit matched to Linux's recorded exit: on Proton's path five units exit with 5 and `d3d9:visual` with 245 (`run2/step3-4/out/step3/8/wine-1/summary.txt`); on WineD3D, dxgi's stall at `dxgi.c:8040` is judged by its progress, never by a clock. Check: the comparison file.
  * 76-21.4 vkd3d-proton's suite built and registered as the `d3d-vkd3d` family, one test run by its author; the release runs the family whole in a 16 GiB guest or at 4 jobs, its one long test as Linux's, lavapipe's segfaults named as Linux's flakes, refusals above feature level 11_1 and shader model 6.0 recorded. Check: the comparison file.
  * 76-21.5 The `d3d` suite's rows and `tests/graphics/Readme.md`'s section.
* Done: the 64-bit programs draw right, and both suites are registered with Linux's results stored, one unit of each matched by its author; the release matches them whole, unit by unit.

### 76-22a Admission and capture

etude's CPU admission and its Windows admission fault, and capture inside the guest with pixel checksums.

* Files: `tools/etude/budget.c`, `authority.c`, the ledger code and `tools/etude/Readme.md`; `tests/graphics/capture/` (new: a libdrm program that reads the scanout through `GETFB2` and a mapping, and checksums it).
* Waits on: 76-16a (`code`: the profiles name the CPU counts).
* Labels: none.
* Subtasks:
  * 76-22a.1 Admission of CPU-bound suites by `cpu_millis` claims, up to 32 guest CPUs at once (4 guests of 8), a performance guest counted as CPU-bound and a light one as 2 CPUs, since etude now claims memory alone (tools/etude/Readme.md:238). Check: a host test of the admission arithmetic, run alone.
  * 76-22a.2 The Windows fault, `etude admit: start: /usr/bin/env: No such file or directory` outside a launcher claim, fixed on the shared branch (Problems item 10). Check: one admission on Windows.
  * 76-22a.3 The new admission deployed as its own etude version beside master's, so the release machinery and other work keep theirs until 76 releases. Check: `etude provision version` lists both.
  * 76-22a.4 In-guest capture, since QMP's screendump has no surface once the scanout is a dma-buf: written and tried against Linux in the Debian guest first, then used on Rach2. Check: the same frame gives the same checksum in both.
  * 76-22a.5 Readme sections.
* Done: four CPU-bound guests of 8 CPUs run at once under the ledger and a fifth waits, and a capture's checksum is the same in Rach2 and Debian.

### 76-22b Browser runners

Runners for the WebGL suite and video playback, each writing one line of numbers and recording the renderer, the IOMMU state, the cache's preparation, the case list and the concurrent work, each run first in the Debian reference guest.

* Files: `tests/graphics/runners/webgl.sh`, `video.sh`, `common.sh` (new); `tests/graphics/Readme.md` (after 76-16a).
* Waits on: 76-16a (`code`); 76-13a (`code`: the browsers and the WebGL copy).
* Labels: `linux-host`.
* Subtasks:
  * 76-22b.1 The common record: renderer, IOMMU state, cache preparation, case list, concurrent work, the switches; fixed size, warm-up and repetitions, since browser fill numbers in QEMU varied by about 50% between runs on the first night.
  * 76-22b.2 The WebGL runner: the whole suite in chunks of a few hundred pages, each in a new browser, a failed page run once more; the discovery harness's silence timeout is no verdict. Check: one chunk in the Debian guest.
  * 76-22b.3 The video runner: clip, codec, resolution, decoder by name, frames shown and dropped. Check: one clip in the Debian guest.
  * 76-22b.4 The Debian reference runs whole, stored with their records for 76's Done.
  * 76-22b.5 The Readme's runner section.
  * 76-22b.6 The `webgl` and `video` families: `tests/cases/machine.webgl.json` and `machine.video.json` (new), each case a chunk or a clip with its stored Debian result, each with its line in `tests/registry` and block in `tests/system/suites.mk`, naming tools, architectures, memory and per-case `requirements.hosts` (WSL alone for virgl and Venus cases). Check: `tests/run` lists each family's cases.
* Done: each runner gives one line per case and a record, and the Debian references are stored.

### 76-22c Display runners and fuzzers

Runners for IGT, modetest, piglit and glmark2 with their Debian references, and an ioctl fuzzer run as an ordinary user against every new device node.

* Files: `tests/graphics/runners/igt.sh`, `modetest.sh`, `piglit.sh`, `glmark2.sh` (new); `tests/graphics/igt-76.list` (new, the subtest list); `tests/fuzz/drm_ioctl_fuzz.c` (new, libFuzzer over the DRM, dma-buf, sync file and evdev ioctl decoders); `tests/drmfuzz.c` (new, the guest fuzzer); `tests/fuzz/Readme.md`.
* Waits on: 76-13b (`code`: IGT); 76-22b (`code`: the common record); 76-4b, 76-5b, 76-3b, 76-10a (`code`: the nodes the fuzzers drive).
* Labels: `linux-host`.
* Subtasks:
  * 76-22c.1 The IGT runner over a named list, by `igt_runner`, with each skip compared to Linux's. Check: one test in the Debian guest on vkms.
  * 76-22c.2 The modetest runner: every EDID mode set, read back and checksummed through 76-22a's capture. Check: std-vga in the Debian guest.
  * 76-22c.3 The piglit and glmark2 runners with the environment of 76-16a.5. Check: one case each in the Debian guest.
  * 76-22c.4 Debian references for IGT on vkms, bochs-drm and virtio-gpu, and for modetest on std-vga and virtio-gpu, stored.
  * 76-22c.5 The libFuzzer harness over the ioctl decoders, built as the other harnesses in `tests/fuzz`. Check: the author runs it alone for a fixed count.
  * 76-22c.6 `tests/drmfuzz.c`: random ioctls as an ordinary user on every node of 76 for a fixed count, with the kernel's own fault counters read after. Check: the author runs it alone in QEMU.
  * 76-22c.7 The `igt`, `modetest`, `piglit` and `glmark2` families: `tests/cases/machine.igt.json`, `machine.modetest.json`, `machine.piglit.json` and `machine.glmark2.json` (new), each case a named test or list part with its stored Debian result, each with its line in `tests/registry` and block in `tests/system/suites.mk`, naming tools, architectures, memory and per-case `requirements.hosts`, so that the slices name cases such as `igt:kms_atomic@vkms` and the release runs the families whole. Check: `tests/run` lists each family's cases.
* Done: each runner gives Linux's result in the Debian guest, and the fuzzers end their counts with no kernel fault.

### Checkpoint slice for 76

The slice runs once every task of 76 is on the branch, after Codex's review of what reached the branch since the last review and the one batch of fixes. It runs on the testing PC in QEMU on WSL under KVM, in parallel guests within the admission of 76-22a, each case judged by counts against the stored Debian reference of the same profile, never by the clock. 76 touched the Lenovo's hardware (fbdrm on its framebuffer, the touchpad, its disk), so one Lenovo trial boot follows. The manager writes the slice's case list at the checkpoint from these rows, each a named case of a registered family: `igt`, `modetest`, `piglit` and `glmark2` (76-22c.7), `webgl` and `video` (76-22b.6), `desktop` (76-14a), `d3d-wine` and `d3d-vkd3d` (76-21), and the probes' existing cases.

| Case | Runner | Profile | What it proves | Measured or expected length |
|---|---|---|---|---|
| igt-vkms | IGT: `core_getversion`, `core_auth`, `core_setmaster`, `kms_addfb_basic`, `kms_atomic`, `kms_prop_blob`, `kms_getfb`, `kms_flip` (basic subtests), `kms_vblank` (idle subtests), `kms_pipe_crc_basic`, `kms_cursor_crc` (64x64), `prime_self_import`, `sw_sync` | vkms | KMS, CRCs, vblank, PRIME and sync files on the display IGT knows best | a few minutes |
| igt-bochs | the same KMS subset, with the skips Linux's bochs-drm gives | std-vga | the KMS core and bochs | a few minutes |
| igt-virtio | the same KMS subset with `syncobj_basic`, `syncobj_wait`, `syncobj_timeline` | virtio-vga | virtio-gpu and syncobjs (Problems item 23) | a few minutes |
| modetest | every EDID mode set, read back and checksummed | std-vga, virtio-vga | the Done's modetest clause | a few minutes |
| buffers | `fenceprobe`, `udmabufprobe`, `drmprobe`; `vulkaninfo` and `eglinfo` for the extensions step 8 found missing | vkms | 76-3's producer cases, PRIME lifetimes and what Debian's stack probes | under a minute |
| webgl | a few hundred pages of the WebGL suite in Chromium, by case against Debian | virtio-vga, llvmpipe | the WebGL path whole, in small; the whole suite took 472.8 s (1.0.4) and 2809.9 s (2.0.1) in run 2 and stays for the release | about five minutes |
| video | one 1080p H.264 clip in each browser, drops and decoder named, the tone recorded through the loopback | virtio-vga | video and sound against Debian's drops | about two minutes |
| webgpu | Chromium's WebGPU on SwiftShader: one compute and one render readback, adapter named | virtio-vga | the WebGPU clause | about a minute |
| sandbox | Chromium's sandbox diagnostics against Debian's; if the owner takes 76-20, the GPU process sandboxed with one WebGL and one WebGPU page drawn instead | virtio-vga | the sandbox clause, in the form decision D chose | about a minute |
| d3d | the Direct3D 9, 11 and 12 programs through DXVK and vkd3d-proton on lavapipe | virtio-vga | 76-21's programs; its two suites (vkd3d-proton's took 656 s) stay for the release | a few minutes |
| desktop | Weston at boot on bochs (Pixman) and virtio-gpu (GL), input through QMP, a resolution change, SIGKILL and the console's checksum | std-vga, virtio-vga | the session, the handover and resolution | a few minutes |
| i686 | an i686 boot: bochs `modetest`, `evprobe`, an llvmpipe GL checksum | std-vga | the i686 clause | a few minutes |
| limits | `limitsprobe` against the Debian guest's answers | any | 76-19's clause | under a minute |
| ext2stop | one row of the forced ext2 test | any | the ext2 clause after the fixes | a few minutes |
| datapart | the data-partition update rehearsal and a trial root boot | any | the storage clause in QEMU | a few minutes |
| fuzz | `drmfuzz` as an ordinary user for a fixed count | vkms | no kernel fault from a hostile caller | about a minute |
| lenovo | trial kernel and trial root, announced since Weston takes the screen: fbdrm at 1366x768, the touchpad's evdev capabilities, Weston started and killed with the console back, the kept file on the data partition read | the Lenovo | the hardware the milestone touched | one trial boot |

Seventeen cases: sixteen in QEMU, which fit about 15 to 20 minutes of wall time in four parallel guests, and one Lenovo trial boot. The full IGT lists, the whole WebGL suite, Wine's tests and vkd3d-proton's suite, the browser page checks of 76-17a and the Lenovo's video acceptance run once, at the release that ends milestone 83.

### Order and parallelism

#### At once

* 76-1, 76-10a, 76-13a (after the downloads card), 76-15a and 76-16a. With the other milestones' first tasks they fill the nine loops. 76-16b follows 76-16a; its root steps are the manager's setup at approval.
* Right after 76-1, which is the one task most others wait on: 76-2a, 76-7, 76-9a, 76-15b.
* Right after 76-16a: 76-13b, 76-22a, 76-22b (with 76-13a), 76-21 (with 76-13a), 76-20 (with 76-13a and 76-13b, if the owner takes it).
* Right after 76-13a: 76-14a.
* Right after 76-10a: 76-10b, then its Lenovo step 76-10c early in the Lenovo's queue.

#### The critical path

76-1, 76-2a, 76-4a, 76-5a, 76-6, 76-11a, 76-14b, 76-17b: eight tasks in series. After 76-5a, 76-5b, 76-6, 76-8 and 76-12 run at once, and 76-11a follows 76-6, whose bochs and fbdrm its checks need, while its Lenovo step 76-11b runs beside 76-14b; 76-3a runs beside 76-4a and must finish before 76-5b. In QEMU alone the path ends at 76-14b, seven tasks. 76-6 is small, so the manager gives it the first free loop after 76-5a. The storage path (76-1, 76-15b, 76-15c) and the browser path (76-16a, 76-22b, 76-17a) are shorter and run beside it.

#### What would make workers wait or collide, and the fix

* Everyone waits on 76-1. Fix: keep 76-1 to contracts, stubs and registries, give it to the first free loop, and let 76-10a, 76-13a, 76-15a and 76-16a run meanwhile.
* The `rach2/sys/drm/` files: each is a stub from 76-1 owned by one task, and the ioctl handlers are declared once in `drm.h` with stubs in their owners' files, so no task edits another's dispatch entry; `drm_atomic.c` passes from 76-5a to 76-5b, and `rach2/sys/drm/sources.mk` is a row file, to which 76-18b and 77-2 add rows.
* `rach2/sys/fs/synthfs.c`, `synthfs.h`: 76-1, then 76-9a, then 76-9b; 80-10 after 76-9b.
* `rach2/sys/fs/procfs.c`: 76-9a, then 76-9b; 80-10's part after 76-9b, which replaces the plan's 76-19 step, since the two `/proc/sys` files moved to 76-9b.
* `rach2/sys/syscall_proc.c`: 76-2a, then 76-2b, then 76-19 and 79-1b at far-apart hunks; 79-4 follows 79-1b, whose `mmap_place` hunk it shares, and is far from 76-19's.
* `rach2/sys/mm/`: 76-2a, then 76-2b, then 76-19; 77-3, 81-1 and 82-2 wait on 76-2b.
* `systab.c` and `sysnr.h` for both ABIs: 76-4a (`kcmp`), then 76-19; 79-1's `futex_waitv` and 80-10's `name_to_handle_at` are row inserts after them.
* `rach2/sys/fs/devfs.c`: 76-1, then 76-2a (`/dev/mem`); later nodes register through 76-1's registry and do not edit it.
* `rach2/kernel.c`: 76-1, then 76-15b.
* `rach2/sys/network/socket/netlink.c`: 76-9a, then 76-9b.
* `rach2/sys/fs/vfs.c`: 76-18a, then 80-8a.
* `rach2/sys/arch/x86/sources.mk`: 76-1's include of `rach2/sys/drm/pc.mk`, 76-10b's row, 78-1a's watchdog row and 78-2a's include of i915's manifest are one line each, under the one-line rule.
* `rach2/sys/arch/x86/ps2mouse.c`: 76-10a, then 76-10b. `rach2/sys/evdev.c`: 76-10a, then 79-6b.
* `utils/init.c` and `vendor/gnu/install_root.sh`: 76-13a, then 76-14a.
* `vendor/gnu/desktop/` and `desktop_scenario.c`: 76-14a, then 76-14b.
* `tools/etude/qemu_profile.c`: 76-16a, then 76-16b.
* `tests/graphics/Readme.md`: 76-16a creates it; 76-16b, 76-17a, 76-21, 76-22a and 76-22b each add their own section, which is a row-file edit (a section per task).
* `man/man2/brk.2`: 76-2a, then 76-19 and 79-1b, each in a section of its own (76-19's madvise and guard regions, 79-1b's `MAP_32BIT` in the mmap section). `man/man4/drm.4`: 76-4a, then 76-4b. `man/man7/drm-kms.7`: 76-5a, then 76-5b. `man/man5/sysfs.5`: 76-9a, then 76-9b. `man/man4/input.4`: 76-10a, then 76-10b. `man/man5/debugfs.5`: 76-18a, then 76-18b. `man/man7/desktop.7`: 76-14a, then 76-14b.
* The GNU root: nine tasks are `linux-host` and each builds a root. Fix: each worker takes the unchanged vendor and GNU builds from the testing host's cache, as AGENTS.md's Resources section asks, so a root costs the changed packages only; 76-13a lands first and the cache is refilled from its head.
* The Lenovo: four steps in 76 (76-10c, 76-11b, 76-15c, 76-17b) among the other milestones' steps, one at a time. Fix: the order 76-10c, 76-11b, 76-15c, 76-17b, each prepared whole before the hand-back so the Mac loop holds the machine only for the step.
* The Debian guest: 76-13b, 76-15a, 76-20, 76-21, 76-22a, 76-22b and 76-22c all use it. Fix: each run starts its own copy from the pinned image, within 76-22a's admission.

## 77 vgpu: accelerated QEMU

### 77-1 Render-node access check (folded into 76-16a)

Not filed. Its check is 76-16a.8 and 76-16a.9, beside the virgl profile whose environment the check uses; the manager puts an account outside `render` into it as setup (Problems item 4).

### 77-2 virtio-gpu 3D

Builds: virgl 3D in Rach2's virtio-gpu driver: contexts, 3D resources, transfers, capsets, submission with fences, waits, and teardown on reset. Submission is pipelined and keeps only the waits the semantics require, since QEMU 10.2.1's 10 ms fence poll holds a driver that waits every frame near 90 to 130 frames a second (plan.md, Risks, item 14).

* Files: `rach2/sys/drm/virtio_gpu.c` (the VIRGL feature, GETPARAM values, the 3D ioctls in its table); new `rach2/sys/drm/virtio_gpu_3d.c` (contexts, 3D resources, transfers, GET_CAPS), `rach2/sys/drm/virtio_gpu_submit.c` (EXECBUFFER, BO lists, fence descriptors, pipelining), `rach2/sys/drm/virtio_gpu_fence.c` (fence ids, the fence queue, WAIT, reset); new stub `rach2/sys/drm/virtio_gpu_blob.c` for 77-3; `rach2/sys/drm/sources.mk`; new `tests/vgpuprobe.c`; new `tests/host/vgpu3d_hosttest.c` over 76-8's device model; new `tests/cases/machine.vgpu.json` and a `vgpu` line in `tests/registry`; the virtio-gpu sections of the DRM Readme and of 76-8's page under `man/man4/`.
* Waits on: 76-8 (code: the 2D driver, its ioctl table and its model), 76-3 (code: sync files and syncobjs), 76-16a (code: the virgl profile).
* Labels: `linux-host`.
* Subtasks:
  * 77-2.1 Negotiate `VIRTIO_GPU_F_VIRGL`; answer GETPARAM 3D_FEATURES, CAPSET_QUERY_FIX and SUPPORTED_CAPSET_IDs; read the capset infos at probe and answer GET_CAPS for capsets 1 and 2; the host test checks each command against the model.
  * 77-2.2 Contexts per open file (CTX_CREATE, CTX_DESTROY) and 3D resources (RESOURCE_CREATE_3D with Linux's argument checks), backing attached, TRANSFER_TO_HOST and TRANSFER_FROM_HOST with box, level and stride; host test.
  * 77-2.3 EXECBUFFER with BO handles, an in-fence descriptor waited by the kernel, an out-fence as a sync file (76-3), fence ids completed from the fence queue's interrupt; WAIT with and without NOWAIT; host test with completions out of order.
  * 77-2.4 Pipelining: submissions queue without waiting on earlier fences; a transfer waits only on the fences of the resources it touches; the host test counts the waits a frame makes and requires none on the submit path.
  * 77-2.5 Reset: every pending fence fails, every context and resource is freed, and later calls on old handles fail as Linux's; a debugfs file through 76-18 triggers a reset for tests; host test.
  * 77-2.6 `tests/vgpuprobe.c` (rows: params, context, resource, transfer, submit, out-fence, wait, reset) in `PROGS` and `tests/probe-suites`, run by a `machine.vgpu` case on QEMU's `virtio-vga-gl` under the virgl profile; its author boots it once on the testing PC.
  * 77-2.7 The DRM Readme's virtio-gpu 3D section (fences, pipelining, reset) and the 3D ioctls on 76-8's manual page.
* Done: vgpuprobe passes on the virgl profile, and a reset fails every pending fence and frees every resource.

### 77-3 Blobs and context init

Builds: blob resources and host-visible memory, and context init with the capsets QEMU offers (1 virgl, 2 virgl2, 4 Venus). Native context (capset 6) is proven by host test only, since stock QEMU 10.2.1 and 10.0.13 offer no native context (plan.md, 77's table, 77-3).

* Files: `rach2/sys/drm/virtio_gpu_blob.c` (from 77-2's stub: RESOURCE_CREATE_BLOB, MAP_BLOB, CONTEXT_INIT); `rach2/sys/drm/virtio_gpu.c` (the RESOURCE_BLOB, HOST_VISIBLE and CONTEXT_INIT features and params); the PCI transport in `rach2/sys/virtio/` for the shared memory capability; `tests/vgpuprobe.c` (blob rows); new `tests/host/vgpublob_hosttest.c`.
* Waits on: 77-2 (code), 76-2 (code: windows onto a BAR at 14 TiB), 76-7 (code: the transport), 76-16b (code: the Venus profile for 77-3.5's boot).
* Labels: `linux-host`.
* Subtasks:
  * 77-3.1 Read `VIRTIO_PCI_CAP_SHARED_MEMORY_CFG` (type 8) for region 1, the host-visible window, and map pieces of it through 76-2's windows, never whole (QEMU puts the 4 GiB window at 14 TiB, plan.md, 76's table, 76-2); host test of the capability walk.
  * 77-3.2 Blob resources in guest, host3d and host3d-guest memory, with the mappable and shareable flags and blob ids; MAP_BLOB places the blob in the window and gives the program an offset to map, with the cache type the device answers; unmap and free in order; host test.
  * 77-3.3 Context init with capset id, ring count and poll-ring mask; a fence context per ring; EXECBUFFER with a ring index; host test with capsets 1, 2, 4 and 6 against the model.
  * 77-3.4 PRIME export of blob resources as dma-bufs and import back (76-3), so Venus and the compositor share buffers; host test.
  * 77-3.5 vgpuprobe rows: a blob created, mapped, written, and read back through a transfer; context init with each capset the device offers; its author boots once with `venus=on,blob=on,hostmem=1G`.
  * 77-3.6 The Readme and manual page sections for blobs and context init.
* Done: on the testing PC a Venus context is created and a host-visible blob is mapped and read back right; capset 6 passes its host test.

### 77-4a Venus off-screen

Builds: Venus in QEMU over the testing PC's host lavapipe with the udmabuf module (decision A), with Debian's virglrenderer and 77-7's Venus driver: render, copy and read back, then Mesa's lavapipe fraction of dEQP-VK (VK-GL-CTS 1.4.1.1) with the `timeline_semaphore` groups counted as flakes, matched against a Linux guest in the same QEMU. It also carries 77-5's Vulkan part: device loss on Venus and the host renderer named. Split from 77-4 because presenting needs the session (76-14) and the off-screen part does not.

* Files: new `tests/cases/machine.venus.json` and a `venus` line in `tests/registry`; new `tests/graphics/deqp-vk-lavapipe.txt` (the case list); new `tests/graphics/ref/venus-wsl/` (the Linux guest's results); kernel fixes in 77-2's and 77-3's files, both closed by then.
* Waits on: 77-3 (code), 77-7b (code: the chosen Venus driver in the root), 77-9 (code: VK-GL-CTS and the dEQP runner), 76-16b (code: the Venus profile; the module is the manager's setup at approval).
* Labels: `linux-host`.
* Subtasks:
  * 77-4a.1 Boot the Venus profile of 76-16b and record that the guest's `vulkaninfo --summary` names Venus over the host's lavapipe at 1.3.269 with the module loaded, and the host renderer.
  * 77-4a.2 Off-screen: `dEQP-VK.api.smoke.*` and one render, copy and readback case pass.
  * 77-4a.3 The case list from Mesa 25.0.7's CI fraction for lavapipe; the Linux guest's results in the same QEMU taken and stored, `timeline_semaphore` marked as flakes; its author runs every 500th case on Rach2 once.
  * 77-4a.4 Device loss: the host's render server killed under load; Rach2's Vulkan programs see `VK_ERROR_DEVICE_LOST` as the Linux guest's do, and the guest goes on.
  * 77-4a.5 Rates recorded beside the Linux guest's under the same QEMU, never as absolute marks; the tests section of the DRM Readme names the Venus profile.
* Done: every 500th case of the lavapipe fraction of dEQP-VK on Venus matches the Linux guest's stored results by case with the flakes named, and device loss behaves as in the Linux guest; the slice runs the same part, and the release runs the `venus` family's fraction whole.

### 77-4b Venus presenting

Builds: vkcube presenting through Venus under Weston on the testing PC, frames read inside the guest, with the testing PC's Venus timing rerun and its output kept before any gate uses it (plan.md, 77's table, 77-4). The Lenovo's part of 77-4 is in 77-10.

* Files: new `tests/cases/machine.vkpresent.json` and a `vkpresent` line in `tests/registry`; stored timings in `tests/graphics/ref/venus-wsl/`.
* Waits on: 77-4a (code), 76-14 (code: Weston for an ordinary user), 76-22 (code: in-guest capture and checksums).
* Labels: `linux-host`.
* Subtasks:
  * 77-4b.1 vkcube under Weston on Venus with QEMU's GL display on llvmpipe over vgem (the Venus-on-screen profile of 76-16b); frames captured inside the guest and checksummed.
  * 77-4b.2 The same in the Linux guest in the same QEMU, timings kept with their output; Rach2's rate compared with it.
  * 77-4b.3 A case that presents N frames and checks the checksums, run once by its author.
* Done: vkcube presents through Venus on the testing PC with in-guest checksums, its rate recorded beside the Linux guest's.

### 77-5 GL acceptance on virgl

Builds: piglit `quick` whole and dEQP-GLES subsets on virgl matched against a Linux guest in the same QEMU, chosen by what the guest is offered (OpenGL 4.2, OpenGL ES 3.1); the WebGL suite; device loss; the host renderer named in every record. Its Vulkan part is in 77-4a.

* Files: new `tests/cases/machine.virgl.json` and a `virgl` line in `tests/registry`; new `tests/graphics/piglit-virgl.txt` and `tests/graphics/deqp-gles-virgl.txt`; new `tests/graphics/ref/virgl-wsl/`.
* Waits on: 77-2 (code), 77-9 (code: dEQP-GLES in the root), 76-14, 76-17 and 76-22 (code: the session, the browser launcher and the runners).
* Labels: `linux-host`.
* Subtasks:
  * 77-5.1 The renderer check: `eglinfo` and `glxinfo` in the guest name virgl and the host's "D3D12 (NVIDIA GeForce RTX 4080 SUPER)"; every runner's record carries both.
  * 77-5.2 The piglit list (`quick` filtered to GL 4.2 and ES 3.1) and the Linux guest's results, taken in the same QEMU and stored; its author runs every 200th test on Rach2 once.
  * 77-5.3 dEQP-GLES2, GLES3 and GLES31 (VK-GL-CTS 1.4.1.1) lists for what virgl offers, with the Linux guest's results stored; its author runs every 200th case once.
  * 77-5.4 The WebGL suite in Chromium and Firefox on virgl through 76-22's runner, chunked as 76-22 does, with the Linux guest's results stored; one chunk run once.
  * 77-5.5 Device loss: a reset through 77-2's trigger while a robust GL context draws; the context reports a reset, Weston restarts its renderer, and the result is recorded beside the Linux guest's behaviour on the same QEMU event.
* Done: each runner judges piglit, dEQP-GLES and the WebGL suite on virgl by case against the Linux guest's stored results, with the renderer named, and its author's every-200th parts and one chunk match; the release runs the `virgl` family whole.

### 77-6 DXVK and vkd3d-proton on Venus

Builds: 76-21's work on a Venus adapter, once 77-7 gives a Venus driver with `VK_KHR_load_store_op_none`, `maintenance5`, `maintenance6` and `VK_EXT_map_memory_placed`; until then WineD3D on Vulkan and Wine's vkd3d are the Direct3D paths on Venus. 64-bit programs only; the 32-bit ones are 79-8d's.

* Files: new `tests/cases/machine.d3dvenus.json` and a `d3dvenus` line in `tests/registry`; new `tests/graphics/ref/d3d-venus-wsl/`; the record of refusals in `dev-log/77-vgpu/`.
* Waits on: 77-4a (code), 77-7b (code), 76-21 (code: the programs, the prefix and the suites' runners on lavapipe).
* Labels: `linux-host`.
* Subtasks:
  * 77-6.1 WineD3D on Vulkan and Wine's vkd3d on Venus: the 76-21 programs draw and read back, as the interim paths.
  * 77-6.2 DXVK: a Direct3D 11 program on the Venus adapter, adapter named and pixels read back.
  * 77-6.3 vkd3d-proton: a Direct3D 12 program likewise.
  * 77-6.4 Only after 77-6.2 and 77-6.3 pass: Wine's Direct3D tests and vkd3d-proton's suite on Venus with the Linux guest's results stored; one unit of each run once.
  * 77-6.5 Every feature a program refuses to start without, recorded with its extension; a refusal that the Linux guest shows too records a blocker and does not complete the task.
* Done: a Direct3D 11 and a Direct3D 12 program run on a Venus adapter with results read back right, and the suites' results are stored with one unit of each matched; the release matches them whole.

### 77-7a Mesa qualification experiment

Builds: the first experiment of 77-7 (plan.md, 77's table, 77-7), on the testing PC with the module: trixie-backports' Mesa 26.1.6 as a Linux guest's Venus driver, its extensions read, then DXVK 3.1.1, vkd3d-proton 3.1 and a 32-bit WoW64 probe, each with its adapter named and its pixels read back; then Weston, the browsers and the suites on it. Split from 77-7 because it needs no Rach2 code and can start at once; 77-7b pins the result.

* Files: new `dev-log/77-vgpu/mesa-qualification.md` (the record); scratch under `build/`.
* Waits on: the downloads card (`decision`: Mesa 26.1.6). The module is loaded by the manager's setup at approval (76-16b), before this task is Ready.
* Labels: `linux-host`.
* Subtasks:
  * 77-7a.1 The Debian 13 reference guest of 76-16a with 26.1.6's Venus driver; `vulkaninfo` read for `VK_KHR_load_store_op_none`, `maintenance5`, `maintenance6` and `VK_EXT_map_memory_placed`.
  * 77-7a.2 GE-Proton11-12's DXVK and vkd3d-proton on it: a Direct3D 11 and a Direct3D 12 program, adapters named, pixels read back.
  * 77-7a.3 A 32-bit program under WoW64 on it, with the mapping that failed under Linux on 25.0.7 tried again.
  * 77-7a.4 Weston, Chromium and Firefox, and the dEQP-VK fraction on it; nothing named as the fix before it passes.
  * 77-7a.5 The record: what passed, the chosen Mesa, and the references a new pin must retake.
* Done: the record names the Mesa that passes DXVK, vkd3d-proton and the WoW64 probe on Venus under Linux, each with its adapter named and its pixels checked.

### 77-7b The Venus driver in the root

Builds: the chosen Mesa's Venus driver pinned in the root beside 76-13's 25.0.7, which stays for llvmpipe, lavapipe, Crocus and hasvk; new Linux references for the new pin.

* Files: `vendor/gnu/debian/venus.list` (new, its own list under 76-13a's form: the pin by SHA-256, the Venus ICD with its own JSON and library path); `vendor/gnu/Readme.md` (its own section); new Linux guest references in `tests/graphics/ref/venus-wsl/`.
* Waits on: 77-7a (evidence), 76-13 (code).
* Labels: `linux-host`.
* Subtasks:
  * 77-7b.1 Pin the package by SHA-256 in `venus.list`, once the downloads card is answered.
  * 77-7b.2 Install only its Venus ICD and library, so the loader lists Venus from the new Mesa and every other driver from 25.0.7; checked with `vulkaninfo --summary` in QEMU.
  * 77-7b.3 Build the same for i386, for 79-8b.
  * 77-7b.4 Retake the Linux guest's references that the new pin changes, for 77-4a and 77-6.
  * 77-7b.5 State the two Mesa pins and why in the Venus section of `vendor/gnu/Readme.md`.
* Done: the root carries the chosen Venus driver beside 25.0.7 for amd64 and i386, and its Linux references are stored.

### 77-8 WebGPU on Venus

Builds: Chromium's WebGPU on a Venus adapter on the testing PC, counted only on an adapter named as Venus: over lavapipe Chromium withholds it as a CPU adapter, as it does under Linux (plan.md, 77's table, 77-8). Each run names Chromium's display platform. The Lenovo's part is in 77-10.

* Files: new `tests/cases/machine.webgpu.json` and a `webgpu` line in `tests/registry`; the record in `dev-log/77-vgpu/`.
* Waits on: 77-4a (code), 76-17 (code: the browser launcher), 80-2's trace (evidence: why Chromium lists Venus and returns no adapter).
* Labels: `linux-host`.
* Subtasks:
  * 77-8.1 Chromium with 76-17's switches on the Venus profile: the adapter list and `requestAdapter`'s answer recorded beside the Linux guest's in the same QEMU.
  * 77-8.2 80-2's trace read and its finding tried on Rach2 and in the Linux guest alike.
  * 77-8.3 A case that requires Rach2's answer to equal the Linux guest's, and counts WebGPU passes only on a Venus-named adapter.
* Done: Rach2's adapter answer matches the Linux guest's in the same QEMU, and no pass is counted on another adapter.

### 77-9 VK-GL-CTS in the root (new)

Builds: VK-GL-CTS 1.4.1.1 (Mesa 25.0.7's CI pin, for the testing PC's references) and 1.4.7.1 (the Lenovo's references, plan.md, How it is verified) built from source at pinned revisions for the x86-64 GNU root, and a dEQP runner in 76-22's form. No task of the plan builds them (Problems 2). Kept in 77, whose acceptance first runs dEQP on Rach2, so that 76-13a stays the packaging task.

* Files: new `vendor/gnu/vk-gl-cts/` (fetch and build script, pins, Readme), lines in `vendor/gnu/sources.txt`; new `tests/graphics/runners/deqp.sh` (the runner: case list, every Nth selection, one line of numbers, results as step 9's CSVs).
* Waits on: 76-13a (code: the root's build), 76-22b (code: the runner form and the common record); the downloads card (decision).
* Labels: `linux-host`.
* Subtasks:
  * 77-9.1 Fetch both releases and the external sources each release's `fetch_sources.py` names, each pinned by SHA-256.
  * 77-9.2 Build `deqp-vk` and `glcts` with dEQP-GLES2, GLES3 and GLES31 for the surfaceless EGL target, both versions, staged under `/opt/vk-gl-cts-VERSION` in the root.
  * 77-9.3 `tests/graphics/runners/deqp.sh`: case lists, every Nth case, flakes named, the renderer and IOMMU state recorded, results comparable with `run2/step9/deqp/*/results.csv.gz`.
  * 77-9.4 Its author runs dEQP-GLES2's every 200th case on llvmpipe in QEMU once.
  * 77-9.5 `vendor/gnu/vk-gl-cts/Readme.md` and the GNU layer's Readme.
* Done: both versions run in the root, and the runner's output compares by case with step 9's results.

### 77-10 Venus on the Lenovo's Debian host

Builds: the Lenovo's parts of 77-4 and 77-8 in one Debian session: Rach2 as a guest of Debian's QEMU 10.0.13 with Venus over hasvk, vkcube through QEMU's GTK and SDL displays (Linux's guest: 50.6 and 56.4 FPS in FIFO, against 60.07 for hasvk directly), a dEQP-VK subset, and Chromium's WebGPU adapter answer (Linux lists Venus as available and returns no adapter). Split out because it takes the Lenovo and needs both 77-4b and 77-8.

* Files: the record in `dev-log/77-vgpu/`; the guest image and scripts on the Lenovo's data partition.
* Waits on: 77-4b, 77-8 (code), 76-15c (`evidence`: the data partition for the guest image).
* Labels: `lenovo`.
* Subtasks:
  * 77-10.1 Prepare: Rach2's x86-64 GNU image with 77-7b's Venus driver copied to the data partition; Debian's QEMU command lines with `venus=on,blob=on,hostmem`, GTK and SDL displays; the Linux guest's run beside it in the same session; risk 15's settings (`intel_iommu=off`, `shmem_enabled=never`).
  * 77-10.2 What passes: Rach2's vkcube rate within the Linux guest's under the same display, its dEQP-VK subset matching the Linux guest's by case, and its WebGPU answer equal to Linux's; hand back `owner-only` with a "Lenovo notice:" posted, since the windows take the screen.
  * 77-10.3 The Lenovo step (Mac loop): a Debian boot of its own, since the windows take the GPU; `mixer vol 1` after the boot.
* Done: on the Lenovo, Rach2's rates and dEQP-VK subset match the Linux guest's in the same QEMU, and its WebGPU answer matches Linux's.

## 78 intel: Haswell rendering, video and every output

### 78-1a Watchdog driver and display tools

Builds: Rach2's driver for the chipset's TCO watchdog (version 2 on the Lenovo's Lynx Point LP, at I/O 0x1860, `run2/step11.md:36`; ICH9's in QEMU's q35) with Linux's `/dev/watchdog` interface; a register dump through `/dev/mem`, the pipe CRC and a scanout capture. Split from 78-1 because the code needs no Lenovo and starts at once; 78-1b holds the expiries.

* Files: new `rach2/sys/arch/x86/itco.c`, `itco.h`; `rach2/sys/arch/x86/sources.mk`; `rach2/sys/arch/x86/board.c` and `board.h` (an accessor for the profile's PMBASE and RCBA); a row in 76-1's device registry (`watchdog` at 10:130), not `devfs.c`; new `man/man4/watchdog.4`; `rach2/hardware.md` (a TCO row); new `tests/host/itco_hosttest.c` with a TCO model; new `tests/cases/machine.watchdog.json` and a `watchdog` line in `tests/registry`; new `utils/gfxtool.c`, `utils/programs.mk` (`SBIN_PROGS`), new `man/man8/gfxtool.8`.
* Waits on: 76-1 (code: the device registry).
* Labels: none.
* Subtasks:
  * 78-1a.1 The TCO driver for the `ich9` and `lynxpoint-lp` profiles (`board.c:40-45`): TCOBASE at PMBASE + 0x60, NO_REBOOT in GCS at RCBA + 0x3410, reload, the timeout in 0.6 s ticks, halt and start, the two-stage expiry, as Linux's `iTCO_wdt` version 2; every wait bounded; `tests/host/itco_hosttest.c` checks the register sequence and the expiry against the model.
  * 78-1a.2 `/dev/watchdog` at 10:130 with Linux's interface: open arms, write feeds, the magic close `V` disarms, nowayout 0 as Debian's, and `WDIOC_GETSUPPORT`, `GETSTATUS`, `GETBOOTSTATUS`, `KEEPALIVE`, `SETTIMEOUT`, `GETTIMEOUT`, `GETTIMELEFT` and `SETOPTIONS`; `man/man4/watchdog.4`; the `rach2/hardware.md` row.
  * 78-1a.3 A q35 case that arms the watchdog, stops feeding it, and reads QEMU's watchdog action through QMP; its author runs it once.
  * 78-1a.4 `utils/gfxtool.c`: `dump` prints Haswell's display, power, forcewake, GTT and ring registers by name through `/dev/mem` (BAR0 at 0xc0000000 is below 4 GiB, which `/dev/mem` covers, `man/man4/mem.4`); `crc` reads the pipe CRC for N frames and restores its control; `capture` reads the primary plane's surface, stride and format and writes a PPM from the aperture; every wait bounded; it refuses any GPU but 8086:0a16 and a GPU a driver owns.
  * 78-1a.5 `man/man8/gfxtool.8`, the program in `SBIN_PROGS`; its author runs it once on QEMU's std-vga to see the refusal.
* Done: the TCO host test and the q35 case pass, and gfxtool refuses every GPU but the HD 4400.

### 78-1b Watchdog and tools on the Lenovo

Builds: the proof that the watchdog brings the Lenovo back, before any hang experiment relies on it: one controlled expiry under Debian on p5, then Rach2's driver armed, fed and forced to expire on a trial boot; and gfxtool run on the firmware's display state, whose dump becomes the host model's fixture.

* Files: new `tests/fixtures/lenovo-gpu/firmware-dump.txt`; the record in `dev-log/78-intel/`.
* Waits on: 78-1a (code).
* Labels: `lenovo`.
* Subtasks:
  * 78-1b.1 Prepare the Debian part: p5 booted through GRUB's one-shot entry; `wdctl` read; systemd's runtime watchdog turned off for the session; `/dev/watchdog` opened and left unfed; the expected path back (firmware reset, GRUB's default entry, the installed Rach2). A Debian boot of its own, since the expiry ends the session by a reset. The `machine` skill has Linux leave by `rtcwake -m off -s 120` because a warm reboot once left the firmware unable to boot (`machine/SKILL.md:43`); the expiry is the chipset's own reset and the event under test, so the step records whether the firmware boots after it, and a failure to come back is this task's finding, reported at once.
  * 78-1b.2 Prepare the Rach2 part: the review note for `itco.c` and the manager's go; `bootctl kernel FILE`; after boot, gfxtool's dump, CRC and capture on the firmware's display, then the watchdog armed at 30 s, fed for two minutes, `GETTIMELEFT` read, feeding stopped; what passes is a reset within the timeout and the installed system back. Hand back `owner-only`.
  * 78-1b.3 The Lenovo step (Mac loop), silent: the Debian expiry, then the Rach2 trial, `mixer vol 1` after each boot; times to return recorded; the dump committed with the step's record as the fixture, and the CRC and capture compared with Linux's dumps of 80-2 where they exist.
* Done: both expiries brought the Lenovo back unattended, and the firmware state's dump is in the tree.

### 78-2a Driver skeleton and chip model

Builds: the Intel driver's skeleton: the Haswell register header, the hardware boundary through which every register access goes (so that the host model and the fault backend can stand in for the chip), the PCI identity `i915` for 8086:0a16 through 76-4, and a host model of the HD 4400 seeded with the Lenovo's facts. Split from 78-2 so that 78-7, 78-10a and 78-2b can start from it at once.

* Files: new `rach2/sys/drm/i915/sources.mk`, `i915.h`, `i915_reg.h`, `i915_hw.h`, `i915_pci.c`, `Readme.md`; the include of i915's manifest in `rach2/sys/arch/x86/sources.mk` beside 76-1's `pc.mk`, into `PC_C_SRC`, since the driver is PC-only and `check-arch` compiles shared files for ARM, and the prerequisite in `rach2/guest.mk:63`; new `man/man4/i915.4`; new `tests/host/i915_model.c` (the model's core and its table of blocks), `i915_model.h`, `i915_lenovo.h`, `tests/host/i915_pci_hosttest.c`; `rach2/hardware.md` (the HD 4400 row).
* Waits on: 76-1 (code: the interface header and identity table), 76-4 (code: DRM device registration). The model needs only the ids, BARs and stolen range, which `dev-log/70-wifi-chip/linux/dmesg.txt:190-200` and `:319-322` hold; the VBT and OpRegion bytes are 78-10a's.
* Labels: none.
* Subtasks:
  * 78-2a.1 `i915_reg.h` from Linux 6.12's register headers for the blocks 78 uses, Haswell's values only, each block naming its source file; it compiles for i686 and x86-64.
  * 78-2a.2 `i915_hw.h` and its MMIO implementation over BAR0 (4 MiB at 0xc0000000 on the Lenovo, `dev-log/70-wifi-chip/linux/dmesg.txt:319-322`) mapped uncached with `map_mmio_typed`; every wait takes a bound and returns a named error.
  * 78-2a.3 `i915_pci.c`: bind 8086:0A16 and the Haswell ids Linux 6.12 gives the same device data; bus mastering and MSI on; the DRM device registered as `i915` with Linux's version and features through 76-4. `intelbl.c` keeps the backlight until 78-9.
  * 78-2a.4 The model: `tests/host/i915_model.c` implements `i915_hw.h` over a register file that later tasks extend, seeded from `i915_lenovo.h` (ids, BARs, stolen memory at 0xaba00000 to 0xaf9fffff; 78-10a adds the VBT and OpRegion); `i915_pci_hosttest.c` binds the driver to it; registered in `tests/cases/host.host.json`. Each later task adds its registers in a block file of its own, registered in the core's table, so that tasks that run at once never edit one model file: `i915_model_base.c` 78-2b, `i915_model_ring.c` 78-4a then 78-8a, `i915_model_reset.c` 78-6a, `i915_model_rc6.c` 78-6b, `i915_model_pipe.c` 78-9, `i915_model_sink.c` 78-10b, `i915_model_ddi.c` 78-10c; 78-4b edits the block that holds a difference, after that block's owner.
  * 78-2a.5 The Readme (summary, Design with the hardware boundary and the lock order, Limits, a section per task), `man/man4/i915.4` with a section per ioctl family, and the `rach2/hardware.md` row.
* Done: the driver binds to the model, and every register access goes through `i915_hw.h`.

### 78-2b Intel base

Builds: forcewake, power wells, the global GTT and stolen memory reserved, so later parts can touch the GT and map memory.

* Files: new `rach2/sys/drm/i915/i915_uncore.c`, `i915_power.c`, `i915_gtt.c`, `i915_stolen.c`; new `tests/host/i915_model_base.c` (the model's forcewake, power wells and GTT); new `tests/host/i915_base_hosttest.c`.
* Waits on: 78-2a (code).
* Labels: none.
* Subtasks:
  * 78-2b.1 Forcewake on Haswell (`FORCEWAKE_MT`, its ack, a reference count, a bounded wait with a named error) and the GT FIFO's free entries; host test with a slow and a stuck ack.
  * 78-2b.2 Power wells (`HSW_PWR_WELL_CTL` request and state), the display well never turned off while the firmware's pipe runs; host test.
  * 78-2b.3 Stolen memory from BDSM, checked against the RMRR (0xab800000 to 0xaf9fffff), reserved from the frame allocator before anything else can take it; host test with the Lenovo's values.
  * 78-2b.4 The GGTT: Haswell's PTE format and cache bits, a scratch page, insert and clear ranges, and the firmware's entries for its framebuffer kept, since the console draws there through the aperture at 0xb0000000 (`rach2/sys/display/Readme.md`); host test.
  * 78-2b.5 The Readme's base section: forcewake rules, stolen memory, the GTT's layout.
* Done: the base host test passes, a stuck forcewake ack fails by name, and the firmware's GTT entries stay as they were.

### 78-3a Buffers

Builds: GEM objects: create, CPU and GTT maps (the 256 MiB aperture reached through windows, as Linux's io_mapping does, since the kernel's device window is 128 MiB, `cpu.h:148-149`), caching, tiling with Haswell's swizzle and fences, busy and wait, PRIME. Split from 78-3 because eviction, purging and userptr (78-3b) need the memory hooks of Problems 9 and nothing else waits for them.

* Files: new `rach2/sys/drm/i915/i915_gem.c`, `i915_gem_mman.c`, `i915_gem_tiling.c`, `i915_vma.c`, `i915_gem_prime.c`; new stub `i915_gem_evict.c` (fails a bind that does not fit, for 78-3b); new `tests/host/i915_gem_hosttest.c`; new `tests/i915gemprobe.c`.
* Waits on: 78-2b (code), 76-2 (code: device mappings, windows and revocation), 76-4a, 76-4b (code: handles and PRIME), 78-7 (code: the fault backend for the QEMU check).
* Labels: none.
* Subtasks:
  * 78-3a.1 Objects from 76-2's page source, handles, close, PREAD and PWRITE with cache flushes where the object is not coherent; host test.
  * 78-3a.2 SET_DOMAIN, SW_FINISH, SET_CACHING and GET_CACHING as Linux on an LLC part; host test.
  * 78-3a.3 MMAP_OFFSET with WB and WC, and GTT maps through a fault handler that binds the object and maps aperture pages through a window, revoked on unbind; host test with more objects than the windows hold.
  * 78-3a.4 X and Y tiling, the swizzle mode read as Linux's `i915_gem_detect_bit_6_swizzle`, fence registers for GTT maps of tiled objects; host test of the detiling arithmetic against Linux's tables.
  * 78-3a.5 BUSY, WAIT with timeouts, PRIME export and import through 76-3; host test.
  * 78-3a.6 `tests/i915gemprobe.c` on the fault backend in QEMU: a tiled object written through a GTT map and read back through a CPU map; its author runs it once; the man page's buffer section.
* Done: the GEM host test passes, and on the fault backend a tiled object written through a GTT map reads back right through a CPU map.

### 78-3b Eviction, purgeable buffers and userptr

Builds: eviction and rebinding of idle buffers, aperture accounting, purgeable buffers given up under memory pressure, and userptr with its invalidation rules; done when a working set larger than the aperture that fits in RAM runs, its contents checked after rebinding.

* Files: `rach2/sys/drm/i915/i915_gem_evict.c` (from 78-3a's stub); new `i915_gem_shrinker.c`, `i915_gem_userptr.c`; `i915_gem.c` (MADVISE, GET_APERTURE) after 78-3a; `tests/i915gemprobe.c` after 78-3a; new `tests/host/i915_evict_hosttest.c`; `rach2/sys/mm/` only if 76-2 does not provide the hooks (Problems 9).
* Waits on: 78-3a (code), 76-2 (code: the shrinker registration and the address-space change notifier).
* Labels: none.
* Subtasks:
  * 78-3b.1 Eviction: idle bound objects in LRU order evicted for a bind that does not fit the mappable aperture or the GGTT, rebound on next use, pinned and active objects never evicted; host test with a working set three times the aperture and its contents checked.
  * 78-3b.2 GET_APERTURE's accounting; host test.
  * 78-3b.3 MADVISE and purging of DONTNEED objects from a shrinker that memory pressure calls before an allocation fails (`rach2/sys/mm/pressure.h:7-10` reclaims only the page and buffer caches today); host test under pressure.
  * 78-3b.4 USERPTR: pages pinned on bind and released when the process unmaps, changes protection, forks or exits, bound VMAs revoked first; the unsynchronized flag refused without privilege as Linux; host test.
  * 78-3b.5 i915gemprobe rows on the fault backend: a working set beyond the aperture, a purge, userptr, and pressure beyond RAM ending in ENOMEM; its author runs them once; the Readme's memory section.
* Done: on the fault backend a working set larger than the aperture runs with its contents checked, and pressure beyond RAM ends in a clean error.

### 78-4a Ring, interrupts and first light

Builds: the render ring and the blitter ring (IGT's gem tests use both), the hardware status page, MSI and the GT interrupts, and first light: one `MI_STORE_DWORD_IMM` from the render ring to memory before any display code. Split from 78-4 so that the Lenovo trial (78-4b) holds no code.

* Files: new `rach2/sys/drm/i915/i915_ring.c`, `i915_irq.c` (every GT and display source with a handler table), `i915_firstlight.c`; new stub `i915_ppgtt.c` with `i915_ppgtt_enable_ring`, called at ring start, for 78-8a; new `tests/host/i915_model_ring.c` (the model's rings and interrupts); new `tests/host/i915_ring_hosttest.c`; a `rach2/cmdline.md` row.
* Waits on: 78-2b (code).
* Labels: none.
* Subtasks:
  * 78-4a.1 The status page in the GGTT, ring buffers bound in the GGTT, ring start with `RING_VALID` and a bounded wait for idle; host test.
  * 78-4a.2 One MSI vector; GT IIR read and acknowledged; IMR and IER for the render and blitter user interrupts; every other source masked until its owner registers; spurious interrupts counted; host test.
  * 78-4a.3 Breadcrumbs: `MI_STORE_DWORD_INDEX` to the status page and `MI_USER_INTERRUPT`; waiters woken from the interrupt; host test.
  * 78-4a.4 First light under a boot option (`rach2.i915.firstlight`): a value written to a GGTT page by the render ring and read back, logged as `i915: first light: ok` or a named failure; every wait bounded; host test end to end on the model.
  * 78-4a.5 The Readme's rings and interrupts section and the `rach2/cmdline.md` row.
* Done: first light runs end to end on the model, and every interrupt source but the rings' is masked.

### 78-4b First light on the Lenovo

Builds: the first trial of new Intel code on the Lenovo, silent: forcewake, power wells, the GTT, stolen memory and one GPU write, with the firmware's display left as it was.

* Files: the record in `dev-log/78-intel/`; the model block that holds a difference from the hardware, after that block's owner.
* Waits on: 78-2b, 78-4a (code), 78-1b (evidence: the watchdog proven).
* Labels: `lenovo`.
* Subtasks:
  * 78-4b.1 The review note for `i915_uncore.c`, `i915_power.c`, `i915_gtt.c`, `i915_stolen.c`, `i915_ring.c` and `i915_irq.c`, and the manager's go.
  * 78-4b.2 Prepare: a kernel whose init arms the watchdog before i915 probes; first light by boot option; gfxtool's dump before and after; what passes (`i915: first light: ok`, the console unchanged on screen, no FIFO underrun); why it cannot hang the machine (bounded waits, the watchdog). Hand back `owner-only`.
  * 78-4b.3 The Lenovo step (Mac loop): one silent trial boot, logs and dumps kept; a difference from the model is fixed in this task and the model updated.
* Done: first light passes on the Lenovo with the firmware's display unchanged.

### 78-5a Contexts, requests and parameters

Builds: hardware contexts with their parameters, requests and their retirement, and the answers Crocus and hasvk read: GETPARAM, I915_QUERY and GET_RESET_STATS, from the table Linux answered on the Lenovo (`tonight/lenovo.md:63-65`), each row required or a probe that may fail. Split from 78-5 because contexts and parameters do not need EXECBUFFER2 and 78-6b, 79-6c and 78-5b start from them.

* Files: new `rach2/sys/drm/i915/i915_gem_context.c`, `i915_request.c`, `i915_getparam.c` (GETPARAM, I915_QUERY, GET_RESET_STATS); new `tests/host/i915_ctx_hosttest.c`; the man page's context section.
* Waits on: 78-3a, 78-4a (code), 76-3 (code: syncobjs).
* Labels: none.
* Subtasks:
  * 78-5a.1 Hardware contexts on the render ring: the image size from `CXT_SIZE`, `MI_SET_CONTEXT` on switch, the default context; host test.
  * 78-5a.2 CONTEXT_CREATE (with extensions), DESTROY, GETPARAM and SETPARAM for recoverable, priority, bannable, GTT_SIZE and NO_ZEROMAP, refused with Linux's errors where Linux refuses; host test.
  * 78-5a.3 Requests: creation, sequence numbers, retirement from the interrupt, waits with a timeout; host test.
  * 78-5a.4 GETPARAM and I915_QUERY from the table Linux answered (CHIPSET_ID 0x0a16, REVISION 11, HAS_GPU_RESET 2, ALIASING_PPGTT 1, ... `tonight/lenovo.md:63`), CMD_PARSER_VERSION left unanswered until 78-8b; host test row by row.
  * 78-5a.5 GET_RESET_STATS answers for every context from the start, with counts that 78-6a fills, since hasvk declares its device lost when the call fails (`vulkan_hasvk/anv_device.c:2875-2879`); host test.
* Done: every GETPARAM and I915_QUERY row Linux answered answers the same on the model, and GET_RESET_STATS answers for every context.

### 78-5b Submission

Builds: EXECBUFFER2 with relocations, fence arrays and in and out fences on the render and blitter rings; Crocus starts in QEMU on the fault backend through the real paths. Until 78-8 lands, a batch from a process without privilege goes to the parser's stub, which refuses it.

* Files: new `rach2/sys/drm/i915/i915_gem_execbuffer.c`; new stub `i915_cmd_parser.c` (`i915_cmd_parser_check` refuses every unprivileged batch) for 78-8b; new `tests/i915execprobe.c`; new `tests/host/i915_exec_hosttest.c`; the man page's submission section.
* Waits on: 78-5a (code), 78-7 (code: the fault backend), 76-3 (code).
* Labels: none.
* Subtasks:
  * 78-5b.1 Object lookup, binding through 78-3a's VMAs, relocations with presumed offsets, NO_RELOC, HANDLE_LUT and BATCH_FIRST; host test with Linux's relocation cases.
  * 78-5b.2 Batch start on the render and blitter rings, the request emitted, the out-fence; host test.
  * 78-5b.3 Fence arrays of syncobjs, an in-fence and an out-fence as sync files; host test.
  * 78-5b.4 Unprivileged batches through `i915_cmd_parser_check`, refused by the stub; host test.
  * 78-5b.5 `tests/i915execprobe.c` on the fault backend: Crocus initializes (`eglinfo -B -p surfaceless` names it), submissions complete, and injected faults (EIO, ENOSPC, a hung request) reach Crocus as Linux's errors; its author runs it once.
* Done: on the fault backend Crocus starts and its submissions complete through Rach2's EXECBUFFER2, and injected faults return Linux's errors.

### 78-5c Submission on the Lenovo

Builds: the first trial of submission on the Lenovo, as root and silent: IGT's gem basics, a working set beyond the aperture, Crocus off-screen.

* Files: new `tests/graphics/igt-gem-trial.txt` (the case list); the record in `dev-log/78-intel/`.
* Waits on: 78-3b, 78-5b (code), 76-13b (code: IGT's tests), 76-15b (code: the trial root's slot), 76-15c (evidence: the data partition on the Lenovo), 78-4b (evidence).
* Labels: `lenovo`.
* Subtasks:
  * 78-5c.1 The review note for the new submission and memory paths, and the manager's go.
  * 78-5c.2 A trial root with IGT's tests, piglit and Crocus from 76-13; the list: `gem_basic`, `gem_create`, `gem_close_race`, `gem_exec_basic`, `gem_exec_store`, `gem_mmap`, `gem_mmap_gtt`, `gem_tiled_blits`, `gem_ctx_param`, `gem_reset_stats`, `gem_busy`, `gem_wait`, `gem_userptr_blits` basic, `prime_self_import`; Crocus off-screen by `eglinfo` and a readback; Linux's results for the same cases from step 9. Hand back `owner-only`.
  * 78-5c.3 The Lenovo step (Mac loop): one silent trial boot as root, results compared by case.
* Done: each case of the list passes or fails as Linux's on the Lenovo, and Crocus draws right off-screen.

### 78-6a Hang detection and reset

Builds: hang detection, the reset of the hung engine with the whole-GPU reset as fallback, as Linux on Haswell (plan.md, 78's table, 78-6), the reset statistics, the guilty context banned and the others' work replayed.

* Files: new `rach2/sys/drm/i915/i915_hangcheck.c`, `i915_reset.c`, `i915_gpu_error.c` (the error state captured at a hang); `i915_gem_context.c` (ban and counts) after 78-5a; new `tests/host/i915_model_reset.c` (the model's reset); new `tests/host/i915_reset_hosttest.c`; new `tests/i915resetprobe.c`.
* Waits on: 78-5b (code).
* Labels: none.
* Subtasks:
  * 78-6a.1 Hang detection: a request whose sequence number and ACTHD do not move within Linux's period is hung; host test.
  * 78-6a.2 Engine reset through GDRST's engine bit with a bounded wait, the ring, status page and context restored; the whole-GPU reset when that fails; host test with a stuck reset.
  * 78-6a.3 The guilty context marked and banned after Linux's count, its requests failed with EIO, innocent requests replayed, GET_RESET_STATS counts as Linux's; host test.
  * 78-6a.4 The error state (rings, active requests, batch heads) captured at a hang for 78-13a's files.
  * 78-6a.5 `tests/i915resetprobe.c` on the fault backend: an injected hang, the reset, the ban and the replay; its author runs it once; the Readme's reset section.
* Done: on the fault backend an injected hang costs only the guilty context, its counts show it, and the other contexts' work completes.

### 78-6b RC6 and frequency

Builds: RC6 and frequency control with residency counters (Linux: 9960 ms of 10 s idle in RC6). Split from 78-6 because it needs only the ring and interrupts, not submission.

* Files: new `rach2/sys/drm/i915/i915_rc6.c`, `i915_rps.c`; the `/sys` files Linux has (`gt_cur_freq_mhz`, `gt_min_freq_mhz`, `gt_max_freq_mhz`, `gt_RP0_freq_mhz`, `gt_RP1_freq_mhz`, `gt_RPn_freq_mhz`, `power/rc6_residency_ms`) through 76-9's attribute interface; new `tests/host/i915_model_rc6.c` (the model's RC6 and pcode mailbox); new `tests/host/i915_power_hosttest.c`.
* Waits on: 78-4a (code), 76-9 (code: the `/sys` attribute interface).
* Labels: none.
* Subtasks:
  * 78-6b.1 RC6 with Linux's thresholds and the residency counter read in milliseconds; host test.
  * 78-6b.2 Frequency through the pcode mailbox (bounded) and RPNSWREQ, the up and down interrupts handled from 78-4a's table; host test.
  * 78-6b.3 The `/sys` files and their section in `man/man4/i915.4`.
  * 78-6b.4 The files read on the fault backend in QEMU by their author once.
* Done: on the model RC6 is entered at idle and frequency requests stay in the limits, and the files have Linux's names.

### 78-6c Reset, power and the parser on the Lenovo

Builds: a silent trial: piglit's two hang triggers, which hung the GPU 9 times under Linux, each recovered by an engine reset; RC6 residency at idle; the parser's rejection cases; ordinary users' submissions. Render and video hung at once is in 78-14b, since the video ring comes later.

* Files: the record in `dev-log/78-intel/`.
* Waits on: 78-6a, 78-6b, 78-8a, 78-8b (code), 78-5c (evidence).
* Labels: `lenovo`.
* Subtasks:
  * 78-6c.1 The review note for GDRST, the pcode mailbox and the RC6 registers, and the manager's go.
  * 78-6c.2 Prepare: piglit's multisample `read-front` tests and `arb_copy_image-formats --samples=2` with the reference's commands, other programs running beside; `gem_reset_stats`, `gem_exec_parse` as an ordinary user, an ordinary user's Crocus readback; a 10 s idle residency read; what passes is each hang recovered with only the guilty context lost, as Linux. Hand back `owner-only`.
  * 78-6c.3 The Lenovo step (Mac loop), silent: a trial boot of its own, since the hangs take the GPU; `mixer vol 1` after the boot.
* Done: each hang recovers as on Linux, RC6 residency rises at idle, and every parser rejection case matches Linux's.

### 78-7 ABI and fault backend

Builds: a test-only backend behind `i915_hw.h` that completes every batch at once and injects faults, so Crocus starts in QEMU through Rach2's real paths; it proves the interfaces and draws nothing (the plan's decision 12).

* Files: new `rach2/sys/drm/i915/i915_fake.c`; its binding under `rach2.i915fake` (a `rach2/cmdline.md` row); new `tests/host/i915_fake_hosttest.c`; new `tests/cases/machine.i915fake.json` and a line in `tests/registry`; the Readme's tests section.
* Waits on: 78-2a (code: `i915_hw.h`), 76-4 (code), 76-9 (code: a DRM device's PCI identity in `/sys`, which Mesa reads; Problems 8).
* Labels: none.
* Subtasks:
  * 78-7.1 The fake chip: a register file, a GTT, rings whose head reaches the tail at once, sequence numbers written and interrupts raised; host test.
  * 78-7.2 Binding: under `rach2.i915fake`, the driver binds to a QEMU PCI device chosen for it and reports 8086:0A16 in `/sys` through 76-9, so that libdrm's `drmGetDevice2` and Mesa's loader see Haswell.
  * 78-7.3 Fault injection through debugfs: EIO on submit, a hang, ENOSPC on bind, a lost interrupt.
  * 78-7.4 A `machine.i915fake` case boots with the backend and checks that Mesa's loader names `crocus`; its author runs it once.
* Done: on QEMU with `rach2.i915fake`, libdrm reports 8086:0a16 and Mesa's loader picks Crocus.

### 78-8a Aliasing PPGTT

Builds: the aliasing PPGTT, as Linux on Haswell: one PPGTT shared by every context, mirroring the GGTT's bindings. Split from 78-8 because it and the parser touch different files and can run at once.

* Files: `rach2/sys/drm/i915/i915_ppgtt.c` (from 78-4a's stub); `tests/host/i915_model_ring.c` (the PPGTT registers, after 78-4a); new `tests/host/i915_ppgtt_hosttest.c`.
* Waits on: 78-5b (code).
* Labels: none.
* Subtasks:
  * 78-8a.1 The page directory in the GGTT and its tables; bindings mirrored into it.
  * 78-8a.2 `PP_DIR_DCLV` and `PP_DIR_BASE` for each ring at start and after a reset, through `i915_ppgtt_enable_ring`; `GFX_MODE`'s PPGTT bit.
  * 78-8a.3 Host test: a batch's addresses resolve through the PPGTT to the same pages as the GGTT's.
  * 78-8a.4 The Readme states that all programs share one GPU address space on Haswell, as on Linux (the plan's decision 4).
* Done: on the model every ring runs with the aliasing PPGTT enabled, after a reset too.

### 78-8b Command parser

Builds: Linux's Haswell command parser for the render and blitter rings, after which ordinary users submit; its version answered through GETPARAM, without which hasvk refuses Haswell (`anv_device.c:1664-1671`).

* Files: `rach2/sys/drm/i915/i915_cmd_parser.c` (from 78-5b's stub); `i915_getparam.c` (CMD_PARSER_VERSION 10, Linux 6.12's `i915_cmd_parser.c:1623`) after 78-5a; new `tests/host/i915_parser_hosttest.c`; the man page's parser section.
* Waits on: 78-5b (code).
* Labels: none.
* Subtasks:
  * 78-8b.1 Linux 6.12's command tables and register allowlists for the render and blitter rings on Haswell.
  * 78-8b.2 The parse: the batch copied to a shadow, chained batches followed as Linux, privileged commands and registers refused.
  * 78-8b.3 GETPARAM CMD_PARSER_VERSION.
  * 78-8b.4 Host test with every `gem_exec_parse` case Linux rejects and accepts.
* Done: the host test rejects every case Linux rejects and accepts the rest, and GETPARAM reports version 10.

### 78-9 Display pipe

Builds: readout and adoption of the firmware's display state, atomic planes with X-tiling and the formats Haswell scans out (the primary plane takes XRGB8888 and no ARGB8888), vblank and flip interrupts, cursor, backlight, and the pipe CRC through debugfs.

* Files: new `rach2/sys/drm/i915/intel_display.c`, `intel_plane.c`, `intel_cursor.c`, `intel_vblank.c`, `intel_backlight.c`, `i915_display_debugfs.c`; `rach2/sys/display/intelbl.c` (its PWM code moves behind the connector, one owner of the PWM) and `man/man4/backlight.4`; new `tests/host/i915_model_pipe.c` (the model's pipe, planes and CRC); new `tests/host/i915_display_hosttest.c`.
* Waits on: 78-3a (code: framebuffers are GEM objects), 78-4a (code: the interrupts), 76-5 (code: atomic KMS), 76-11a (code: the console on DRM), 76-18 (code: the CRC files). It does not wait on 78-1b, a Lenovo task; its host test starts from Linux's readout and takes 78-1b's dump when that is in the tree.
* Labels: none.
* Subtasks:
  * 78-9.1 Readout of pipe A, transcoder EDP, DDI A, plane A (surface, stride, format, tiling), the fitter and the PPS into atomic state; host test seeded from Linux's `i915_display_info` and panel timings (`tonight/lenovo-out/debugfs/`), and from 78-1b's dump once it is committed.
  * 78-9.2 Adoption: the first commit keeps the mode, takes over the framebuffer, and 76-11's console moves without a blank; host test.
  * 78-9.3 Planes: the primary's formats as Linux lists them for Haswell (no ARGB8888), the sprite plane, linear and X-tiling, stride and offset checks; host test.
  * 78-9.4 Vblank and flip-done on each pipe, the frame counter, page flips at vblank with events and monotonic stamps; host test.
  * 78-9.5 The cursor plane and the backlight through the connector and `/sys/class/backlight/intel_backlight`; `intelbl.c` stands down when i915 binds; `backlight.4` updated.
  * 78-9.6 The pipe CRC through 76-18's files; the Readme and man page's display sections; on the fault backend, modetest lists the connector and the plane formats.
* Done: on the model the firmware's state is adopted without a mode set, and flips, vblank events, the cursor and the CRC work.

### 78-10a VBT and OpRegion

Builds: the VBT and OpRegion decode from the Lenovo's saved bytes (`run2/step11/facts/`). Split from 78-10 because it needs only the skeleton, starts early, and the outputs that follow are too large for one task (Problems 18).

* Files: new `rach2/sys/drm/i915/intel_bios.c`, `intel_opregion.c`; new `tests/host/intel_bios_hosttest.c`; `tests/host/i915_lenovo.h` (the VBT and OpRegion, after 78-2a); it reads the fixtures `tests/fixtures/lenovo-gpu/i915_vbt.bin` (6,144 bytes) and `i915_opregion.bin` (8,192 bytes), which the manager commits when the branch is cut (decision H's recommendation).
* Waits on: 78-2a (code).
* Labels: none.
* Subtasks:
  * 78-10a.1 The OpRegion found through ASLS, its header and mailboxes checked, a bad signature refused.
  * 78-10a.2 The VBT's blocks as Linux 6.12's `intel_bios.c`: general definitions, child devices and ports, LFP data and power timings, eDP, driver features; each block's size checked.
  * 78-10a.3 Linux's defaults without a VBT (eDP on DDI A).
  * 78-10a.4 Host test: the Lenovo's bytes decode to the ports, panel timings and eDP settings Linux reported (`tonight/lenovo-out/debugfs/`, `i915_vbt` and `eDP-1-i915_panel_timings`); the Readme's section.
* Done: the host test decodes the Lenovo's VBT to what Linux reported.

### 78-10b eDP and the panel

Builds: a full mode set of the panel: DDI, eDP with power sequencing, AUX and link training (1 lane at 2.7 Gb/s, 6 bits per colour), the panel fitter, CDCLK (450 MHz) and the watermarks of one pipe.

* Files: new `rach2/sys/drm/i915/intel_ddi.c`, `intel_dp.c`, `intel_dp_aux.c`, `intel_dp_link_training.c`, `intel_pps.c`, `intel_panel.c`, `intel_pfit.c`, `intel_cdclk.c`, `intel_wm.c`; new `tests/host/i915_model_sink.c` (the model's AUX sink and panel); new `tests/host/i915_edp_hosttest.c`.
* Waits on: 78-9, 78-10a (code).
* Labels: none.
* Subtasks:
  * 78-10b.1 AUX with bounded waits and retries; DPCD read; host test against a sink model.
  * 78-10b.2 Link training, clock recovery and channel equalization as Linux's; host test reaching 1 lane at 2.7 Gb/s.
  * 78-10b.3 The panel power sequencer with the VBT's timings; host test of the on and off order.
  * 78-10b.4 A full disable and enable of pipe A on eDP in Linux's register order; the fitter for the modes the connector lists; host test.
  * 78-10b.5 CDCLK readout and its rules; the watermarks of one pipe; the Readme's outputs section.
* Done: on the model a full mode set of the panel follows Linux's register sequence and link training reaches 1 lane at 2.7 Gb/s.

### 78-10c External outputs

Builds: HDMI and DisplayPort monitors: GMBUS with its pin map and the bit-banging fallback for an HDMI monitor's EDID, HDMI with infoframes, the PLLs, hotplug, the second pipe's power well and the watermarks of two pipes.

* Files: new `rach2/sys/drm/i915/intel_gmbus.c`, `intel_hdmi.c`, `intel_dpll.c`, `intel_hotplug.c`; `intel_dp.c` and `intel_wm.c` after 78-10b; `i915_power.c` after 78-2b; new `tests/host/i915_model_ddi.c` (the model's GMBUS, HDMI and PLL registers); new `tests/host/i915_hdmi_hosttest.c`.
* Waits on: 78-10b (code), 76-18 (code: EDID override and connector forcing).
* Labels: none.
* Subtasks:
  * 78-10c.1 GMBUS with the VBT's pin map and the bit-banging fallback; host test reading an EDID from a model.
  * 78-10c.2 HDMI on DDI B, C and D as the VBT says, with AVI and SPD infoframes; host test.
  * 78-10c.3 WRPLL and SPLL computed as Linux's; host test with a table of clocks against Linux's values.
  * 78-10c.4 External DisplayPort on the DDI ports through `intel_dp.c`.
  * 78-10c.5 Hotplug interrupts (long and short pulses, storm detection) and the second pipe's power well and watermarks; host test; IGT's `kms_hdmi_inject` path through 76-18's EDID override.
* Done: on the model an injected HDMI EDID's modes are set with Linux's PLL values, and hotplug reports connect and disconnect.

### 78-10d Display on the Lenovo

Builds: the display trial, which takes the screen: adoption at boot without a blank, the panel at each mode with pipe CRCs matching Linux's for one pattern, the fitter, the backlight, HDMI by IGT's EDID injection, hotplug.

* Files: new `tests/graphics/ref/lenovo-crc.txt` (Linux's CRCs); the record in `dev-log/78-intel/`.
* Waits on: 78-10b, 78-10c (code), 78-4b (evidence).
* Labels: `lenovo`.
* Subtasks:
  * 78-10d.1 The review note for the display, DDI, AUX, PPS and PLL registers, and the manager's go.
  * 78-10d.2 Prepare the Debian part: IGT's CRC of one pattern at each mode on Linux, kept as the reference; a Debian boot of its own, since the CRCs take the screen.
  * 78-10d.3 Prepare the Rach2 part: modetest at each mode with the CRC read; the fitter; backlight levels; `kms_hdmi_inject`; hotplug; IGT's KMS subset (`kms_addfb_basic`, `kms_atomic`, `kms_flip` basic, `kms_pipe_crc_basic`, `kms_cursor_crc` one size); a capture at adoption. Hand back `owner-only` with a "Lenovo notice:".
  * 78-10d.4 The Lenovo step (Mac loop): the Debian boot, then the Rach2 trial boot, `mixer vol 1` after each; a difference from Linux is fixed in this task.
* Done: the panel at each mode with CRCs matching Linux's, adoption without a blank, HDMI by injection and hotplug seen.

### 78-11a hasvk kernel pieces

Builds: the kernel pieces hasvk probes, from the vkcube trace under Linux (`tonight/lenovo.md:65`): timeline syncobj waits and transfers in EXECBUFFER2's extension, sync-file import and export on dma-bufs, `I915_REG_READ` of the timestamp, reset statistics each frame. Split from its Lenovo step (78-11c), since 78-11b, 80-5 and 83-2b need the code.

* Files: `rach2/sys/drm/i915/i915_gem_execbuffer.c` (the timeline fences extension) after 78-5b; new `i915_reg_read.c` (`I915_REG_READ` of the timestamp); `tests/host/i915_exec_hosttest.c` (timeline rows, after 78-5b). The parameters hasvk reads are already in 78-5a's table.
* Waits on: 78-5b, 78-8b (code: hasvk needs the parser's version), 76-3 (code: timeline syncobjs and sync-file export).
* Labels: none.
* Subtasks:
  * 78-11a.1 The timeline fences extension of EXECBUFFER2; host test.
  * 78-11a.2 The trace's ioctls checked one by one against Rach2, and `I915_REG_READ` for the timestamp; host test.
* Done: the host tests pass for every ioctl of the trace.

### 78-11c hasvk off-screen on the Lenovo

Builds: vulkaninfo and dEQP-VK's smoke set off-screen on the Lenovo, as root on a silent trial boot.

* Files: new `tests/graphics/deqp-vk-smoke.txt`; the record in `dev-log/78-intel/`.
* Waits on: 78-11a (code), 77-9 (code: VK-GL-CTS), 78-5c (`evidence`: submission proven on the Lenovo).
* Labels: `lenovo`.
* Subtasks:
  * 78-11c.1 The review note for the timeline path and `I915_REG_READ`, and the manager's go.
  * 78-11c.2 Prepare: `vulkaninfo`, dEQP-VK's smoke set and every 500th case of `vk-default` (Linux: 936 pass, 1 fail, 1 resource error of 6,431, `run2/step9/deqp/run.log`), compared by case. Hand back `owner-only`.
  * 78-11c.3 The Lenovo step (Mac loop): a silent trial boot of its own; `mixer vol 1` after the boot.
* Done: vulkaninfo matches Linux's on the Lenovo, and the smoke set and every 500th case match Linux's by case.

### 78-11b hasvk presenting and WebGPU

Builds: vkcube presenting, Chromium's WebGPU on hasvk with `--ignore-gpu-blocklist` from the test launcher (decision C), compute and render read back, and a WebGPU picture on the panel captured (59.9 FPS under Linux), its sandbox state recorded; hasvk at its default 1.2, with the 1.3 option tried only per program.

* Files: the launcher's switches in 76-17a's file, after 78-12a; the record in `dev-log/78-intel/`.
* Waits on: 78-11a, 78-12a (code: the browsers on the GPU and the launcher's file), 76-14 (code), 78-11c, 78-10d (`evidence`).
* Labels: `lenovo`.
* Subtasks:
  * 78-11b.1 Prepare vkcube under Weston, captured and its rate recorded beside Linux's under the same settings.
  * 78-11b.2 Prepare Chromium's WebGPU on hasvk: adapter named, compute and render read back, a picture captured, the sandbox state read from Chromium's diagnostics.
  * 78-11b.3 Hand back `owner-only` with a "Lenovo notice:"; a trial boot of its own, since it takes the screen; `mixer vol 1` after the boot.
  * 78-11b.4 The Lenovo step (Mac loop).
* Done: vkcube presents and WebGPU computes and draws right on hasvk with its adapter named, as on Linux.

### 78-12a Accelerated browsers: the launcher and the fixes

Builds: the launcher's GPU profile for Chromium's GPU process and Firefox's WebRender on Crocus, each process sandboxed as Debian's (decision D's recommendation), or, if the owner takes 76-20, Chromium's GPU process sandboxed on Crocus with WebGL and WebGPU working; the check scripts of the Lenovo step; and the kernel fixes the browsers need, each found and checked on the fault backend in QEMU. Split from its Lenovo step (78-12b), since 78-11b, 78-14b, 80-7b, 80-9 and 83-2b need the code.

* Files: the launcher's GPU profile in 76-17a's file, after 76-17a; new `tests/graphics/lenovo/browsers-gpu.sh` (the step's checks); kernel fixes as subtasks.
* Waits on: 78-8a, 78-8b, 78-9 (code), 76-14, 76-17a (code: the session and the browser launcher).
* Labels: none.
* Subtasks:
  * 78-12a.1 The launcher's GPU profile: Chromium's GPU process on Crocus with Debian's sandbox switches, Firefox with WebRender's hardware path. Check: on the fault backend in QEMU, Chromium's GPU process starts and names Crocus and Firefox's `about:support` names it; its author runs it once.
  * 78-12a.2 `browsers-gpu.sh`: WebGL's unmasked renderer, each process's sandbox from its diagnostics beside Debian's on the same binaries, `about:support`, an X11 GL program through Xwayland and a Direct3D 9 program through Wine on WineD3D's GL renderer, each read back. Check: its author runs it once on the fault backend, where the readbacks are not judged.
  * 78-12a.3 Each kernel fix the browsers need on the fault backend, a subtask filed as found.
* Done: on the fault backend both browsers start their GPU paths on Crocus with Debian's sandboxes, and the step's script runs end to end.

### 78-12b Accelerated browsers on the Lenovo

Builds: both browsers on Crocus on the Lenovo with each process's sandbox compared with Debian's, and the X11 GL and Direct3D 9 checks, on a trial boot that takes the screen.

* Files: the record in `dev-log/78-intel/`.
* Waits on: 78-12a (code), 78-6c (`evidence`: ordinary users' submissions proven on the Lenovo), 78-10d (`evidence`: the display).
* Labels: `lenovo`.
* Subtasks:
  * 78-12b.1 Prepare Chromium on Crocus: WebGL's unmasked renderer names Crocus, each process's sandbox read from its diagnostics and compared with Debian's on the same binaries.
  * 78-12b.2 Prepare Firefox on WebRender with Crocus, read from `about:support`.
  * 78-12b.3 Prepare an X11 GL program through Xwayland and a Direct3D 9 program through Wine on WineD3D's GL renderer, each read back.
  * 78-12b.4 Hand back `owner-only` with a "Lenovo notice:"; the Lenovo step (Mac loop), a trial boot of its own; `mixer vol 1` after the boot.
* Done: both browsers render on Crocus with each process sandboxed as Debian's, and the X11 GL and Direct3D 9 checks draw right.

### 78-13a i915 debugfs

Builds: the i915 debugfs files IGT's gem tests open: drop caches, wedged, the error state and the information files.

* Files: new `rach2/sys/drm/i915/i915_debugfs.c`, through 76-18's debugfs; the man page's debugfs section.
* Waits on: 78-3b (code: drop caches calls the shrinker), 78-6a (code: wedged and the error state), 76-18 (code).
* Labels: none.
* Subtasks:
  * 78-13a.1 `i915_gem_drop_caches` with Linux's flags.
  * 78-13a.2 `i915_wedged`, read and write, through 78-6a's reset.
  * 78-13a.3 `i915_error_state` read and cleared, and `i915_gpu_info`.
  * 78-13a.4 `i915_capabilities`, `i915_engine_info`, `i915_gem_objects` and `i915_frequency_info` in Linux's formats as far as IGT parses them.
  * 78-13a.5 On the fault backend, IGT's `gem_basic` and `gem_reset_stats` open the files; its author runs them once.
* Done: IGT's gem tests find these files and parse what they read.

### 78-13b Conformance against Linux

Builds: the Lenovo's conformance runs against Linux on the same machine: IGT's gem and KMS tests, piglit, dEQP-GLES (VK-GL-CTS 1.4.7.1), the WebGL suite and glmark2, failures compared by case, every difference as Linux's or an exclusion the owner approves; the speed gate against translation-off Linux (change 2, decision E). The whole comparison runs at the release's Lenovo step; this task builds it and proves each runner there.

* Files: new `tests/native/gpu.cases` and its name in `tools/etude/native_prepare.c:254`; new `tests/graphics/ref/lenovo/` (Linux's per-case results from `run2/step9/`); the exclusions list for the owner; the gate's settings.
* Waits on: 78-13a, 77-9, 76-22 (code). Its preparation does not wait on the Lenovo; its step runs last in the queue, after every other step of 78.
* Labels: `lenovo`.
* Subtasks:
  * 78-13b.1 The native case file with IGT gem and KMS, piglit `quick`, dEQP-GLES2, GLES3 and GLES31, the WebGL suite and glmark2 under Weston, each through its runner.
  * 78-13b.2 Linux's results turned into per-case expectations: piglit (`run2/step9/piglit/`), dEQP (`run2/step9/deqp/`), IGT (`run2/step9/igt/`), the WebGL suite.
  * 78-13b.3 The gate: glmark2 under Weston against translation-off Linux (1387 GL, 1379 GLES), the WebGL fill and ALU checks; the replays are 80's.
  * 78-13b.4 Prepare the Debian part: the WebGL suite retaken on an idle machine, as 78-13's row in plan.md's table requires, in a Debian boot of its own.
  * 78-13b.5 Prepare the Rach2 part: each runner on a small part, whole runs left to the release. Hand back `owner-only` with a "Lenovo notice:".
  * 78-13b.6 The Lenovo step (Mac loop): the Debian boot, then the Rach2 trial boot, `mixer vol 1` after each; each difference found goes on the exclusions list or is fixed in this task.
* Done: each runner judges by case on the Lenovo against Linux's stored results, and the exclusions list is with the owner.

### 78-14a Video ring and VA driver

Builds: the video ring and the video enhancement ring with their parser tables, and Debian's i965 VA driver (MPEG-2, H.264, VC-1 and JPEG on Haswell) with libva and vainfo in the root, which 76-13's list lacks (Problems 13).

* Files: `rach2/sys/drm/i915/i915_ring.c` (VCS and VECS) after 78-4a; `i915_cmd_parser.c` (their tables) and `i915_getparam.c` (HAS_BSD, HAS_VEBOX) after 78-8b; `vendor/gnu/debian/va.list` (new, its own list: `libva2`, `libva-drm2`, `i965-va-driver` 2.4.1, `vainfo`); the man page's video section.
* Waits on: 78-5b, 78-8b (code: the file order above), 76-13 (code); the downloads card (`decision`: the VA packages).
* Labels: `linux-host`.
* Subtasks:
  * 78-14a.1 VCS and VECS rings with their interrupts registered in 78-4a's table; host test.
  * 78-14a.2 Linux 6.12's parser tables for both; host test with their rejection cases.
  * 78-14a.3 GETPARAM rows for the new rings.
  * 78-14a.4 The VA packages pinned and installed, once the downloads card is answered.
  * 78-14a.5 On the fault backend, `vainfo` lists Haswell's decode profiles; its author runs it once.
* Done: on the fault backend vainfo lists Haswell's profiles through the i965 driver.

### 78-14b Video on the Lenovo

Builds: decoding on the GPU checked by frame hashes against software (equal under Linux); Firefox's hardware decoding confirmed by its own log; Chromium's as decision B's recommendation says; drop counts against Debian's for each browser with each decoder named; render and video work hung at once and recovered as on Linux (plan.md, 78's table, 78-6).

* Files: new `tests/graphics/ref/lenovo-video/` (Debian's drop counts and hashes); the record in `dev-log/78-intel/`.
* Waits on: 78-14a, 78-12a (code), 76-17a (code: the video runner), 78-12b (`evidence`: the browsers on Crocus).
* Labels: `lenovo`.
* Subtasks:
  * 78-14b.1 The review note for the VCS and VECS rings and their parser tables, and the manager's go.
  * 78-14b.2 Prepare frame hashes of H.264, MPEG-2, VC-1 and JPEG clips decoded through VA against a software decode.
  * 78-14b.3 Prepare Firefox and Chromium playback at 720p and 1080p with drops counted and each decoder read by name from the browser's log.
  * 78-14b.4 Prepare a hang on the render ring while the video ring decodes, recovered as on Linux.
  * 78-14b.5 Hand back `owner-only` with a "Lenovo notice:" (sound at `mixer vol 1`, the screen); the Lenovo step (Mac loop), a trial boot of its own; `mixer vol 1` after the boot.
* Done: decoded frames hash equal to software, drops are no worse than Debian's with each decoder named, and a hang during decoding recovers as on Linux.

## 79 ia32: 32-bit programs on the 64-bit kernel

### 79-1a Compat entry, segments and TLS

Builds: what Wine's WoW64 needs first: the 32-bit user code segment, the GDT's TLS entries reloaded on every switch, an `int $0x80` gate into a compat table with i386 numbers and argument registers, `set_thread_area` through it, signal return that restores 32-bit segments, seccomp and ptrace that tell 32-bit entries apart, and `in_compat_syscall`. Split from 79-1 because its other two parts edit `syscall_proc.c` after 76-2b (Problems 11), and this part waits on nothing.

* Files: in `rach2/sys/arch/x86_64/`: `percpu.h` (Linux's x86-64 GDT: the 32-bit user code segment at 0x23, the TLS entries 12 to 14, the LDT's two entries reserved for 79-3, `GDT_ENTRIES` 16), `percpu.c`, `idt.c` (gate 0x80 at DPL 3), `isr_handler.s`, `cpu.h` (`SC_NR` and `SC_ARG` for a compat entry), `context.c`, `tls.c`, `sigframe.c`, `ptrace.c`; `rach2/sys/syscall.c` (`_syscall_handler` picks the table); `rach2/sys/proc/seccomp.c` and `rach2/sys/proc/ptrace.c`; new `ia32.h`, `ia32_systab.c`; new stubs `ia32_exec.c`, `vdso32.c`, `vdso32code.s`, `ldt.c`, `sigframe32.c`, `ia32_signal.c`, `ia32_fs.c`, `ia32_time.c`, `ia32_ioctl.c`, `ia32_ioctl_drm.c`, `ia32_ioctl_gpu.c`, `ia32_net.c`, `ia32_ipc.c`, `ia32_misc.c`, all in `rach2/sys/arch/x86_64/sources.mk`; `tools/verify/mancheck.c` and the `syscalls` target of `Makefile` (the compat column); `rach2/Readme.md` (the system call table paragraph, line 60); `man/man2/syscalls.2`, `man/man2/set_thread_area.2`, new `man/man7/ia32.7`; new `tests/wow64probe.c`, built for x86-64 alone through a new `X86_64_ONLY` list beside `I686_ONLY` in `tests/Makefile`.
* Waits on: nothing.
* Labels: none.
* Subtasks:
  * 79-1a.1 The GDT in Linux's x86-64 layout, with the comment at `percpu.h:19-27` rewritten; a far call to 0x23 and back works.
  * 79-1a.2 `int $0x80`: the gate, an entry that saves the frame, and `_syscall_handler` taking the number from eax and the arguments from ebx, ecx, edx, esi, edi and ebp, zero-extended, into `ia32_systab.c`; a 64-bit program's `int $0x80` uses the compat table, as on Linux; `in_compat_syscall` true for that call; the restart and ptrace stops of `syscall.c:2777-2843` keep working for it.
  * 79-1a.3 `ia32_systab.c` with every row of Linux 6.12's `syscall_32.tbl`, each pointing at the native handler where the layouts are equal, at a compat handler in its owner's stub (returning -ENOSYS until that task), or empty; `make syscalls` writes the compat column of `rach2/syscalls.md`, and mancheck reads the compat table.
  * 79-1a.4 `set_thread_area` and `get_thread_area` through `int $0x80` with Linux's x86-64 entry numbers (12 to 14, -1 picks a free one), the three descriptors per thread loaded into the GDT on every switch in `context.c`; `set_thread_area.2` says so.
  * 79-1a.5 `rt_sigreturn` restores CS 0x23 and the saved SS under Linux's `UC_SIGCONTEXT_SS` and `UC_STRICT_RESTORE_SS` rules, so a 64-bit handler of a signal taken in 32-bit code returns there.
  * 79-1a.6 seccomp sees `AUDIT_ARCH_I386` and the i386 number for a compat entry (today's constant is at `seccomp.c:328,372`), and ptrace's system call information reports the architecture.
  * 79-1a.7 `tests/wow64probe.c`: a far call into 32-bit code, `set_thread_area` and a load of its selector, `getpid` through `int $0x80`, a signal taken in 32-bit code and returned there, a seccomp filter that kills only the i386 number; in `tests/default-probes` and `tests/cases/guest.default.json`; its author runs it once; `ia32.7` and `syscalls.2`.
* Done: wow64probe passes on the x86-64 kernel, and every i386 number has a row in the compat table.

### 79-1b futex_waitv and memory below 4 GiB

Builds: `futex_waitv` in both ABIs, without which Wine turns Proton's fsync off (plan.md, The research), and memory below 4 GiB on request: `MAP_32BIT` and hints below 4 GiB honoured, for WoW64's 32-bit pointers.

* Files: `rach2/sys/proc/futex.c`; `rach2/sys/arch/i686/systab.c`, `rach2/sys/arch/x86_64/systab.c` and both `sysnr.h` (449); row 449 of `ia32_systab.c` (the native handler: the structure is the same in both ABIs); `rach2/sys/syscall_proc.c` (`MAP_32BIT` in `mmap_place`, `:6267`); `man/man2/futex.2`, `man/aliases`, `man/man2/brk.2` (the mmap section); `tests/wow64probe.c` after 79-1a; new `tests/futexvprobe.c` for both architectures.
* Waits on: 79-1a (code: the compat table and the probe), 76-2b (code: the order on `syscall_proc.c`). 76-19's hunks (madvise near :6765, `sched_set` near :7250) are far from `mmap_place` (:6267), and the system call tables are row files.
* Labels: none.
* Subtasks:
  * 79-1b.1 `futex_waitv`: up to 128 waiters, private and shared futexes, an absolute timeout on the clock its flag names, a wake on any; EAGAIN on a changed value as Linux.
  * 79-1b.2 Its entry in both native tables and the compat row.
  * 79-1b.3 `MAP_32BIT` places a mapping in the first 2 GiB, as Linux; a hint below 4 GiB is taken when free.
  * 79-1b.4 `tests/futexvprobe.c` (both architectures, and through `int $0x80` in wow64probe) and wow64probe rows for `MAP_32BIT`; their author runs them once.
  * 79-1b.5 `futex.2` with its alias, and the mmap section of `brk.2`.
* Done: futexvprobe passes on both kernels and through `int $0x80`, and `MAP_32BIT` returns addresses below 2 GiB.

### 79-2 Coverage list

Builds: every i386 system call, structure and ioctl family, with Rach2's state, its owning task and its test (plan.md, 79's table, 79-2).

* Files: new `rach2/ia32.md`.
* Waits on: nothing.
* Labels: none.
* Subtasks:
  * 79-2.1 A row per call of Linux 6.12's `syscall_32.tbl`, marked equal in layout, translated (with Linux's `compat_sys_` or `ia32_sys_` name) or absent from Rach2.
  * 79-2.2 A row per structure that differs: `stat64`, the 32-bit `timespec` and `timeval`, `iovec`, `msghdr` and `cmsghdr`, `sigaction`, `siginfo`, `stack_t`, the `ipc64` structures, `rlimit`, `rusage`, `sysinfo`, `flock`, `statfs64`, `linux_dirent`, `input_event`, `ifreq`, `loop_info`, `fb_fix_screeninfo`.
  * 79-2.3 A row per ioctl family Rach2 answers: DRM core, i915, virtio-gpu, dma-buf, syncobj, sync_file, udmabuf, evdev, terminals, OSS, loop, fb, sockets and network devices, watchdog, rfkill; translation needed or none.
  * 79-2.4 An owner (79-1a to 79-7b) and a test for each row; 76-19's calls carried to 79-6a and 79-7b; the state column for calls refers to `rach2/syscalls.md`'s generated column, so it cannot drift.
* Done: every i386 call, structure and ioctl family has a row with its owner and its test.

### 79-3 Entry

Builds: `sysenter` and AMD's 32-bit `syscall`, entered through the vDSO's `__kernel_vsyscall` as Linux does; `modify_ldt` and the LDT loaded on every switch.

* Files: `rach2/sys/arch/x86_64/isr_handler.s`, `percpu.c` (the SYSENTER MSRs and IA32_CSTAR), `percpu.h` (the LDT's descriptor), `context.c`, `ldt.c` (from 79-1a's stub), `vdso32code.s` after 79-4 (`__kernel_vsyscall` by `sysenter` on Intel and `syscall` on AMD, with Linux's landing pad); row 123 of `ia32_systab.c`; new `man/man2/modify_ldt.2`; `man/man7/ia32.7`; new `tests/ldtprobe.c`, built for i686.
* Waits on: 79-4 (code: the vDSO whose entry it changes). The plan's 79-6 and 79-7 do not wait on it (Problems 20).
* Labels: none.
* Subtasks:
  * 79-3.1 The `sysenter` entry: the MSRs on each processor, the stack switch, the return as Linux's 64-bit kernel returns from it.
  * 79-3.2 AMD's 32-bit `syscall` through IA32_CSTAR, returning by `sysretl` under Linux's checks.
  * 79-3.3 `__kernel_vsyscall` chooses the entry by processor vendor, as Linux's.
  * 79-3.4 `modify_ldt` (read, write, the old write, read_default), the LDT loaded on switch, inherited on fork, dropped on exec and exit.
  * 79-3.5 `tests/ldtprobe.c` (i686): each `modify_ldt` operation, a far call through an LDT code segment, a call through `AT_SYSINFO`; its author runs it on the x86-64 kernel once; `modify_ldt.2`.
* Done: i386 programs enter by `sysenter` or `syscall` through the vDSO, and `modify_ldt`'s segments hold across switches and fork.

### 79-4 Process

Builds: 32-bit ELF loading with the stack and auxv of an i386 process, the address space below 4 GiB, a 32-bit vDSO, compat `execve` and `clone`, the old and new mmap calls, and 32-bit core dumps.

* Files: `rach2/sys/proc/elf.c` (ELFCLASS32 and EM_386 beside the native class, `elf.c:45,61,118,122`); `rach2/sys/proc/uimage.c` (the 32-bit stack and auxv); `rach2/sys/syscall_proc.c` (exec's layout at `:1631-1632` and the 4 GiB limit in `mmap_place`); `rach2/sys/proc/coredump.c`; `ia32_exec.c`, `vdso32.c`, `vdso32code.s` (from 79-1a's stubs) and `rach2/sys/arch/x86_64/vdso.c` (it builds both images at boot); `man/man2/execve.2`, `man/man7/vdso.7`, `man/man7/ia32.7`.
* Waits on: 79-1a (code), 79-1b (code: the order on `syscall_proc.c`).
* Labels: none.
* Subtasks:
  * 79-4.1 ELF32 on x86-64: class and machine accepted per binary, the interpreter (`/lib/ld-linux.so.2`, `/lib/ld-musl-i386.so.1`) loaded as ELF32, the process marked compat.
  * 79-4.2 The layout of an i386 process on Linux's 64-bit kernel: stack top 0xFFFFE000, the mmap base and every placement below 4 GiB, brk; an auxv of 4-byte words with `AT_PLATFORM` "i686", `AT_HWCAP` from CPUID, `AT_SYSINFO` and `AT_SYSINFO_EHDR`.
  * 79-4.3 Compat `execve` and `execveat` with 32-bit argument and environment pointers; `clone` with i386's argument order and `CLONE_SETTLS` as a `user_desc`; `fork` and `vfork`; an x86-64 program running an i386 one and back.
  * 79-4.4 The old `mmap` with its argument block (90), `mmap2` in pages (192), `munmap`, `mremap` and `mprotect` held below 4 GiB.
  * 79-4.5 A 32-bit vDSO built at boot like the 64-bit one (`rach2/sys/arch/x86_64/vdso.c`): the time calls from the same data page, `__kernel_sigreturn` and `__kernel_rt_sigreturn`, and `__kernel_vsyscall` by `int $0x80` until 79-3; `ia32_vdso_sigreturn` for 79-5; `vdso.7` says the i686 kernel has none (`rach2/sys/arch/i686/vdso.c:2`) and the x86-64 kernel gives one to i386 programs.
  * 79-4.6 ELF32 core files with i386 `prstatus`, FPU registers and the auxv note as Linux writes them; one read back with `readelf` on the host.
  * 79-4.7 The i686 builds of `auxvprobe`, `vmprobe`, `forktest` and `exitprobe` booted once on the x86-64 kernel by their author, as the new loader's first run, behaviour that reading the code cannot settle, which the Testing section allows; the man pages.
* Done: i686 programs from the test image run on the x86-64 kernel with the auxv and vDSO an i386 process gets on Linux, and their core files read back with i386 registers.

### 79-5 Signals and tracing

Builds: 32-bit signal frames with FPU state, siginfo, sigaltstack and both sigreturns; and ptrace of 32-bit tracees by 32-bit and 64-bit tracers, which shares the FPU layouts.

* Files: `rach2/sys/arch/x86_64/sigframe32.c`, `ia32_signal.c` (from 79-1a's stubs); `sigframe.c` (dispatch by the process's ABI) and `ptrace.c` (the i386 `user_regs_struct`, FPU and FXSR registers, `PTRACE_GET_THREAD_AREA`, `PEEKUSR` offsets) after 79-1a; `man/man2/sigaction.2`, `man/man2/ptrace.2`; new `tests/host/ia32_signal_hosttest.c`.
* Waits on: 79-1a (code).
* Labels: none.
* Subtasks:
  * 79-5.1 The i386 `sigframe` and `rt_sigframe` with `_fpstate_32` (the legacy header, FXSR, XSAVE); the restorer from `SA_RESTORER`, else 79-4's vDSO, else the frame's `retcode`, as Linux.
  * 79-5.2 Compat siginfo and the calls that pass it: `rt_sigqueueinfo`, `rt_tgsigqueueinfo`, `pidfd_send_signal`, `rt_sigtimedwait` in both time ABIs, `waitid`'s siginfo for 79-7b.
  * 79-5.3 The sigaction family (old `sigaction`, `rt_sigaction`, `signal`), the masks (`sigprocmask`, `sigpending`, `sigsuspend`, `rt_sigprocmask`), `sigaltstack` and `signalfd`.
  * 79-5.4 `sigreturn` and `rt_sigreturn` for 32-bit frames.
  * 79-5.5 ptrace of a 32-bit tracee by a 32-bit and by a 64-bit tracer.
  * 79-5.6 `ia32_signal_hosttest.c`: the 32-bit frames, compat siginfo and the i386 `user_regs_struct` checked against Linux 6.12's offsets; wow64probe rows for a signal on an alternate stack taken in 32-bit code, run once by its author; the man pages. The i686 builds of `sigprobe` and `xsaveprobe` run in 79-8c, since an i686 program needs 79-4's loader.
* Done: the layout host test and wow64probe's rows pass on the x86-64 kernel; 79-8c runs the i686 probes, a 32-bit tracer's read of a 32-bit tracee's registers among them.

### 79-6a Files and time

Builds: the 64-bit file calls, split 64-bit arguments, and both time ABIs (plan.md, 79's table, 79-6). Split from 79-6 because the ioctls (79-6b, 79-6c) wait on 76's and 78's drivers and these calls do not.

* Files: `rach2/sys/arch/x86_64/ia32_fs.c` and `ia32_time.c` (from 79-1a's stubs), reusing the logic of `rach2/sys/arch/i686/sys_ia32.c` without editing it; their rows of `ia32_systab.c`; new `tests/host/ia32_fs_hosttest.c`.
* Waits on: 79-1a (code).
* Labels: none.
* Subtasks:
  * 79-6a.1 The stat family (`stat64`, `lstat64`, `fstat64`, `fstatat64`, the old forms Linux keeps), `statfs`, `fstatfs` and their 64-bit forms.
  * 79-6a.2 Split 64-bit arguments: `truncate64`, `ftruncate64`, `pread64`, `pwrite64`, `fallocate`, `fadvise64`, `fadvise64_64`, `readahead`, `sync_file_range`, `_llseek`.
  * 79-6a.3 `getdents` with the 32-bit `linux_dirent`; `fcntl` and `fcntl64` with `flock` and `flock64`.
  * 79-6a.4 The iovec calls (`readv`, `writev`, `preadv`, `pwritev`, `preadv2`, `pwritev2`, `vmsplice`, and 76-19's `process_vm_readv`); `select`, `_newselect`, `pselect6` and `ppoll` in both time ABIs.
  * 79-6a.5 The time calls in both ABIs: `time`, `stime`, `gettimeofday`, `settimeofday`, the `clock_*`, `timer_*` and `timerfd_*` calls and their `_time64` forms, `nanosleep`, `utime`, `utimes`, `futimesat`, `utimensat`, `adjtimex`, `clock_adjtime`, `getitimer`, `setitimer`, `alarm`.
  * 79-6a.6 `ia32_fs_hosttest.c`: `stat64`, `statfs64`, `linux_dirent`, `flock64`, the 32-bit `iovec` and both time ABIs' structures and the split arguments checked against Linux 6.12's layouts. The i686 builds of `stattest`, `dirtest`, `dirprobe`, `vfsprobe`, `pathcanon` and `timeprobe` run in 79-8c.
* Done: the layout host test passes; 79-8c runs the i686 file and time probes.

### 79-6b ioctl translation

Builds: the compat ioctl path and the translations of the families outside the GPU drivers: terminals, sound, loop, the frame buffer, network devices, evdev, and the DRM core as Linux's `drm_ioc32.c` does on x86.

* Files: `rach2/sys/arch/x86_64/ia32_ioctl.c` and `ia32_ioctl_drm.c` (from 79-1a's stubs); `rach2/sys/evdev.c` after 76-10a (16-byte `input_event` for a compat reader and writer); new `tests/drmcompatprobe.c`, built for i686; new `tests/host/ia32_ioctl_hosttest.c`.
* Waits on: 79-1a (code), 76-5 (code: the KMS ioctls), 76-6 (code: std-vga's driver, on which drmcompatprobe runs), 76-10a (code: evdev's events and `evdev.c`).
* Labels: none.
* Subtasks:
  * 79-6b.1 The path: a command whose argument has one layout in both ABIs passes; one that differs has a translation or fails with ENOTTY, as Linux's compat path; translations keyed by command in `ia32_ioctl.c`, so no driver file changes.
  * 79-6b.2 Terminals, OSS (checked equal), loop (`LOOP_SET_STATUS` and `LOOP_GET_STATUS` with `loop_info`), the frame buffer (`fb_fix_screeninfo`), network devices (`ifreq`, `ifconf`, the old `SIOCGSTAMP`), watchdog and rfkill (checked equal).
  * 79-6b.3 The DRM core: VERSION, GET_UNIQUE, GET_CLIENT, WAIT_VBLANK and MODE_ADDFB2 (packed on x86), the rest checked equal.
  * 79-6b.4 evdev: 16-byte events for a compat caller in `read` and `write`, the size-coded EVIOC calls.
  * 79-6b.5 `ia32_ioctl_hosttest.c`: each translation against Linux 6.12's i386 layout, `drm_version` and `drm_unique` with their 4-byte `size_t` and pointers among them; `tests/drmcompatprobe.c` (VERSION, GETRESOURCES, ADDFB2, WAIT_VBLANK on std-vga) built. drmcompatprobe and the i686 builds of `ptyprobe`, `ttyprobe`, `loopprobe`, `netifprobe` and `evprobe` run in 79-8c.
* Done: the translation host test passes; 79-8c runs drmcompatprobe and the i686 terminal, loop, network and input probes.

### 79-6c GPU drivers' ioctls

Builds: the translations the GPU drivers need: i915's GETPARAM with its int pointer, as Linux's `i915_ioc32.c`; virtio-gpu, udmabuf, dma-buf, syncobj and sync_file checked to need none.

* Files: `rach2/sys/arch/x86_64/ia32_ioctl_gpu.c` (from 79-1a's stub); rows of `rach2/ia32.md`; new `tests/gpucompatprobe.c`, built for i686.
* Waits on: 79-6b, 78-5a, 78-7, 77-3, 76-3 (code); 79-4 (code: the ELF32 loader that runs 79-6c.3's i686 probe on the x86-64 kernel).
* Labels: `linux-host` (the virgl profile).
* Subtasks:
  * 79-6c.1 i915: GETPARAM translated; the GEM calls checked equal (pointers carried as 64-bit fields).
  * 79-6c.2 virtio-gpu, udmabuf, dma-buf, syncobj and sync_file checked equal, each named in the list.
  * 79-6c.3 `tests/gpucompatprobe.c` on virtio-gpu under the virgl profile and on the i915 fault backend; its author runs it once.
* Done: a 32-bit program's GETPARAM, GEM and virtio-gpu calls answer as a 64-bit program's on both devices.

### 79-7a Sockets

Builds: message headers and descriptor passing, socketcall, and the socket calls whose structures differ. Split from 79-7 because sockets and IPC share no files and can run at once.

* Files: `rach2/sys/arch/x86_64/ia32_net.c` (from 79-1a's stub); its rows of `ia32_systab.c`; `man/man2/socketcall.2`; new `tests/host/ia32_net_hosttest.c`.
* Waits on: 79-1a (code).
* Labels: none.
* Subtasks:
  * 79-7a.1 `socketcall` (102) and the direct socket calls (359 to 373).
  * 79-7a.2 Compat `msghdr` and `cmsghdr` with 4-byte alignment: `SCM_RIGHTS`, `SCM_CREDENTIALS`, the old and new timestamps.
  * 79-7a.3 `sendmmsg` and `recvmmsg` with timeouts in both time ABIs.
  * 79-7a.4 `getsockopt` and `setsockopt` with `timeval` (the old and new `SO_RCVTIMEO` and `SO_SNDTIMEO`) and the other options whose layout differs.
  * 79-7a.5 `ia32_net_hosttest.c`: compat `msghdr`, `cmsghdr` with 4-byte alignment, `mmsghdr` and the `timeval` options against Linux 6.12's layouts. The i686 builds of `sockettest`, `udptest`, `tcptest`, `dgramprobe` and `packetprobe` run in 79-8c.
* Done: the layout host test passes; 79-8c runs the i686 socket probes, descriptor passing among them.

### 79-7b IPC, futex times and the rest

Builds: SysV IPC, futex with 32-bit times and the robust list, and the rest of 79-2's list.

* Files: `rach2/sys/arch/x86_64/ia32_ipc.c` and `ia32_misc.c` (from 79-1a's stubs); `rach2/sys/proc/futex.c` after 79-1b (the 32-bit robust list walked at exit); new `tests/host/ia32_ipc_hosttest.c`.
* Waits on: 79-1a, 79-1b (code: the order on `futex.c`).
* Labels: none.
* Subtasks:
  * 79-7b.1 `ipc` (117) and the direct calls with the `ipc64` structures, `semtimedop` in both time ABIs, `msgsnd` and `msgrcv` with the compat message.
  * 79-7b.2 `futex` with 32-bit times and `futex_time64`; `set_robust_list` and `get_robust_list` with the 32-bit list, walked at exit.
  * 79-7b.3 `getrlimit`, `ugetrlimit`, `setrlimit`, the old `uname` forms, `sysinfo`, `times`, `getrusage`, `wait4` and `waitid`.
  * 79-7b.4 `sched_rr_get_interval` in both ABIs, the affinity calls with 32-bit masks, the `mq_` calls, `personality` (`PER_LINUX32` makes `uname` report i686), and the remaining rows 79-2 gives this task, 76-19's among them.
  * 79-7b.5 `ia32_ipc_hosttest.c`: the `ipc64` structures, `rlimit`, `rusage`, `sysinfo`, `tms` and the 32-bit robust list against Linux 6.12's layouts. The i686 builds of `waitprobe`, `limitprobe`, `jobprobe`, `procprobe`, `miscprobe` and `zombieprobe` run in 79-8c.
* Done: the layout host test passes for every structure 79-2 gives this task; 79-8c runs the i686 probes.

### 79-8a The ia32 test profile

Builds: a way to run the i686 tests on the x86-64 kernel: `RACH2_KERNEL=ia32` boots the i686 images with the x86-64 kernel on `qemu-system-x86_64`, with expectations per kernel where the kernels differ. Split from 79-8 because it needs no compat code and can start early (Problems 3).

* Files: `tests/run`, the kernel choice in `tools/etude/` (which knows only i686 and x86-64, `tools/etude/collection.c:1275`); `tests/Readme.md` (the variable's row at line 34); `tests/registry` where a suite cannot run under it; the cases whose expectations differ: `tests/cases/guest.vdso.json` (the i686 kernel has no vDSO, the x86-64 kernel gives a 32-bit one), `tests/cases/guest.fpu.json` (`nofxsr` is the i686 kernel's), `auxvprobe`'s `AT_SYSINFO`, and `uname -m`.
* Waits on: 76-22 (code: its admission change to `tools/etude/` comes first).
* Labels: none.
* Subtasks:
  * 79-8a.1 `RACH2_KERNEL=ia32` in `tests/run` and etude: the i686 images and probes, the x86-64 kernel, `qemu-system-x86_64`, for every suite that boots the kernel directly.
  * 79-8a.2 The expectations per kernel, each case that differs saying why.
  * 79-8a.3 The list of suites that cannot run under it (those whose disk carries `rach2/kernel`), with the reason, for the owner's exclusions.
  * 79-8a.4 The change to `tools/flow/release.sh` that runs the profile at the release, proposed to the manager as a process fix to master.
  * 79-8a.5 `tests/Readme.md`'s table row. The first boot under the profile is 79-8c.2's, since an i686 program needs 79-4's loader.
* Done: `RACH2_KERNEL=ia32` selects the i686 images, the x86-64 kernel and `qemu-system-x86_64` for every suite that boots the kernel directly, and each case that differs by kernel says why.

### 79-8b The i386 root

Builds: i386 libraries and Mesa beside the 64-bit ones in the x86-64 GNU root, for Proton's default mode and 32-bit native programs, as an option the pure 64-bit profile turns off.

* Files: `vendor/gnu/debian/i386.list` (new, its own list under 76-13a's form) and the root's build under `vendor/gnu/` (the multiarch layout and the option, after 76-13a; Debian's i386 packages with multiarch paths: `libc6:i386` with `/lib/ld-linux.so.2`, `libgcc-s1`, `libstdc++6`, Mesa 25.0.7's i386 drivers, 77-7b's i386 Venus driver, `libvulkan1`, `libdrm2`, `libwayland-client0`, `libx11-6` and what Proton's 32-bit Wine loads); `vendor/gnu/Readme.md` (its own section).
* Waits on: 76-13, 77-7b (code); the downloads card (`decision`: the i386 packages).
* Labels: `linux-host`.
* Subtasks:
  * 79-8b.1 The package list from Proton's 32-bit needs and Mesa's i386 drivers, pinned by SHA-256, once the downloads card is answered.
  * 79-8b.2 The multiarch layout in the root, and the root image grown to fit.
  * 79-8b.3 The option that builds the root without the i386 layer.
  * 79-8b.4 The Readme's section. Check: the root builds with and without the layer, and `/lib/ld-linux.so.2` and Mesa's i386 drivers are in it; the i386 `eglinfo` and `vulkaninfo` runs are 79-8c.3's.
* Done: the x86-64 GNU root carries the i386 layer, and with the option off the build makes the root without it.

### 79-8c ABI acceptance

Builds: every i686 test on the 64-bit kernel through 79-8a's profile, mixed tests between the two ABIs, and a 32-bit Windows program under Proton's default mode.

* Files: new `tests/mixprobe.c`, built for both architectures, the i686 build staged at `/ia32/mixprobe` in the x86-64 test image (`tests/Makefile` and the image's staging); new `tests/cases/guest.ia32.json` and an `ia32` line in `tests/registry`; a case in the GNU family for Proton's default mode.
* Waits on: 79-3, 79-4, 79-5, 79-6a, 79-6b, 79-7a, 79-7b, 79-8a, 79-8b (code).
* Labels: `linux-host` (the GNU image).
* Subtasks:
  * 79-8c.1 mixprobe: exec across the ABIs both ways, `SCM_RIGHTS` between a 32-bit and a 64-bit process, a shared futex, ptrace of each by the other, a seccomp filter of each ABI, signals between them.
  * 79-8c.2 The `ia32` family in `guest.ia32.json`: the i686 probes 79-5.6, 79-6a.6, 79-6b.5, 79-7a.5 and 79-7b.5 name, and the direct-boot i686 suites under the profile, each case with its line and per-case requirements; its author runs the earlier tasks' probes and one case of each suite once; failures fixed in the task that owns the call, or here once that task is closed. The release runs the family whole.
  * 79-8c.3 An i386 `eglinfo -B -p surfaceless` and `vulkaninfo --summary` in QEMU, then a 32-bit Windows program under GE-Proton11-12's default mode in the GNU image, read back.
  * 79-8c.4 The suites that cannot run, with their reasons, sent to the owner as one exclusion request.
* Done: mixprobe and the earlier tasks' i686 probes pass under the profile, the `ia32` family is registered for the release, and a 32-bit Windows program runs under Proton's default mode.

### 79-8d 32-bit Direct3D

Builds: 76-21's 32-bit Direct3D programs on lavapipe and 77-6's on Venus, in Proton's default and WoW64 modes where Linux runs them, each against the Linux guest in the same QEMU. Split from 79-8 because it waits on 77-6 and 76-21, which the ABI acceptance does not need.

* Files: cases in `tests/cases/guest.ia32.json` after 79-8c; new `tests/graphics/ref/d3d32-wsl/`.
* Waits on: 79-8c, 79-6c, 79-1b, 76-21, 77-6 (code).
* Labels: `linux-host`.
* Subtasks:
  * 79-8d.1 The 32-bit Direct3D 9, 11 and 12 programs on lavapipe, read back, with the Linux guest's results stored.
  * 79-8d.2 The same on Venus, where WoW64's mappings need `VK_EXT_map_memory_placed` (77-7a's finding).
  * 79-8d.3 Each run once by its author.
* Done: the 32-bit Direct3D programs draw on lavapipe and on Venus as in the Linux guest.

### 79-9 Pure 64-bit profile

Builds: a build switch and Linux's boot switch (`ia32_emulation=`, Linux 6.7 and later) that leave 32-bit programs out; a root with no i386 libraries (79-8b's option off, or today's root); refused execs fail plainly.

* Files: the build switch in the kernel's makefiles: `IA32=0` defines `RACH2_IA32=0`, under which the compat files, the 0x80 gate, the 32-bit code segment and ELF32 compile to nothing, so that every manifest lists the same files for both settings, as `guestcheck` requires (it reads only a plain `NAME =`, `tools/verify/guestcheck.c:395-435`); `rach2/sys/cmdline.c` and `rach2/cmdline.md`; `idt.c`, `percpu.c` after 79-3; `rach2/sys/proc/elf.c` after 79-4; `man/man7/ia32.7`; new `tests/cases/guest.pure64.json` and a `pure64` line in `tests/registry`.
* Waits on: 79-3, 79-4 (code).
* Labels: none.
* Subtasks:
  * 79-9.1 The build switch, with both settings compiled; a case in `tests/cases/host.host.json` builds the kernel with `IA32=0`.
  * 79-9.2 `ia32_emulation=` with Linux's meaning: off leaves the gate out of the IDT and refuses ELF32.
  * 79-9.3 With either switch off, an i386 program's exec fails with ENOEXEC and `int $0x80` raises SIGSEGV, as Linux; two cases, run once by their author.
  * 79-9.4 `ia32.7` and `rach2/cmdline.md`.
* Done: with either switch off, every i386 program is refused with ENOEXEC and `int $0x80` raises SIGSEGV.

### Checkpoint slices for 77, 78 and 79

Each slice runs once, after the milestone's tasks are merged, Codex has reviewed what reached the branch since the last review, and the findings are fixed. The Linux results each case compares with are the ones its task stored. Times are machine time.

77, in QEMU on the testing PC's WSL under KVM with the udmabuf module, the virgl profile of 76-16a and the Venus profile of 76-16b; 9 cases, about 20 minutes with the guests admitted side by side:

* vgpuprobe on virgl, with the reset row: every pending fence fails and every resource is freed (77-2's Done).
* The renderer check: the guest names virgl over the RTX 4080 and Venus over lavapipe.
* piglit: every 200th test of 77-5's list on virgl, about 200 tests, compared by case.
* dEQP-GLES2, GLES3 and GLES31: every 200th case of 77-5's lists, compared by case.
* The WebGL suite: one fixed chunk in Chromium and one in Firefox on virgl.
* dEQP-VK: every 500th case of the lavapipe fraction on Venus, flakes named.
* vkcube under Weston on Venus, N frames checksummed inside the guest.
* A Direct3D 11 and a Direct3D 12 program on Venus, read back.
* WebGPU on Venus: the adapter answer equals the Linux guest's.

Why these prove 77's goal: each advertised API (GL, GLES, Vulkan, Direct3D through DXVK and vkd3d-proton, WebGPU) runs at least one compared case on the device that offers it, the reset row proves the device-loss clause, and every case judges counts against the Linux guest in the same QEMU.

78, in two parts. In QEMU on WSL, 3 cases, a few minutes: the i915 and TCO host tests (`RACH2_CASES` naming them), the `machine.i915fake` boot with Crocus started and faults injected, and the q35 watchdog case. Then one trial boot of the Lenovo with the trial root, announced since it takes the screen, sound at `mixer vol 1`, about 30 minutes of cases with the watchdog armed throughout (Linux's times at 4 jobs, `run2/step9/deqp/run.log`):

* First light and adoption of the firmware's mode without a blank.
* modetest at each panel mode with the pipe CRC against Linux's; `kms_hdmi_inject`.
* IGT: `kms_addfb_basic`, `kms_atomic`, `kms_flip` basic, `kms_pipe_crc_basic`, `kms_cursor_crc` one size, `gem_exec_basic`, `gem_exec_parse`, `gem_ctx_param`, `gem_reset_stats`, `gem_mmap_gtt`, `gem_tiled_blits`, `gem_userptr_blits` basic.
* piglit: every 50th test of `quick`, about 1,260 (the whole took 31 minutes 50 seconds under Linux).
* dEQP-GLES2 whole (154 s under Linux) and every 20th case of GLES3 and GLES31 (about 4,100 cases).
* dEQP-VK on hasvk: every 500th case of `vk-default` (6,431 cases, 81 s under Linux).
* piglit's two hang triggers, each recovered with only the guilty context lost; RC6 residency over 10 s idle.
* One H.264 clip decoded through VA with frame hashes against software; one 720p clip in Chromium and in Firefox with drops counted against Debian's and each decoder named from the browser's log; a hang on the render ring while the video ring decodes, recovered as on Linux.
* A working set beyond the aperture under RAM pressure (i915gemprobe's pressure rows), its contents checked and the end a clean ENOMEM.
* One WebGL chunk in Chromium with the renderer Crocus; glmark2 under Weston against the gate.

Why: it covers every clause of 78's Done once on the hardware (each output path, the parser, a hang, RC6, video, the browsers, the gate) with cases that take minutes; the whole suites run at the release.

79, in QEMU on WSL, 7 cases, about 15 minutes:

* A named part of the `ia32` family: the default suite's i686 probes, the musl programs among them, under `RACH2_KERNEL=ia32`.
* mixprobe.
* wow64probe and futexvprobe on the x86-64 kernel.
* ldtprobe, drmcompatprobe and gpucompatprobe under the profile.
* A 32-bit Windows program under Proton's default mode, and a WoW64 program, in the GNU image.
* The pure 64-bit profile: the build switch's kernel and `ia32_emulation=0` each refusing an i386 exec with ENOEXEC and `int $0x80` with SIGSEGV.
* One 32-bit Direct3D program on lavapipe and one on Venus, read back.

Why: the default suite's probes exercise every subsystem's calls from i386 programs once, mixprobe the ABI boundaries, and the last three the goal's Proton, pure-64-bit and Direct3D clauses. The Lenovo is not needed, since 79 touches no Lenovo hardware.

### Order and parallelism

Start at once, with no blocker in any milestone: 77-7a (once the downloads card is answered; the module is the manager's setup at approval), 79-1a and 79-2. They share no file with each other or with 76's first tasks, but for the one-line tables and the two far-apart hunks the Shared files section names.

Start as soon as one early blocker closes: 77-9 (76-13, 76-22), 78-1a (76-1), 78-1b (78-1a), 78-2a (76-1, 76-4), 78-7 and 78-10a (78-2a), 79-1b (79-1a, 76-2b), 79-5, 79-6a and 79-7a (79-1a), 79-8a (76-22).

Critical paths, counted in tasks of these milestones after the 76 tasks they wait on:

* 77: 77-2, 77-3, 77-4a, 77-4b, 77-10, five tasks, after 76-1, 76-2a, 76-4a, 76-5a and 76-8: ten in all. The path through 77-7a and 77-7b joins at 77-4a and is shorter, since 77-7a starts at once.
* 78: 78-2a, 78-2b, 78-3a, 78-5a, 78-5b, 78-8b, 78-11a, seven code tasks after 76-1, 76-2a and 76-4a, then the Lenovo steps 78-11c and 78-11b: twelve in all with the waits on evidence. The display path 78-2a, 78-2b, 78-3a, 78-9, 78-10b, 78-10c, 78-10d is seven, and 78-5b, 78-6a, 78-13a, 78-13b is as long as the hasvk path's end. 78-4a, 78-7, 78-10a and 78-6b run beside them.
* 79: inside the milestone 79-1a, 79-1b, 79-4, 79-3, 79-8c, 79-8d, six tasks, 79-1b after 76-2b; across the stack the longest chain ending in 79 is ten, through 77's Venus path: 76-1, 76-2a, 76-4a, 76-5a, 76-8, 77-2, 77-3, 77-4a, 77-6, 79-8d. 79-5, 79-6a, 79-6b, 79-7a and 79-7b run beside 79-4.

How many run at once: from the first wave, up to five of these tasks run beside 76's; once 78-2b and 79-1a are in, 78 fans out to four or five code tasks (78-3a, 78-4a, 78-7, 78-10a, then 78-5a, 78-6b, 78-9) and 79 to six (79-1b, 79-5, 79-6a, 79-6b, 79-7a, 79-7b), more than the nine loops hold, so no loop waits on these milestones once 76's base is in.

The Lenovo's queue for 78, one step at a time, run by Mac loops. Code tasks never wait on it. A later step that is ready first goes first, except that L2 to L4 keep their order, since each trial builds on the one before, and L7 runs last.

* L1 78-1b, silent: a Debian boot of its own for the controlled expiry, which ends it, then the Rach2 trial with the forced expiry. 80-2's session, with 81-1's trace in it (80-2.2), is a boot apart, since it takes the GPU.
* L2 78-4b, silent: first light.
* L3 78-5c, silent: submission as root on the trial root.
* L4 78-6c, then 78-11c, silent and off-screen, each in a boot of its own, since each takes the GPU.
* L5 78-10d, the screen: the Debian CRC references, then the display trial.
* L6 78-12b, 78-11b and 78-14b, one after another, each in a boot of its own, the screen and sound at `mixer vol 1`.
* L7 78-13b, the screen, the longest.
* L8 78's checkpoint trial boot.
* 77-10, a Debian session of its own that takes the screen, goes wherever it is ready.

Waits and collisions found while splitting, and what removes each:

* Code must never wait on the Lenovo: the plan's 78-2 waits on 78-1 (plan.md, 78's table). Here 78-2a and 78-2b wait on nothing of 78-1; only the first trial (78-4b) waits on 78-1b, as evidence.
* `rach2/sys/drm/i915/` would be edited by several tasks at once: each file has one owner at a time, and stubs hand files on in a named order (Shared files).
* The i915 interrupt file would be touched by 78-6b, 78-9 and 78-14a at once: 78-4a writes every source with a handler table, so they register from their own files.
* `ia32_systab.c` and the x86-64 manifest would be edited by every 79 task: 79-1a writes all rows and all stubs first.
* `syscall_proc.c` is edited by 76-2a, 76-2b, 76-19, 79-1b and 79-4: 76-2a, 76-2b, then 76-19 and 79-1b at far-apart hunks, then 79-4; 79-1a, which waits on nothing, does not touch it.
* `tests/host/i915_model.c` would be edited by every task of 78: 78-2a writes the core with a table of blocks, and each task adds a block file of its own.
* Case manifests would be shared by 77-4b, 77-6 and 77-8, which can run at once: each task has its own family file.
* A code task that waits on a Lenovo task waits on the machine: 78-9, 78-11a, 78-12a and 78-14a wait only on code; the Lenovo steps 78-11c, 78-12b, 78-13b and 78-14b wait on earlier steps as evidence, and the queue orders them.
* `tools/etude/` is changed by 76-22 and 79-8a: 79-8a waits on 76-22.
* Downloads: Mesa 26.1.6 (77-7a), VK-GL-CTS's sources (77-9), the VA packages (78-14a) and the i386 packages (79-8b) are on the one downloads card that the manager files with the milestones (decision G in `plan.md`), so no loop stops on a question.

## 80 games

The goal (plan.md, 79): on the Lenovo, Crysis plays to the owner's criterion; SuperTuxKart and 0 A.D. play within the gate of Linux's frame times; an i386 native game and a 32-bit Windows Direct3D 9 game run; saves survive updates.

The plan's ten tasks become fourteen: 80-7 splits in three and 80-8 and 80-10 in two, since each mixes work that can start early with work that must wait, in different files.

### 80-1 Proton for the games

The game route's settings on Rach2 over 76-13's GE-Proton: WoW64, WineD3D's GL renderer, fsync, `SteamGameId` for `PROTON_LOG`, each confirmed from Proton's log.

* Files: `vendor/gnu/games/rach2-game.sh` (new, staged as `/usr/bin/rach2-game`), `man/man1/rach2-game.1` (new), `man/aliases` (one line if any), the Proton section of `vendor/gnu/games/Readme.md` (made by 80-7a).
* Waits on: 76-13 (code: GE-Proton11-12 staged in the root with its libraries); 79-1a and 79-1b (code: the 32-bit code segment and `futex_waitv`, without which the WoW64 route and fsync cannot run); 80-7a (code: the games layer's directory, Readme and install script, so that two tasks do not create them).
* Labels: `linux-host`.
* Subtasks:
  * 80-1.1 `rach2-game.sh` sets `PROTON_USE_WOW64=1`, `PROTON_USE_WINED3D=1`, WineD3D's GL renderer (`WINE_D3D_CONFIG=renderer=gl`), `SteamGameId` and `PROTON_LOG`, and leaves fsync on; check: the script's `--print` lists each variable under the name Proton's script reads (`proton:533` for WoW64, `:1086-1089` for WineD3D's DLLs).
  * 80-1.2 Run a 64-bit Direct3D 9 probe through the route in QEMU on llvmpipe; check: Proton's log on Rach2 names WOW64, WineD3D, the GL renderer and fsync, and no DXVK DLL is loaded.
  * 80-1.3 Record that the pinned DXVK and vkd3d-proton are not on the route on the Lenovo, with Linux's refusals as the reason (hasvk lacks Vulkan 1.3, then `shaderInt16`; vkd3d-proton lacks `maintenance5` and `maintenance6`); check: the Readme's Proton section names each refusal and its source in the second run's report.
  * 80-1.4 `rach2-game(1)`: the route, its variables, the log's place; check: `make mancheck` and `mandoc -T lint` pass.
  * 80-1.5 A row in `tests/system/gnu_rows.sh` that starts `rach2-game --print` on the GNU root; check: the row's output names the four settings.
* Done: on Rach2 in QEMU, Proton's own log of a Direct3D 9 probe started by `rach2-game` shows WoW64, WineD3D on GL and fsync on.

### 80-2 The graphics and games reference

The Linux references still missing, taken on the Debian on the Lenovo's p5 in one boot, with the KVM page trace of 81-1 moved in so that Debian boots once for both.

* Files: `tools/lenovo/debian-ref/` (new: the session's scripts, each step's command and expected output), `dev-log/80-games/80-2.md` (new: the numbers, each with renderer, IOMMU state, cache preparation, case list and concurrent work). No file of the product.
* Waits on: nothing.
* Labels: `lenovo`, and `owner-only` from the start, since every step is on the Lenovo.
* Subtasks:
  * 80-2.1 The session's scripts staged on the Mac and rehearsed in a Debian 13 guest under QEMU: the boot through GRUB's `next_entry` with `intel_iommu=off` (the gate's configuration, plan.md, Yours, kept, which also puts i915 on stolen memory and 4 KiB pages), each step's command, and the exit by `rtcwake -m off -s 120` as the machine skill says; check: the rehearsal runs every script to its end in the guest.
  * 80-2.2 Silent steps without the screen, first: Linux's KVM exit costs (kvm-unit-tests' `vmexit_*` cases) and the KVM selftests' timing cases, at revisions this step pins and records for 81-6b; the "kvm run failed Bad address" reproducer of `run2/step10/` (`kvm-efault-host.txt`, `qemu-sdl2-efault.log`) run again with `shmem_enabled=always` and the failing page traced (its flags and reference count, by bpftrace on `hva_to_pfn_remapped`); the register dump of eDP with IGT's tools; check: each number and the trace are in `80-2.md` with their raw output kept.
  * 80-2.3 The WebGPU-on-Venus trace in step 10's guest, with risk 15's settings, `--enable-logging` and the adapter variants, the guest's display headless; check: `80-2.md` says why Chromium 156 lists Venus over hasvk and returns no adapter, or what the trace rules out.
  * 80-2.4 "Lenovo notice:" on the issue, ten minutes before the steps that take the screen; check: the comment's time is ten minutes or more before 80-2.5.
  * 80-2.5 SuperTuxKart and 0 A.D. fixed replays with frame times (the same replay files 80-7c stages), Firefox's H.264 and VP9 at 1080p30 with `media.ffmpeg.vaapi.enabled` false, each under cage and marked so; check: frame-time percentiles and drop counts in `80-2.md`.
  * 80-2.6 Crysis run directly and through umu on the chosen fullscreen route with a silent working sound device (`snd-dummy` or a PulseAudio null sink), the flyby's four loops each way, and the suit's reboot on the first level reached from a checkpoint save with the HUD read back; check: both routes' FPS beside each other and the HUD's state in `80-2.md`.
  * 80-2.7 Debian left and p1 put back as the machine skill says, and what the session added to the boot partition deleted; check: `bootctl status` and the boot partition's hash as before the session.
* Done: every reference the plan gives 80-2 (plan.md, 80's table) is in `80-2.md` with its configuration, the KVM trace among them. HDMI's references and the whole first level need a person or a monitor and are the sitting's (84-2, 84-3).

### 80-3 Wine in QEMU

The pinned GE-Proton's Wine run directly in QEMU on virgl: a new prefix, 32-bit and 64-bit programs, GL and Direct3D 9 through Xwayland, `VideoMemorySize`, Wine's memory reservations.

* Files: `vendor/gnu/games/rach2-game.sh` (after 80-1: the `VideoMemorySize` registry key), `tests/system/games/` (new: the Wine probes and their runner), `tests/cases/machine.games-wine.json` (new), `tests/registry` (one row), `tests/system/suites.mk` (the family's sentence, its own block), the Wine section of `vendor/gnu/games/Readme.md`.
* Waits on: 80-1 (code: the route and the script it extends); 77-2 (code: virtio-gpu 3D, without which virgl does not exist on Rach2).
* Labels: `linux-host`.
* Subtasks:
  * 80-3.1 `VideoMemorySize` written into each new prefix from the device's memory, since WineD3D on virgl reports 16,430,080 and 352,620,360 MiB in two runs; check: a new prefix's registry has the key, and WineD3D's log reports that size.
  * 80-3.2 A new prefix made by `rach2-game`; check: `wineboot` ends with status 0 and the prefix has `drive_c/windows/syswow64`.
  * 80-3.3 64-bit GL and Direct3D 9 probes through Xwayland on virgl, pixels read back in the guest; check: the probes' checksums equal Linux's in the same QEMU.
  * 80-3.4 The same probes 32-bit under the WoW64 route; check: the same checksums.
  * 80-3.5 Wine's address-space reservations at start (its preloader's ranges) against Linux's `/proc/PID/maps` for the same program; check: the reserved ranges are the same or the difference is named in the Readme.
  * 80-3.6 The `games-wine` family with these cases, its line in `tests/registry` and its block in `suites.mk`, with per-case `requirements.hosts`; check: `tests/run` lists each case, and its author runs one case once in QEMU on WSL. The release runs the family whole.
* Done: in QEMU on virgl, the pinned Wine makes a new prefix and draws GL and Direct3D 9, 32-bit and 64-bit, with the pixels Linux draws in the same QEMU.

### 80-4 Wine's display, input and sound

A fullscreen X client at 1366x768 placed at the origin, unframed, on the kiosk shell, with focus and mouse capture; relative mouse motion; scaled lower modes; a working sound output for Wine; input devices found through `/sys/class/input`.

* Files: a patch to Weston's Xwayland window manager (`xwayland/window-manager.c`, its `legacy_fullscreen`, in Debian's Weston 14 source) carried as a rebuilt package beside the pinned ones, in `vendor/gnu/games/weston/` (new: the patch, the pinned source package and its build recipe); 76-14's session configuration (after 76-14); `tests/system/games/xplace.c` (new), `tests/cases/machine.games-wm.json` (new, its own family, with its line in `tests/registry` and block in `suites.mk`).
* Waits on: 76-14 (code: the session, Weston's configuration and the sound routes); 76-9 (code: `/sys/class/input` and the uevents Wine reads); 80-3 (code: `tests/system/games/`, whose runner it adds `xplace` to); the downloads card (`decision`: Weston's source package).
* Labels: `linux-host`.
* Subtasks:
  * 80-4.1 Read why Weston 14's Xwayland frames a fullscreen client at +32+32 (`legacy_fullscreen` in `xwayland/window-manager.c`), and decide between Weston's window manager and Wine's hints; check: the reason is in the Readme with the source lines.
  * 80-4.2 The fix as a rebuilt Weston package: Debian's source package pinned by SHA-256, the patch, a build in a Debian container on the Linux host, the result pinned and staged in place of Debian's binary; check: the root's `weston` reports the rebuilt version and the patch's name.
  * 80-4.3 `xplace`, a test X client that asks for 1366x768 fullscreen without `_NET_WM_STATE`, as Wine does; check: in QEMU at 1366x768 it sits at 0,0 with no frame, has focus, and its pointer grab confines the pointer.
  * 80-4.4 Relative motion and scaled lower modes: the client at 800x600 is scaled to the output, and raw motion events reach it while the pointer is confined; check: `xplace`'s report of its geometry and of motion deltas.
  * 80-4.5 Wine's sound through 76-14's PulseAudio on OSS, silent or not; check: a tone from a Wine program recorded through the audio loopback.
  * 80-4.6 Input devices found by Wine through `/sys/class/input`; check: Wine's `dinput` log lists the keyboard and mouse.
  * 80-4.7 The Readme's section on the route; check: the section names the patch, the pin and the cases.
* Done: in QEMU, a fullscreen X client at 1366x768 is at the origin, unframed, focused and captured on the kiosk shell, and Wine plays a recorded tone; a launcher under cage is not used.

### 80-5 Wine on the Lenovo's GPU

A Direct3D 9 program on Crocus as soon as the parser is in; WineD3D on hasvk beside it as a research comparison for small programs.

* Files: `tests/system/games/` (after 80-4: the Lenovo's probe scripts), `dev-log/80-games/80-5.md` (new).
* Waits on: 78-8a, 78-8b (code: the PPGTT and parser, after which an ordinary user submits); 78-11a (code: hasvk's kernel pieces, for 80-5.4); 80-3 (code: the probes); 80-4 (code: `tests/system/games/` after it); 76-15b (code: the trial root's slot); 76-15c, 78-6c, 78-11c (`evidence`: the data partition, ordinary users' submissions and hasvk proven on the Lenovo).
* Labels: `lenovo`.
* Subtasks:
  * 80-5.1 The trial root with the route and the probes, built and its hash recorded; check: it boots in QEMU from a file on a data-partition image.
  * 80-5.2 The Lenovo step prepared: Weston headless with GL on Crocus's render node, so that nothing takes the screen; `mixer vol 1` after the boot; the exact commands; check: the step's script runs to its end in QEMU on llvmpipe.
  * 80-5.3 Lenovo step, one silent trial boot of its own for 80-5.3 and 80-5.4: the Direct3D 9 probe on Crocus, pixels read back; check: the checksum equals Linux's on the same Lenovo.
  * 80-5.4 Lenovo step: WineD3D's Vulkan renderer on hasvk on the small Direct3D 11 readback test, 1000 round trips; check: the time beside Linux's 26.6 ms (Vulkan) and 182.3 ms (GL) in `80-5.md`.
* Done: a Direct3D 9 program draws right on Crocus under Rach2; the hasvk comparison is recorded as research only.

### 80-6 Crysis installed

The installer unpacked by innoextract onto the data partition, the launcher in place, saves and Mesa's shader cache on the data partition.

* Files: `vendor/gnu/games/crysis.sh` (new: the install and launch procedure, run on the machine), `man/man1/rach2-game.1` (its Crysis section, after 80-1). The machine skill's Crysis line (`.agents/skills/machine/SKILL.md:59`) and `build/cache/crysis/README` on the Mac are the manager's process fix on master before this task's step (plan.md, After your approval).
* Waits on: 76-15b (code: the data partition's mount); 80-7a (code: innoextract staged by the games layer); 80-1 (code: `rach2-game`); 76-15c (`evidence`: the data partition on the Lenovo).
* Labels: `lenovo`.
* Subtasks:
  * 80-6.1 `crysis.sh install SETUP.exe`: innoextract unpacks GOG's three installer files onto the data partition, and each file is checked against `game.sha256`; check: in QEMU from a copy of `build/cache/crysis/`, 2,358 files match.
  * 80-6.2 The prefix, the saves and `MESA_SHADER_CACHE_DIR` on the data partition; check: a save written in one boot is read in the next, and Mesa's cache statistics show hits on the second run.
  * 80-6.3 The launch through `rach2-game` with `Bin64/Crysis.exe -dx9` and the Bin directory as working directory; check: the game's log in QEMU shows the renderer started.
  * 80-6.4 Lenovo step: the install onto the Lenovo's data partition from the Mac's copy, silent, on a trial boot, `mixer vol 1` after the boot; check: every file matches `game.sha256`.
* Done: Crysis is installed on the Lenovo's data partition by innoextract, its saves and Mesa's shader cache live there, and an update of the root leaves them.

### 80-7a The games layer and innoextract

The directory of the games layer in the GNU root, with innoextract from Debian, so that 80-1, 80-6 and 80-7c start from it. Split from the native games' packages (80-7c), which 80-1 and 80-6 do not need.

* Files: `vendor/gnu/games/Readme.md` (new), `vendor/gnu/games/install_games.sh` (new), `vendor/gnu/games/layer.list` (new: innoextract, in 76-13a's list format), `vendor/gnu/install_root.sh` (one call line).
* Waits on: 76-13a (code: the Debian package fetcher and its list format); the downloads card (`decision`: innoextract).
* Labels: `linux-host`.
* Subtasks:
  * 80-7a.1 The games layer: Readme, install script, one call from `install_root.sh`; check: `make os-real-gnu-ext2` stages the layer and `install_root.sh` refuses a file the layer and the root both have.
  * 80-7a.2 innoextract with its closure, pinned under the fetcher's snapshot; check: the fetcher's `check` step resolves its libraries inside the root, and `innoextract --version` runs in QEMU.
* Done: the GNU root has the games layer and innoextract.

### 80-7c The native games' packages and replays

SuperTuxKart and 0 A.D. from Debian with their fixed replays, run in QEMU, as a family of their own.

* Files: `vendor/gnu/games/native.list` (new: the packages, in 76-13a's list format), `vendor/gnu/games/replays/` (new: the replay files), `tests/cases/machine.games-native.json` (new, with its line in `tests/registry` and block in `tests/system/suites.mk`), the native games' section of `vendor/gnu/games/Readme.md`, the programs' pages under `man/` and `man/programs.md` (made again).
* Waits on: 80-7a (code: the layer); the downloads card (`decision`: SuperTuxKart, 0 A.D.).
* Labels: `linux-host`.
* Subtasks:
  * 80-7c.1 SuperTuxKart and 0 A.D. with their closures, pinned under the fetcher's snapshot; check: the fetcher's `check` step resolves each program's libraries inside the root.
  * 80-7c.2 Fixed replays: SuperTuxKart's benchmark mode and a 0 A.D. replay started by `-replay-visual`, each writing frame times; check: both run to their end in QEMU on llvmpipe and write the file.
  * 80-7c.3 Their pages: the programs' upstream pages rendered as the ports' are, and `programs.md` made again; check: `make mancheck`, which the pre-commit hook runs.
  * 80-7c.4 The `games-native` family with both replays and per-case `requirements.hosts`; check: `tests/run` lists its cases, and its author runs one once. The release runs the family whole.
* Done: the GNU root has SuperTuxKart and 0 A.D., both replays run to their end in QEMU, and the family is registered for the release.

### 80-7b Native games on the Lenovo

SuperTuxKart and 0 A.D. replays on Crocus with frame times and sound, and an i386 game under 79's i386 root.

* Files: `tests/system/games/` (the Lenovo's replay scripts, after 80-4), `vendor/gnu/games/native.list` (after 80-7c: the i386 game), `dev-log/80-games/80-7.md` (new).
* Waits on: 80-7c (code: the packages and replays); 80-4 (code: `tests/system/games/` after it); 78-12a (code: GL on Crocus for browsers and X clients); 79-8b, 79-8c (code: the i386 libraries and Mesa, and the ABI's acceptance); 79-6c (code: i915's 32-bit GETPARAM); 76-15b (code: the trial root's slot); 80-2, 76-15c, 78-12b (`evidence`: Linux's frame times with translation off, the data partition, GL on Crocus proven on the Lenovo).
* Labels: `linux-host`, `lenovo`.
* Subtasks:
  * 80-7b.1 The i386 game: Debian's `supertuxkart:i386`, so that its replay is the same as the 64-bit one, unless 79-8c names another; check: it runs its replay in QEMU on the i386 root.
  * 80-7b.2 The trial root and the Lenovo step's scripts, with `mixer vol 1` after each start; check: the scripts run in QEMU.
  * 80-7b.3 "Lenovo notice:" ten minutes before the step; check: the comment's time.
  * 80-7b.4 Lenovo step, one trial boot of its own that takes the screen: both replays and the i386 game on Crocus with sound at volume 1; check: frame-time percentiles within 10% of 80-2's translation-off numbers, marked "under cage" where Linux's were.
* Done: SuperTuxKart's and 0 A.D.'s replays on the Lenovo are within the 10% gate against translation-off Linux, and the i386 game plays its replay.

### 80-8a Namespace limits as Linux's

Rach2's limits raised to Linux's for the Steam runtime's container: more than 32 mount namespaces at once and 340 extents in a user namespace's map.

* Files: `rach2/sys/proc/ns.c`, `ns.h`, `ns.Readme.md`, `rach2/sys/fs/vfs.c` (after 76-18a), `vfs.h` (after 80-10a), `tests/system/gnu_rows.sh` (one new row), `man/man7/limits.7`, `man/man2/unshare.2`, `tests/musl/nsmusl.c`, `tests/host/` (a new `ns_limits_hosttest.c`).
* Waits on: 80-10a (code: both edit `vfs.h`); 76-18a (code: both edit `vfs.c`).
* Labels: none.
* Subtasks:
  * 80-8a.1 Mount visibility no longer a bit of a 32-bit mask in each mount (`ns_hidden`, `vfs.h:367`; `ns.Readme.md:44`), so that mount namespaces are bounded by memory as on Linux, with Linux's `user.max_mnt_namespaces` in `/proc/sys`; check: the host test makes 1,000 namespaces and mounts in each, and each sees what it should.
  * 80-8a.2 `NS_EXTENTS` raised from 5 to 340 (`ns.h:33`), with Linux's rules for sorting and lookup of a large map; check: `nsmusl` writes a 340-extent map, a 341st is `EINVAL`, and ids map as Linux maps them.
  * 80-8a.3 A new row in `tests/system/gnu_rows.sh` that runs bubblewrap with 40 mount namespaces and a 340-extent map; check: its author runs that row alone once on the GNU root in QEMU.
  * 80-8a.4 `limits(7)`, `unshare(2)` and `ns.Readme.md` state the new limits; check: `make mancheck`.
* Done: the 33rd mount namespace and a 340-extent map succeed as on Linux in the host test, `nsmusl` and the new row; the release runs the existing namespace cases.

### 80-8b umu and the Steam runtime

umu-launcher with its Steam Linux Runtime, pinned: the container's namespaces, mounts, descriptors and sandbox, `/dev/dri` and the sound socket passed in, its D-Bus proxy.

* Files: `vendor/gnu/games/umu.list` (new: umu-launcher and the runtime's archive, pinned by SHA-256, with its self-update off), `vendor/gnu/games/rach2-game.sh` (after 80-3: the umu path), `man/man1/rach2-game.1` (its umu section), `tests/cases/machine.games-umu.json` (new, with its line in `tests/registry` and block in `suites.mk`).
* Waits on: 80-3 (code: the route works directly first, so a container fault is told apart); 80-8a (code: the limits); the downloads card (`decision`: umu-launcher and the Steam Linux Runtime).
* Labels: `linux-host`.
* Subtasks:
  * 80-8b.1 umu-launcher and the runtime (sniper) fetched once, checked, staged, and umu's runtime update turned off; check: a second start downloads nothing.
  * 80-8b.2 `rach2-game --umu` starts the Direct3D 9 probe in the container with WoW64 and WineD3D; check: Proton's log in the container shows the same settings as the direct route.
  * 80-8b.3 What the container needs from the kernel, listed from pressure-vessel's own log; each refusal fixed in Rach2 or brought to the owner as a scope decision; check: the log has no refused call.
  * 80-8b.4 `/dev/dri`, the PulseAudio socket and the D-Bus proxy inside the container; check: the probe draws on virgl and plays a tone recorded through the loopback.
  * 80-8b.5 The pin and download in the Readme, with their sizes; check: the Readme's table.
  * 80-8b.6 The `games-umu` family with 80-8b.2's and 80-8b.4's checks; check: `tests/run` lists its cases, and its author runs one once. The release runs the family whole.
* Done: in QEMU, umu starts the probe in the pinned Steam Linux Runtime and it draws and plays sound as on the direct route.

### 80-9 Acceptance

The owner's criterion for Crysis on the Lenovo through umu, with the direct route beside it; the native games within the gate; saves across a cold restart and an update; a new and an existing prefix; the 32-bit Direct3D 9 game.

* Files: `tests/system/games/` (the acceptance script), `tests/cases/machine.save-update.json` (new), `tests/registry` (one row), `tests/system/suites.mk` (its block), `dev-log/80-games/80-9.md` (new).
* Waits on: 78-12a (code: the launcher's GPU profile); 79-8c (code: the ABI's acceptance, under which the 32-bit game runs); 80-4, 80-8b (code: the window manager's fix and umu); 78-13b, 80-2, 80-5, 80-6, 80-7b (`evidence`: the gate's configuration, Linux's numbers and the earlier steps).
* Labels: `lenovo`.
* Subtasks:
  * 80-9.1 The trial root and the scripts, rehearsed in QEMU with `crysis-game.ext2`; the rehearsal of a prefix and a save surviving an update of a data-partition image registered as the `save-update` family, with its row in `tests/registry` and its block in `suites.mk`; check: the script runs to the flyby's end in QEMU, and its author runs `save-update` once.
  * 80-9.2 "Lenovo notice:" ten minutes before; check: the comment's time.
  * 80-9.3 Lenovo step, one trial boot of its own for 80-9.3 to 80-9.6 that takes the screen, `mixer vol 1` after the boot: the flyby's four loops at Low 1366x768 through umu and directly, on the kiosk shell's fullscreen route, with a working sound output at volume 1 and `s_SoundEnable = 1`; WineD3D and fsync read from the logs; check: each loop at 24 FPS or more, beside 80-2's Linux numbers.
  * 80-9.4 Lenovo step: the suit's reboot on the first level from a checkpoint save, the HUD back afterwards; check: a screenshot read in the machine shows the HUD.
  * 80-9.5 Lenovo step: `Bin32/Crysis.exe -dx9` under the WoW64 route, the flyby once, as the 32-bit Windows Direct3D 9 game of the goal (Problems, item 9); check: it completes the loop.
  * 80-9.6 Saves across a cold restart and across a trial root's update; a new prefix and the existing one; check: the same save loads after each.
* Done: as 80's Done (plan.md, 80): the flyby, the suit's reboot from a save, the saves, the native games within the gate and the 32-bit game, every number beside Linux's on the same Lenovo. The whole first level is the sitting's (84-3).

### 80-10a User extended attributes on ext2

Wine's `user.DOSATTRIB` (6,400 calls in step 8) stored on ext2 as Linux's ext2 stores them: in the shared attribute block that `i_file_acl` names.

* Files: `rach2/sys/fs/ext2xattr.c` (new), `rach2/sys/fs/ext2.c`, `ext2.h`, `rach2/sys/fs/vfs.h` (an attribute operation), `rach2/sys/syscall_fs.c` (`xattr_call` and the twelve calls at `:4616-4708`, today `-EOPNOTSUPP` or empty), `rach2/sources.mk` (one line), `rach2/sys/fs/ext2.Readme.md`, `man/man2/getxattr.2`, `tests/host/ext2_hosttest.c` and a new `ext2_xattr_rows.h`, `tests/fuzz/ext2_fuzz.c`.
* Waits on: 76-15a (code: the forced ext2 test, which may fix `ext2.c`).
* Labels: none.
* Subtasks:
  * 80-10a.1 Read, list, set and remove `user.` attributes in the attribute block, with Linux's header, entry layout, hash and sorting, and a block shared by several inodes split on write; check: host test rows against volumes `mke2fs` and Linux's `setfattr` wrote.
  * 80-10a.2 The VFS operation and the twelve calls dispatched to it; other types keep today's answers; check: `getxattr(2)`'s rows on ext2, tmpfs and KRFFS.
  * 80-10a.3 Interruption rows: a stop at each write of a set or a remove, then on the copy `e2fsck -fy`, then a second `e2fsck -fn`, in the order 76-15a's test uses; check: the first run repairs only the documented state of an unclean stop and the second finds the volume clean.
  * 80-10a.4 The ext2 fuzzer reaches the attribute parser; check: the corpus replays clean under the sanitizers.
  * 80-10a.5 `getxattr(2)` and `ext2.Readme.md` give the new contract; check: `make mancheck`.
* Done: Wine's attribute calls succeed on ext2, after every interrupted write `e2fsck -fy` repairs only the documented state and a second `e2fsck -fn` is clean, and the volume reads the same under Linux.

### 80-10b Wine's proc and sys files and calls

The rest of what step 8 found Wine asking: writable `/proc/PID/task/TID/comm`, the CPU cache, NUMA, DMI and clock-source files of `/sys`, `STATX_MNT_ID`, `mnt_id` in `fdinfo`, `name_to_handle_at`, and `/proc/PID/fd` naming the file's own path.

* Files: `rach2/sys/fs/procfs.c` and `synthfs.c`, `synthfs.h` (after 76-9b), `rach2/sys/syscall_fs.c` (after 80-10a), `rach2/sys/fs/fd.c`, `file.h`, `rach2/sys/arch/x86_64/systab.c`, `rach2/sys/arch/i686/systab.c`, both `sysnr.h` if needed, `rach2/syscalls.md` (made again), `man/man2/name_to_handle_at.2` (new, with `open_by_handle_at` as an alias), `man/man2/stat.2`, `man/man5/proc.5`, `man/man5/sysfs.5`, `tests/musl/statxmusl.c`, `proclinkmusl.c`, `procfilesmusl.c`, `tests/host/synthfs_hosttest.c`, `procfs_hosttest.c`.
* Waits on: 76-9b (code: `procfs.c` and `synthfs.c` after it; 76-19's two `/proc/sys` files moved to 76-9b, so 76-19 edits neither); 80-10a (code: `syscall_fs.c`).
* Labels: none.
* Subtasks:
  * 80-10b.1 `/proc/PID/task/TID/comm` written as Linux takes it (15 bytes, own thread group); check: `procfilesmusl` names 183 threads as step 8's Wine did.
  * 80-10b.2 `/sys/devices/system/cpu/cpuN/cache/indexM/*` from CPUID leaf 4, `/sys/devices/system/node/node0/*`, `/sys/class/dmi/id/*` from the SMBIOS tables (`rach2/sys/arch/x86/smbios.c`), `/sys/devices/system/clocksource/clocksource0/*`; check: the classifier's host test and the files' text against Linux's on the same QEMU machine.
  * 80-10b.3 `STATX_MNT_ID` and `mnt_id` in `fdinfo`, from the mount's id that `mountinfo` already prints (`procfs.c:3283`); check: `statxmusl` compares both with `mountinfo`.
  * 80-10b.4 `name_to_handle_at` and `open_by_handle_at` as Linux answers them for libudev, on both architectures; check: a musl row and `mancheck`.
  * 80-10b.5 `/proc/PID/fd/N` follows a rename, as Linux's `d_path` does; check: `proclinkmusl` renames an open file and reads the new path.
  * 80-10b.6 `proc(5)`, `sysfs(5)`, `stat(2)` and the new page; check: `make mancheck`.
* Done: each call and file of 80-10's row in plan.md's table answers as Linux's on the same QEMU machine.

## 81 kvm

The goal (plan.md, 81): Debian's QEMU on Rach2 boots a Debian guest and Rach2's own images with `-accel kvm` on the Lenovo (VMX), the testing PC (SVM under nested KVM and under TCG) and the Mac (SVM under TCG); kvm-unit-tests and the KVM selftests pass or skip with a stated reason; the cost of a VM exit is measured.

The plan's seven tasks become twelve. 81-1 is split so that the files, the vCPU state and the backend interface start early and only the memory slots wait on 76-2b; 81-3 is split into its code (81-3a) and its Lenovo step (81-3b); 81-6 is split by host, since it mixes a kernel change, the GNU layer and the test tools; 81-6d carries the Lenovo step of 81-6a and 82-1.

All of 81's kernel code goes in `rach2/sys/kvm/` (new), listed by `rach2/sys/kvm/sources.mk` (new), which `rach2/sys/arch/x86_64/sources.mk` includes, so that the i686 kernel builds without it.

### 81-1a The KVM interface

`/dev/kvm`, the VM and vCPU descriptors, the capabilities QEMU requires, the run page, the state ioctls, the run loop and the backend interface, and the kernel's own use of eventfds.

* Files: `rach2/sys/kvm/Readme.md`, `sources.mk`, `kvm.h` (the backend and slot interface), `kvm_abi.h` (Linux's API 12 structures in C89), `kvm.c`, `vcpu.c`, `cpuid.c`, `msr.c` (all new), with stubs for `svm.c`, `vmx.c`, `vlapic.c`, `irq.c`, `iobus.c`, `slots.c`, `tdp.c`, `dirty.c`, `decode.c` (a decoder that refuses every instruction), `svm_run.s` and `vmx_run.s` (entry points that return an error) that the later tasks replace, so that no later task edits `sources.mk`; `rach2/sys/arch/x86_64/sources.mk` (one include line); a row in 76-1's device registry (`kvm` 10:232), not `devfs.c`; `rach2/sys/io/eventfd.c`, `eventfd.h`; `man/man4/kvm.4` (new); `man/man7/limits.7`; `rach2/hardware.md`; `tests/host/kvm_hosttest.c` (new), `tests/host/eventfd_hosttest.c`.
* Waits on: 76-1 (code: the device registry); 76-2a (code: the mapping dispatch through `file_ops.mmap`, which the run page needs).
* Labels: none.
* Subtasks:
  * 81-1a.1 `kvm.h`: the backend operations (create, run, state, event injection, the guest-physical entry format), the slot lookup that 81-1b, 82-2 and 83-5a call, and the lock order, written in the Readme; check: every later file of 81 compiles against it.
  * 81-1a.2 `/dev/kvm`, `KVM_GET_API_VERSION` 12, `KVM_CHECK_EXTENSION` answering QEMU's required capabilities (USER_MEMORY, DESTROY_MEMORY_REGION_WORKS, JOIN_MEMORY_REGIONS_WORKS, INTERNAL_ERROR_DATA, IOEVENTFD, IOEVENTFD_ANY_LENGTH, IRQFD, and for x86 SET_TSS_ADDR, EXT_CPUID, MP_STATE, SIGNAL_MSI, IRQ_ROUTING, DEBUGREGS, XSAVE, VCPU_EVENTS, X86_ROBUST_SINGLESTEP, MCE, ADJUST_CLOCK, SET_IDENTITY_MAP_ADDR) only as each later task makes it true, `KVM_CREATE_VM`, `KVM_CREATE_VCPU`, `KVM_GET_VCPU_MMAP_SIZE`; check: the host test against a backend that refuses to run.
  * 81-1a.3 The run page mapped into QEMU through the vCPU descriptor's `file_ops.mmap`, which 76-2a's dispatch calls (today `mmap_file_check`, `syscall_proc.c:6102-6112`, refuses a descriptor that is not a file), giving a `vm_file` whose `share` returns the page's frame (`rach2/sys/mm/vm.h:65-71`), so that no edit of `mm/` is needed; check: a musl program maps it, and its fields survive a fork as Linux's do.
  * 81-1a.4 The run loop: signals and `immediate_exit`, the big lock let go in guest mode and taken back on each exit (`bkl_drop`, `bkl_take`, `rach2/sys/proc/smp.h:53-60`), a vCPU stopped from another processor by `smp_kick` (`smp.h:78`), exits to QEMU for port I/O, MMIO, HLT, shutdown and internal errors; check: the host test's fake backend drives each exit kind.
  * 81-1a.5 The state ioctls: registers, special registers, FPU and XSAVE with XCRs, MSRs with `KVM_GET_MSR_INDEX_LIST` and the feature list, `KVM_SET_CPUID2` and `KVM_GET_SUPPORTED_CPUID` from the host's CPUID with no paravirtual leaf at first, MP state, vCPU events, debug registers, TSS and identity-map addresses, `KVM_GET_CLOCK` and `KVM_SET_CLOCK`; check: QEMU's start-up sequence, taken from `strace` under Linux, replays through the host test.
  * 81-1a.6 The kernel's eventfd interface: take a reference from a descriptor, signal from the kernel, and a callback when a program signals it, for 81-4's irqfd and ioeventfd, 82-2's VFIO interrupts and 83-3b's doorbells; check: `eventfd_hosttest.c` rows for each, with a writer and a callback racing a close.
  * 81-1a.7 `kvm(4)`, `limits(7)` (vCPUs per VM, slots per VM) and `hardware.md` (VMX or SVM required); check: `make mancheck` and the i686 kernel built with no file of `kvm/`.
* Done: on Rach2 in QEMU, Debian's QEMU with `-accel kvm` opens `/dev/kvm`, finds every capability it requires, makes a VM and its vCPUs and reaches `KVM_RUN`, where the stub backend refuses with an error QEMU reports.

### 81-1b Memory slots and the guest-physical tables

Memory slots over process memory that follow every change to it, the guest-physical page tables that EPT and NPT share, dirty logging, slots over device mappings and over GPU objects with huge pages.

* Files: `rach2/sys/kvm/slots.c`, `tdp.c`, `dirty.c` (new, or the stubs of 81-1a), `rach2/sys/kvm/Readme.md` (its section), `man/man4/kvm.4` (its section), `tests/musl/kvmslotmusl.c` (new).
* Waits on: 81-1a (code); 76-2 (code: pinning, device mappings, huge pages and the notice of a region's change, which this task uses without editing `mm/`, Problems item 4).
* Labels: none.
* Subtasks:
  * 81-1b.1 `KVM_SET_USER_MEMORY_REGION`: slots made, moved, flagged read-only and deleted while vCPUs run; check: the musl test against Linux's rules for overlap and alignment.
  * 81-1b.2 The guest-physical tables built on fault from the slot's pages, with the entry format the backend gives, large pages where the host page and the slot allow; check: host test rows for 4 KiB, 2 MiB and mixed slots.
  * 81-1b.3 Slots follow copy-on-write, protection changes, `madvise`, truncation and unmap: vCPUs stopped and translations flushed before a frame is reused; check: the musl test forks, protects, drops and unmaps under a running guest and reads what the guest sees.
  * 81-1b.4 Dirty logging: `KVM_MEM_LOG_DIRTY_PAGES`, `KVM_GET_DIRTY_LOG` and `KVM_CLEAR_DIRTY_LOG`, which QEMU's VGA turns on at once (Problems, item 3); check: the selftest `dirty_log_test` in 81-2's run, and a host test row.
  * 81-1b.5 Slots over device mappings (`/dev/mem`'s `VM_PROT_IO` and later VFIO's BARs) mapped uncached with the guest's PAT in charge, never pinned as RAM; check: a slot over a QEMU BAR mapped from `/dev/mem`, its writes reaching the device.
  * 81-1b.6 Slots over a GPU object's huge-page mapping, mapped or refused with an error the VM survives; check: 76-2's huge-page fixture as a slot, the guest reading it or QEMU getting the error and going on.
* Done: a slot follows every change the plan lists (plan.md, Problems, item 4), dirty logging works, and a killed QEMU leaves no pinned page counted.

### 81-2 SVM

AMD's backend: the VMCB, nested paging, intercepts, the interrupt shadow, run under TCG and under WSL's nested KVM.

* Files: `rach2/sys/kvm/svm.c` (replacing 81-1a's stub), `rach2/sys/kvm/svm_run.s` (new, listed by 81-1a's manifest), the Readme's SVM section.
* Waits on: 81-1a, 81-1b, 81-4, 81-5 (code: the interface, memory, interrupts to inject and the MMIO decoder); 81-6b, 81-6c (code: QEMU and the corpus in the root, the TCG and nested-KVM profiles to run them).
* Labels: `linux-host`.
* Subtasks:
  * 81-2.1 Enable SVM per processor, the host save area, VMRUN with the guest's and host's state saved, host interrupts taken after the exit (no edit of `isr.c`); check: kvm-unit-tests' `sieve` under the TCG profile.
  * 81-2.2 Nested page tables in 81-1b's format, ASIDs and flushes by ASID where offered; check: `access` and `emulator`.
  * 81-2.3 Intercepts: I/O, MSR permission maps, CPUID, HLT, INVLPG, shutdown, XSETBV checked against the advertised features, WBINVD as Linux handles it; check: `msr`, `xsave`, the selftests `state_test` and `sync_regs_test`.
  * 81-2.4 Event injection and the interrupt shadow, with 81-4's LAPIC; check: `eventinj`, `apic-split`, `smptest`.
  * 81-2.5 The Debian guest and Rach2's i686 image to their tokens under the TCG profile on WSL and on the Mac, then under nested KVM on the testing PC, each a case of `machine.kvm.json` whose `requirements.hosts` names its hosts; check: 81-6c's runner reports both tokens on each.
  * 81-2.6 From the runs of 81-2.1 to 81-2.5 and 81-2.7, each timed as it ran, the minutes of 81's checkpoint slice estimated and recorded in `tests/kvm/` and the plan's Checkpoints; no extra run.
  * 81-2.7 `kvm:slots` and `kvm:leak` as cases of `machine.kvm.json`: 81-1b's slots over a `/dev/mem` device mapping and over a huge-page object with the guest going on in both, slots following fork, `mprotect` and `munmap` under a running guest, and QEMU killed under load with every pinned page, descriptor and mapping counted back; check: its author runs each once under the TCG profile.
* Done: under TCG on WSL and the Mac and under nested KVM, Debian and Rach2's i686 image boot under Rach2's SVM, the corpus's SVM rows pass or skip with a reason, and the slice's minutes are recorded.

### 81-3a VMX

Intel's backend: VMXON, the VMCS, EPT, unrestricted guest, VPID, the preemption timer, and EPT memory types as Linux sets them, proven by a host test that runs the trial's whole path on a VMX model. Split from its Lenovo step (81-3b), since 81-7 and 83-2b need the code.

* Files: `rach2/sys/kvm/vmx.c` (replacing the stub), `rach2/sys/kvm/vmx_run.s` (replacing 81-1a's stub), the Readme's VMX section, `tests/host/kvm_vmx_hosttest.c` (new) with a model of the Lenovo's VMX.
* Waits on: 81-1a, 81-1b, 81-4, 81-5 (code: the interface, memory, interrupts and the decoder).
* Labels: none.
* Subtasks:
  * 81-3a.1 VMXON after reading `IA32_FEATURE_CONTROL` (locked with VMX on, 0x5, on the Lenovo), the VMCS from the capability MSRs, VMXOFF on every exit path, every VMX instruction through one table of operations that the model stands in for; check: a review on the issue of each VMX instruction's failure path, line by line against Linux's `vmx.c`.
  * 81-3a.2 EPT with the entry format of 81-1b, VPID, unrestricted guest, the preemption timer bounding every guest run; EPT memory types write-back with the guest's PAT ignored until a non-snooping device is assigned, then the guest's PAT honoured, as Linux's `vmx_get_mt_mask`; check: the code beside Linux's, and a host test of the type chosen for each case.
  * 81-3a.3 Exits as 81-2's, with NMIs passed to the host and host interrupts taken after the exit; check: 81-3a.4's host test drives each exit's handler, since TCG has no VMX.
  * 81-3a.4 `kvm_vmx_hosttest.c`: a model of the Lenovo's capability MSRs and of VMXON, VMLAUNCH, VMRESUME, VMREAD, VMWRITE and their failures, through which the host test runs the trial's whole path, start to finish: VMXON, the VMCS built, a guest entered, each exit kind 81-6c's corpus rows cause handled, the preemption timer's exit, VMXOFF on every exit path and on panic, as the `machine` skill's readiness note asks; check: the host test passes.
* Done: the VMX host test runs the trial's whole path on the model, and each VMX instruction's failure path has its review on the issue.

### 81-3b VMX on the Lenovo

Rach2's VMX on the Lenovo by trial boot, silent and headless.

* Files: `dev-log/81-kvm/81-3.md` (new).
* Waits on: 81-3a (code); 81-6b, 81-6c (code: QEMU, the corpus and the runner); 76-15b (code: the trial root's slot); 76-15c (`evidence`: the data partition that carries QEMU on the Lenovo).
* Labels: `lenovo`.
* Subtasks:
  * 81-3b.1 The readiness note for the trial boot: why no new path can stop the machine (every wait bounded, the preemption timer armed, VMXOFF on panic), 81-3a.4's host test as the run of the same path, and the manager's go; hand back `owner-only`; check: the note and the go on the issue.
  * 81-3b.2 Lenovo step, silent and headless, a trial boot of its own, `mixer vol 1` after the boot: the trial kernel and root; the corpus rows of 81-6c; the Debian guest and Rach2's i686 image to their tokens with `-accel kvm`; check: the runner's tokens and the corpus's results in `81-3.md`.
* Done: on the Lenovo, by trial boot, Debian and Rach2's i686 image boot under Rach2's VMX and the corpus passes or skips with a reason.

### 81-4 Interrupts

The in-kernel local APIC with its timer, the split irqchip's routing, `KVM_SIGNAL_MSI`, irqfd and ioeventfd.

* Files: `rach2/sys/kvm/vlapic.c`, `irq.c`, `iobus.c` (replacing the stubs), the Readme's interrupt section, `man/man4/kvm.4` (its section), `tests/host/kvm_irq_hosttest.c` (new).
* Waits on: 81-1a (code).
* Labels: none.
* Subtasks:
  * 81-4.1 The local APIC: registers through MMIO and x2APIC MSRs, the timer in one-shot, periodic and TSC-deadline modes, `KVM_GET_LAPIC` and `KVM_SET_LAPIC`; check: a host test against the SDM's register rules.
  * 81-4.2 `KVM_ENABLE_CAP(KVM_CAP_SPLIT_IRQCHIP)`, `KVM_SET_GSI_ROUTING` with MSI routes, the EOI exit to QEMU for level-triggered lines; `KVM_CREATE_IRQCHIP` refused, so every QEMU command line names `kernel-irqchip=split` (Problems, item 5); check: host test rows.
  * 81-4.3 `KVM_SIGNAL_MSI` and `KVM_IRQFD` with resample, through 81-1a's eventfd callback; check: an interrupt raised by a write to the eventfd reaches the vLAPIC in the host test.
  * 81-4.4 `KVM_IOEVENTFD` on port and MMIO addresses with and without a data match; check: host test rows.
  * 81-4.5 The page and the Readme's section; check: `make mancheck`.
* Done: the host tests of the APIC, routing, irqfd and ioeventfd pass; 81-2 and 81-3b prove them with guests.

### 81-5 MMIO decoder

The instruction decoder for guest accesses to device memory, tested against a table of encodings.

* Files: `rach2/sys/kvm/decode.c` (replacing 81-1a's stub), `decode.h` (new), `tests/host/kvm_decode_hosttest.c` (new) with its table, `tests/fuzz/x86dec_fuzz.c` (new).
* Waits on: 81-1a (code: the stub and the manifest).
* Labels: none.
* Subtasks:
  * 81-5.1 Decode MOV of each size with sign and zero extension, MOVS, STOS, the arithmetic and logic group with a memory operand, TEST, CMP, XCHG, PUSH and POP of memory, in 16, 32 and 64-bit modes with prefixes; check: the table of encodings, each with its expected operand and length.
  * 81-5.2 Anything else is an internal error carrying the instruction's bytes, as Linux's `KVM_INTERNAL_ERROR_EMULATION`; check: table rows.
  * 81-5.3 The decoder's fuzzer; check: a fixed corpus replays clean under the sanitizers.
* Done: every row of the table decodes as written, and the fuzzer finds nothing.

### 81-6a Rach2's PCI devices found by one walk

One walk at boot that follows bridges, in place of one walk per driver over every bus; the SB16 probe's 40,400 reads of port 0x80 a boot cut; tried on the Lenovo by 81-6d.

* Files: `rach2/sys/pci.c`, `pci.h`, `rach2/sys/arch/x86/platform.c`, `rach2/sys/mm/pagecache.c` (`pc_probe_window_size` alone, as `run2/step5-7/pci-once.diff:26-85` does), `rach2/sys/audio/sb16.c`, `tests/host/` (a new `pciwalk_hosttest.c`), `rach2/hardware.md`.
* Waits on: nothing.
* Labels: none.
* Subtasks:
  * 81-6a.1 The walk from `run2/step5-7/pci-once.diff`, from bus 0 through each bridge's secondary bus, kept as one list that `pci_nth` and the finders read; check: a host test over a configuration-space model with bridges and an empty slot.
  * 81-6a.2 The SB16 probe's waits on port 0x80 replaced or skipped when no card answers; check: a count of port reads at boot under QEMU, before and after.
  * 81-6a.3 Under KVM on the testing PC at the second level, boot time and exits counted against step 7's 0.914 to 0.954 s and 148,401 to 149,035 exits; check: the counts recorded.
* Done: one walk finds every device on QEMU's machines, and a boot under KVM costs the exits step 7 measured with the patch; 81-6d proves the walk on the Lenovo.

### 81-6b QEMU and the corpus staged

Debian's QEMU through the fetcher, and kvm-unit-tests and the KVM selftests built from pinned sources and staged.

* Files: `vendor/gnu/kvm/` (new: Readme, `qemu.list` in 76-13's fetcher's format, the build of kvm-unit-tests and the selftests, their pins), `vendor/gnu/install_root.sh` (one call line), `man/programs.md` (made again).
* Waits on: 76-13a (code: the fetcher); the downloads card (`decision`: Debian's QEMU, kvm-unit-tests and the KVM selftests' sources).
* Labels: `linux-host`.
* Subtasks:
  * 81-6b.1 `qemu-system-x86`, `qemu-system-modules-opengl` (virtio-gpu-gl and the GL displays, which 83-2 needs), `qemu-utils`, SeaBIOS and OVMF with their closures; check: the fetcher's `check` step resolves QEMU's libraries inside the root.
  * 81-6b.2 kvm-unit-tests at the revision 80-2 pins, built for x86-64, with its `unittests.cfg`; check: its `x86/sieve.flat` boots under TCG on the build host.
  * 81-6b.3 The KVM selftests from the Linux source at the same revision 80-2 pins, built static with the GNU toolchain; check: each binary runs `--help` or its skip on a host without `/dev/kvm`.
  * 81-6b.4 The downloads and their sizes in the Readme, as the downloads card lists them; check: the Readme's table.
* Done: the root has Debian's QEMU with its GL modules, kvm-unit-tests and the selftests, each pinned.

### 81-6c Profiles and the guest runner

The outer profiles (TCG with `svm` and `npt`, qualified first; nested KVM on the testing PC with the SVM features WSL offers) and a runner on Rach2 that boots guests to a serial token.

* Files: `tools/etude/processor.c` (its only editor in the stack), `tests/kvm/` (new: the runner, the corpus's case lists, the guest images' recipes), `tests/cases/machine.kvm.json` (new), `tests/registry` (one row), `tests/system/suites.mk` (its block), `tests/Readme.md` (its line).
* Waits on: 76-16a (code: the KVM profile with `-cpu host`, 76-16a.2); 81-6b (code: QEMU and the corpus in the root).
* Labels: `linux-host`.
* Subtasks:
  * 81-6c.1 A processor profile with `svm` and `npt` under TCG, qualified by kvm-unit-tests' own `svm` tests run by the outer QEMU directly; check: those tests pass under TCG on WSL and on the Mac.
  * 81-6c.2 The nested-KVM profile on the testing PC with npt, nrip_save, tsc_scale, vmcb_clean, flushbyasid and the pause filter; check: the profile's guest reports those flags.
  * 81-6c.3 The runner: QEMU on Rach2 with `-accel kvm`, `kernel-irqchip=split` and a serial console, a guest booted to its token or failed on silence; check: a Debian guest under TCG inside Rach2 reaches its token.
  * 81-6c.4 The corpus's lists: the kvm-unit-tests and selftests that run, and each skip with its reason (nested virtualization, PMU, Hyper-V); check: the lists are in `tests/kvm/` and the suite `kvm` names them.
  * 81-6c.5 The family's line in `tests/registry`, its block in `suites.mk` and the Readme entry, each case with its `requirements.hosts`; check: `tests/run` lists the `kvm` cases.
* Done: the two outer profiles exist, the TCG one qualified, and the runner boots a guest to its token on Rach2.

### 81-6d The PCI walk and the VT-d units on the Lenovo

One silent, read-only trial kernel for 81-6a's walk and 82-1's reading of the DMAR table, so that neither code task, nor 82-2 after them, waits on the Lenovo.

* Files: `dev-log/81-kvm/81-6d.md` (new, the record).
* Waits on: 81-6a, 82-1 (code).
* Labels: `lenovo`.
* Subtasks:
  * 81-6d.1 Prepare the step: the trial kernel from the branch's head with both tasks on it and its SHA-256, the commands, what to read and what passes; the review note (the walk only reads configuration space; the DMAR code reads the table and both units' capability registers with translation off and writes no unit register) and the manager's go; hand back `owner-only`; check: the comment on the issue.
  * 81-6d.2 Lenovo step, silent, read-only, `mixer vol 1` after the boot: the trial kernel lists the same PCI functions as the boot before, and parses the DMAR table and reads both units' capability registers; check: `/proc/bus/pci/devices` or `/sys/bus/pci/devices` equal before and after, and the values equal Linux's log (`dev-log/70-wifi-chip/linux/dmesg.txt:190-200`: HAW 39, two units, two RMRRs, dmar0 ecap f0101a, dmar1 ecap f010da).
* Done: one walk finds every device on the Lenovo, and its VT-d units are read as Linux reads them.

### 81-7 Measurement

The exit cost and the selftests' timing cases on the Lenovo beside Linux's there, exits counted by kind, and the `/dev/kvm` fuzzer.

* Files: `tests/kvm/` (after 81-6c: the measurement scripts), `dev-log/81-kvm/81-7.md` (new).
* Waits on: 81-2, 81-3a, 81-4, 81-6a, 81-6c (code); 80-2, 81-3b, 81-6d (`evidence`: Linux's numbers, VMX and the walk proven on the Lenovo).
* Labels: `lenovo`.
* Subtasks:
  * 81-7.1 Exits counted by kind in the kernel and read through the debug files or `/proc`; check: a boot's counts by kind in QEMU.
  * 81-7.2 76-22's ioctl fuzzer run as an ordinary user against `/dev/kvm`, a VM and a vCPU; check: a fixed seed count with no fault or leak.
  * 81-7.3 On the testing PC, Rach2's and Linux's exit counts for the same guests under the same outer layers, never timed across layers; check: the counts in `81-7.md`.
  * 81-7.4 Lenovo step, silent, a trial boot of its own, `mixer vol 1` after the boot: `vmexit_*` and the selftests' timing cases, boot time, exit microbenchmarks and a workload's throughput, each apart; reusing 81-3b's trial kernel when nothing changed; check: each beside 80-2's Linux numbers in `81-7.md`.
  * 81-7.5 The big lock's cost on the hottest exits read from the counts, as risk 9 asks; check: the report says which exits a lock-free path would serve, with the numbers.
* Done: the exit costs on the Lenovo beside Linux's, counted by kind, and the fuzzer clean.

## 82 assign

The goal (plan.md, 82): QEMU on Rach2 gives a PCI device to a guest with DMA and interrupt isolation; on the Lenovo the guest is Debian with Linux's i915 driving the panel, and Rach2 gets the GPU back when the guest ends.

Five tasks: the plan's four, with 82-4 split into its code (82-4a) and its Lenovo step (82-4b). 82-1's read-only probe of the Lenovo's units is in 81-6d, one trial with 81-6a's walk, so that 82-2's code waits on no step on the Lenovo.

### 82-1 VT-d

The DMAR table, units, domains, queued invalidation, reserved regions for the devices their scopes name, interrupt remapping and fault reports, each unit's capabilities obeyed; first against QEMU's emulated intel-iommu.

* Files: `rach2/sys/arch/x86/iommu/` (new: `Readme.md`, `sources.mk`, `dmar.c` for the table, read through ACPICA's table access so that `acpi.c` is not edited, `vtd.c` for units, domains and invalidation, `intremap.c`, `fault.c`), `rach2/sys/iommu.h` (new: the interface VFIO uses), `rach2/sys/arch/x86_64/sources.mk` (one include line), `rach2/sys/arch/x86/ioapic.c` (the remapped redirection entries) and `rach2/sys/pci.c` and `pci.h` (hooks in `pci_msi_enable`, `pci.c:426`, and `pci_msix_enable`, `pci.c:535`, which writes its table entry from `pci_msi_message` itself and which NVMe (`nvme.c:1672`) and 76-7's virtio use, and a record of each enabled MSI and MSI-X entry, for remapped messages; far from 76-2b's and 81-6a's hunks), `rach2/hardware.md`, `rach2/cmdline.md` (`intel_iommu=off` as Linux's), `tests/host/vtd_hosttest.c` (new), `tests/host/acpi_qemu_tables.h` (a DMAR table from QEMU's intel-iommu).
* Waits on: nothing.
* Labels: none.
* Subtasks:
  * 82-1.1 The DMAR table's units, device scopes, RMRRs and the IOAPIC's and HPET's scopes; check: a host test on QEMU's table and on the Lenovo's (`dev-log/70-wifi-chip/linux/dmesg.txt:190-200`: HAW 39, two units, two RMRRs).
  * 82-1.2 Root and context tables, second-level page tables, domains, queued invalidation; tables flushed from the cache after each write when the unit's walks are not coherent (ecap bit 0, clear on both of the Lenovo's units) and the snoop bit left clear on a unit without snoop control (dmar0, ecap 0xf0101a); check: the host test against a model unit that records each flush.
  * 82-1.3 Translation turned on for a unit only while a device in its scope is given to a VM, so that Rach2's own drivers, and 78's gate, run with translation off (Problems, item 6). Since translation is per unit, every other device in that unit's scope gets a pass-through context entry, or an identity domain where the unit offers no pass-through, set before translation is turned on and kept through assignment and teardown, so that the host's own DMA goes on; each RMRR mapped only for the devices its scope names, the graphics RMRR reported relaxable when the GPU goes to a VM; check: the host test's domain contents for the assigned device and the unit's other devices.
  * 82-1.4 Interrupt remapping with compatibility-format interrupts refused (`GCMD.CFI` clear), as Linux sets it, since a compatibility-format message bypasses the table; while remapping is on, the host's IOAPIC entries, MSIs and MSI-X entries go through remapped entries of their own, through `ioapic.c` and the hooks in `pci_msi_enable` and `pci_msix_enable`; each enabled MSI and MSI-X entry, those written before remapping turns on among them, is rewritten in remapped format when it turns on, and in compatibility format when it turns off or the device releases the entry, whose remapping entry is then freed; check: host test rows for both paths through setup and teardown, and a QEMU boot with remapping on whose NVMe disk (MSI-X), AHCI disk (MSI) and network work under traffic.
  * 82-1.5 Fault reports read, counted and logged as `vtd: fault: ...`; check: a fault forced in the host test's model.
  * 82-1.6 Why Debian's kernel translated DMA on this Lenovo without `intel_iommu=on`, read in Linux's source; check: the reason, with file and line, in the Readme.
* Done: against QEMU's emulated intel-iommu Rach2's units come up, map and fault as Linux's; 81-6d reads the Lenovo's units as Linux reads them.

### 82-2 VFIO

Type1 containers, groups, vfio-pci's regions and interrupts through eventfds, the function-level reset, the IGD OpRegion region, BARs as device memory, a wider pool of host vectors, reserved regions as Linux reports them, one owner per device.

* Files: `rach2/sys/vfio/` (new: `Readme.md`, `container.c`, `group.c`, `pci.c`, `igd.c`), `rach2/sys/arch/x86_64/sources.mk` (one line), `rach2/sys/pci.c`, `pci.h` (the reset, after 81-6a), `rach2/sys/arch/x86/isr.h`, `isr.c`, `rach2/sys/arch/x86_64/isr_handler.s`, `idt.c`, `rach2/sys/arch/i686/isr_handler.s`, `idt.c` (the pool, `isr.h:98-99`), the nodes `vfio/vfio` and `vfio/N` through 76-1's devfs registry, the `vfio` row of 76-1's table of `/dev` directories, and `/sys`'s `iommu_group` links, `/sys/kernel/iommu_groups/N/` and `driver_override` through 76-9a's registration interface, so that 82-2 edits none of `devfs.c`, `synthfs.c`, `procfs.c` and `syscall_fs.c`, `man/man4/vfio.4` (new), `man/man5/sysfs.5`, `man/man7/limits.7`, `tests/host/vfio_hosttest.c` (new), `tests/host/pcimsi_hosttest.c`.
* Waits on: 82-1 (code: domains and remapping); 76-2 (code: pinning and device mappings); 81-1a (code: the kernel eventfd interface); 81-6a (code: `pci.c` after the walk); 76-9a (code: the `/sys` registration interface, Problems item 2); 79-9 (code: the x86-64 entry files, `idt.c` and `isr_handler.s`, after 79-1a, 79-3 and 79-9). `isr.c` is edited beside 76-19's return-to-user hook at far-apart hunks.
* Labels: none.
* Subtasks:
  * 82-2.1 The vector pool widened from 16 (`ISR_MSI_FIRST` 48 to `ISR_MSI_LAST` 63) to what the IDT can give, with 0x80 (79-1a's compat system call gate), the exceptions, the legacy IRQ vectors and the LAPIC's own vectors (timer, error, spurious, IPIs) kept out, vectors taken and given back with each interrupt setup; check: `pcimsi_hosttest.c` rows past 16 and back.
  * 82-2.2 `/dev/vfio/vfio`, groups as `/dev/vfio/N`, `/sys/bus/pci/devices/BDF/iommu_group` (QEMU reads it to find the group), `/sys/kernel/iommu_groups/N/{devices,reserved_regions,type}`, a device given to vfio-pci by `driver_override` and refused while Rach2's driver holds it; check: the classifier's host test and a musl row.
  * 82-2.3 Type1: `VFIO_SET_IOMMU`, `VFIO_IOMMU_MAP_DMA` and `UNMAP_DMA` with pinned pages, the IOVA ranges and reserved regions in `VFIO_IOMMU_GET_INFO`; check: host test against 82-1's model.
  * 82-2.4 vfio-pci: regions at Linux's offsets (index shifted by 40 bits, which fits Rach2's 32-bit page offset for indexes below 16), BARs mapped as device memory through 76-2, configuration space with the reset and the IGD OpRegion region; check: host test rows and QEMU's `vfio-pci` reading them in 82-3.
  * 82-2.5 Interrupts: INTx with masking, MSI and MSI-X into eventfds through remapped entries, torn down on release; check: host test rows.
  * 82-2.6 The function-level reset on release and on the owner's death, then the device's state checked; check: host test rows.
  * 82-2.7 `vfio(4)`, `sysfs(5)`, `limits(7)`; check: `make mancheck`.
* Done: the host tests pass, and QEMU's `vfio-pci` opens a device on Rach2 (proved with a guest in 82-3).

### 82-3 Assignment in QEMU

A nested guest given an emulated device through an emulated intel-iommu, under TCG and under the testing PC's nested KVM, compared before one is chosen; isolation faults counted.

* Files: `tests/assign/` (new: the outer machine's recipe, the guest's scripts), `tests/cases/machine.assign.json` (new), `tests/registry` (one row), `tests/system/suites.mk` (its block), `dev-log/82-assign/82-3.md` (new).
* Waits on: 82-2 (code); 81-6b, 81-6c (code: QEMU in the root and the runner); 81-2, 81-4 (code: SVM and interrupts, for the nested-KVM variant, Problems item 10); 76-8 (code: virtio-gpu's driver on 76-7's MSI-X transport, which 82-3.1's remapped host interrupts need).
* Labels: `linux-host`.
* Subtasks:
  * 82-3.1 The outer machine: q35 with `intel-iommu,intremap=on,caching-mode=on`, a NIC that Rach2 does not drive (`virtio-net-pci` or `pcnet`), QEMU's `edu` test device, and for the host an NVMe disk and `virtio-gpu-pci`, which Rach2 drives on MSI-X (76-7.4); check: Rach2 boots and lists the unit.
  * 82-3.2 The NIC given to a Debian guest of Rach2's QEMU under TCG; check: the guest's driver binds and pings the outer gateway.
  * 82-3.3 The same under Rach2's KVM on the nested-KVM profile, and the two compared by exits and time before one is chosen for the suite; check: the comparison in `82-3.md`.
  * 82-3.4 Isolation faults, with the host's disk and network under traffic throughout: `edu`'s DMA to an address outside its domain blocked and counted; an MSI with a wrong index blocked and counted; a compatibility-format MSI from the guest's device blocked and counted; check: the counts, Rach2's memory unchanged and the host's traffic without error.
  * 82-3.5 The guest killed under traffic: reset, the group back, pins and vectors back to their counts before; then the same device given to a second guest; check: the counts, and the second guest's driver binds. These are the cases `assign:kill` and `assign:reassign`.
  * 82-3.6 From the runs of 82-3.2 to 82-3.5, each timed as it ran, the minutes of 82's checkpoint slice recorded in `82-3.md` and the plan's Checkpoints; no extra run.
* Done: assignment works in QEMU on the chosen profile, and every isolation fault is blocked and counted.

### 82-4a GPU handoff

Rach2's Intel driver releases the GPU to vfio-pci and brings it up again from a reset device, proven by a host test of the whole path on the HD 4400 model; QEMU's command line for the Lenovo, tried in QEMU against 82-3's device. Split from its Lenovo step (82-4b), since 83-5b, 83-6c and 83-7 need the code.

* Files: `rach2/sys/drm/i915/i915_handoff.c` (new, Problems item 11), with one line in `rach2/sys/drm/i915/sources.mk`; `tests/host/i915_handoff_hosttest.c` (new) over 78's model; `tests/assign/` (the Lenovo's scripts, after 82-3).
* Waits on: 82-2 (code); 78-10b (code: the panel's full mode set); 82-3 (code: `tests/assign/` and the device the scripts are tried against).
* Labels: `linux-host`.
* Subtasks:
  * 82-4a.1 The driver's release (engines idle, interrupts off, the device handed to vfio-pci) and its bring-up again from a reset device, with no firmware state to adopt; check: `i915_handoff_hosttest.c` runs release, reset and bring-up on the HD 4400 model to a full mode set of the panel, start to finish, as the `machine` skill's readiness note asks.
  * 82-4a.2 QEMU's command line: `-machine q35,kernel-irqchip=split -nodefaults -vga none -device vfio-pci,host=0000:00:02.0,bus=pcie.0,addr=02.0,x-igd-opregion=on`, SeaBIOS and the guest's kernel; with a UEFI guest and the guest's own GRUB tried beside; check: the runner's scripts in QEMU against 82-3's device.
* Done: the handoff host test runs release, reset and bring-up on the model, and the scripts run in QEMU against 82-3's device.

### 82-4b The GPU to a Linux VM on the Lenovo

QEMU on q35 with SeaBIOS gives the Lenovo's GPU to Debian, whose i915 lights the panel; on a clean exit and on QEMU's death under load, the reset, then Rach2's full bring-up and mode set.

* Files: `dev-log/82-assign/82-4.md` (new).
* Waits on: 82-4a (code); 81-1b, 81-3a, 81-4, 81-6b, 81-6c (code: device slots, VMX, interrupts, QEMU, the runner); 76-15b (code: the trial root's slot); 76-15c, 78-10d, 81-3b, 81-6d (`evidence`: the data partition, the display, VMX and the VT-d units proven on the Lenovo).
* Labels: `lenovo`.
* Subtasks:
  * 82-4b.1 The readiness note: why each new path cannot stop the machine (the reset bounded, the bring-up's waits bounded, SSH kept), 82-4a.1's host test as the run of the same path, and the manager's go; hand back `owner-only`; check: the note and the go on the issue.
  * 82-4b.2 The paired Linux reference: glmark2 under Weston on translation-on Linux, with the guest's workload and settings, in a Debian boot of its own, or the stored 0.988 and 0.983 if their settings match the guest's; check: the numbers in `82-4.md`.
  * 82-4b.3 "Lenovo notice:" ten minutes before, since the guest takes the screen; check: the comment's time.
  * 82-4b.4 Lenovo step, a trial boot of its own, `mixer vol 1` after the boot: the guest's i915 lights the panel and runs glmark2 under Weston, its ratio recorded against 82-4b.2's reference; then a clean exit, then QEMU killed under load, each followed by the reset, Rach2's bring-up, a pipe CRC equal to the same mode's before, and a GL readback on Crocus; check: the ratio, the CRCs, the readbacks and no DMAR fault counted.
* Done: as 82's Done (plan.md, 82), with the ratio taken against a separate Debian boot.

## 83 testbed

The goal: as "The goal as a researcher sees it" in plan.md.

The plan's seven tasks become thirteen. 83-1, 83-2, 83-3, 83-5 and 83-6 each mix work that can start on Linux at once, or on the testing PC, with work that needs Rach2's hypervisor or the Lenovo. The bed's own userland goes in a new top-level directory, `bed/`.

### 83-1a The vtest relay and its Linux fixture

The relay from vtest's local socket to a TCP connection that hides the version query, so both sides keep to protocol 0, and its fixture between two Linux machines.

* Files: `bed/Readme.md` (new), `bed/vtestrelay.c` (new, POSIX C that builds for Linux and for Rach2's GNU root), `bed/fixture/` (new: the Debian guest's recipe with virglrenderer's test server), `Readme.md` (one row of the source table), `man/man1/vtestrelay.1` (new).
* Waits on: nothing.
* Labels: `linux-host`.
* Subtasks:
  * 83-1a.1 The relay: Mesa's client on the Unix socket, the server over TCP, the version query answered as protocol 0; check: a Linux host's virpipe reaches a Debian guest's `virgl_test_server` over QEMU's user networking.
  * 83-1a.2 Resource creation, readback and fences through the relay; check: a triangle's pixels read back equal to llvmpipe's.
  * 83-1a.3 The fixture's recipe kept so that 83-1b and 83-7 boot the same guest; check: the recipe makes the guest from Debian's cloud image with its pin.
  * 83-1a.4 `vtestrelay(1)` and `bed/Readme.md`; check: `make mancheck`.
* Done: between two Linux machines, the relay carries a virgl triangle and its fences with the right pixels.

### 83-1b The first rung on Rach2

Mesa's virpipe on Rach2 through the relay to the guest's server, under TCG with llvmpipe in the guest and under KVM.

* Files: `bed/` (the Rach2 side's scripts), `vendor/gnu/install_root.sh` (one line: the relay staged), `tests/cases/machine.bed-vtest.json` (new), `tests/registry` (one row), `tests/system/suites.mk` (its block).
* Waits on: 83-1a (code); 81-6b, 81-6c (code: QEMU and the runner on Rach2); 81-2, 81-4 (code: KVM on the testing PC).
* Labels: `linux-host`.
* Subtasks:
  * 83-1b.1 The relay in the GNU root; check: the root's `vtestrelay --version`.
  * 83-1b.2 The guest under TCG in Rach2's QEMU; check: the triangle's pixels.
  * 83-1b.3 The same under Rach2's KVM; check: the same pixels.
  * 83-1b.4 The `bed-vtest` family with these cases and per-case `requirements.hosts`; check: `tests/run` lists its cases, and its author runs one once. The release runs the family whole.
* Done: a program on Rach2 renders through the guest by vtest with verified pixels, under TCG and under KVM. The rung over the assigned GPU is measured in 83-7 (Problems, item 13).

### 83-2a The forward direction on the testing PC

A Debian guest of QEMU on Rach2 uses Rach2's GPU through QEMU's own virtio-gpu, virgl then Venus, as the comparison for the reversed direction. Split from the plan's 83-2 so that the testing PC's half waits on none of 78's Lenovo work.

* Files: `bed/forward/` (new: QEMU's command lines and the guest's scenes), `tests/cases/machine.bed-forward.json` (new, with its line in `tests/registry` and block in `suites.mk`), `dev-log/83-testbed/83-2.md` (new).
* Waits on: 81-2, 81-4, 81-6b, 81-6c (code: KVM on the testing PC, QEMU with its GL modules); 76-3, 77-2, 77-3 (code: dma-buf and udmabuf, and a render node through virtio-gpu, which QEMU's GL needs, Problems item 10).
* Labels: `linux-host`.
* Subtasks:
  * 83-2a.1 On the testing PC, QEMU on Rach2 with `virtio-gpu-gl` and `egl-headless` on Rach2's render node; check: glmark2's one scene in the guest with an in-guest checksum.
  * 83-2a.2 Venus with blobs on the same; check: vkcube off-screen read back.
  * 83-2a.3 The `bed-forward` family with 83-2a.1's and 83-2a.2's checks; check: `tests/run` lists its cases.
* Done: the forward direction draws right on the testing PC, its rates recorded in `83-2.md` for 83-7.

### 83-2b The forward direction on the Lenovo

The same on the Lenovo's Crocus and hasvk.

* Files: `bed/forward/` (the Lenovo's command lines, after 83-2a), `dev-log/83-testbed/83-2.md` (after 83-2a).
* Waits on: 83-2a (code); 78-8b, 78-10b, 78-11a (code: Crocus's submission for ordinary users, the display and hasvk's kernel pieces); 81-3a (code: VMX); 76-15b (code: the trial root's slot); 78-6c, 78-11c, 81-3b, 76-15c (`evidence`: ordinary users' submissions, hasvk, VMX and the data partition proven on the Lenovo).
* Labels: `linux-host`, `lenovo`.
* Subtasks:
  * 83-2b.1 Prepare the step: the trial kernel and the trial root with QEMU and its GL modules, the commands, what to read and what passes; "Lenovo notice:" ten minutes before if the guest's display is shown; check: the comment's time, or a headless display named.
  * 83-2b.2 Lenovo step, a trial boot of its own, `mixer vol 1` after the boot: the same scenes on Crocus and hasvk; check: the checksums and the rates in `83-2.md`.
* Done: the forward direction draws right on the Lenovo, its rates recorded for 83-7.

### 83-3a The transport's format and a Linux peer

The shared region's layout, fixed first so that the two sides fan out: the control area with roles, peer ids, a version and a connection epoch, feature negotiation, reset and queue setup, offsets as DMA addresses; and a small driver-side peer on Linux that sends 2D commands through it.

* Files: `bed/shm/vgpushm.h` (new: the layout, used by Rach2's kernel, the daemon, the VM's backend and the peer), `bed/shm/Readme.md` (new), `bed/shm/peer.c` (new), `bed/shm/corpus/` (new: seed inputs for both fuzzers).
* Waits on: nothing.
* Labels: none.
* Subtasks:
  * 83-3a.1 The layout and its rules: every offset and length checked, lifetimes and epochs in the transport's tables, never in virtio-gpu's commands, quotas; check: the header compiles under C89 for the kernel and C99 for Linux, and the Readme states each rule.
  * 83-3a.2 The peer: it maps a region, plays the driver's side through the handshake, and sends `RESOURCE_CREATE_2D`, `ATTACH_BACKING`, `TRANSFER_TO_HOST_2D`, `SET_SCANOUT` and `RESOURCE_FLUSH`, and replays a recorded virgl command stream; check: it runs against a loopback responder in the same file.
  * 83-3a.3 Seed inputs for the fuzzers; check: the corpus parses with the header's checks.
* Done: one header and one document fix the region, and a Linux program speaks the driver's side of it.

### 83-3b Rach2's side of the transport

Rach2's virtio-gpu driver over virtqueues in one shared region: commands and backing allocated in the region, the ivshmem daemon and the attach call, in-kernel eventfd signals and waits, a fixed pair of doorbell vectors, failure on the VM's death, a fuzzer on Rach2's parser.

* Files: a new transport file beside 76-7's virtio-pci (`shmem.c`; Problems item 11), `bed/ivshmemd.c` (new: QEMU's ivshmem server and the host's peer), a control node `/dev/vgpu-shm` (a row in 76-1's device registry, not `devfs.c`) and its page `man/man4/vgpu-shm.4` (new), `man/man8/ivshmemd.8` (new), `vendor/gnu/install_root.sh` (one line), `tests/fuzz/vgpushm_fuzz.c` (new), `tests/cases/machine.bed-shm.json` (new, with its line in `tests/registry` and block in `suites.mk`).
* Waits on: 83-3a (code); 76-3, 77-3 (code: buffers and fences, blobs and context init); 81-1a, 81-4 (code: kernel eventfds, ioeventfd and irqfd); 81-2 (code: SVM, for 83-3b.3's round trip under Rach2's KVM); 81-6b, 81-6c (code: QEMU and the runner on Rach2); 83-4 (code: the VM's backend, for the end-to-end proof).
* Labels: `linux-host`.
* Subtasks:
  * 83-3b.1 The transport under 76-7's transport operations, with the driver's allocations taken from the region and ENOMEM when it is full; check: a host test with a full region.
  * 83-3b.2 The daemon as QEMU's ivshmem server (its documented protocol) and the attach call that hands the memfd and the doorbells to the driver; check: QEMU's `ivshmem-doorbell` connects and the attach succeeds once and is refused twice.
  * 83-3b.3 Doorbells as ioeventfd and notifications as irqfd under Rach2's KVM, polling first; check: a round trip counted without an exit to QEMU.
  * 83-3b.4 The VM's death: every pending fence failed, every resource freed, the epoch moved; check: QEMU killed with fences pending, every waiter gets an error.
  * 83-3b.5 The fuzzer on Rach2's parser; check: a fixed seed count clean under the sanitizers.
  * 83-3b.6 End to end with 83-4's backend: a Rach2 program draws through the VM, virgl with llvmpipe in the VM; check: pixels read back equal to llvmpipe's.
  * 83-3b.7 The pages, and the `bed-shm` family with 83-3b.3's, 83-3b.4's and 83-3b.6's checks; check: `make mancheck`, and `tests/run` lists its cases.
* Done: on the testing PC, a program on Rach2 renders through the VM over the transport with verified pixels, and the VM's death fails every wait.

### 83-4 The VM's backend

The virtio-gpu device backend around libvirglrenderer in the VM, from QEMU's virglrenderer glue; interrupts through the VM's small kernel module; GL and Venus resources exportable as dma-bufs; Rach2's frames on the VM's KMS; Rach2's network and serial kept.

* Files: `bed/vm/backend/` (new), `bed/vm/module/` (new, GPL-2, built against Debian's kernel headers), `bed/vm/image/` (new: the VM image's recipe from Debian's cloud image), `tests/fuzz/` (the backend's fuzzer, built on Linux).
* Waits on: 83-3a (code: the format and the peer).
* Labels: `linux-host`.
* Subtasks:
  * 83-4.1 The VM's image: Debian 13 with virglrenderer, Mesa and the build of the backend and module, pinned; check: the image boots to its token in QEMU on WSL.
  * 83-4.2 The module's interrupt part: the ivshmem device's MSI-X vectors delivered to the backend as eventfds, since Debian's kernel has no vfio no-IOMMU mode; with vfio-pci over QEMU's emulated intel-iommu as the second way; check: the peer's doorbell wakes the backend.
  * 83-4.3 The backend: the control area, the queues, offsets checked, virgl, Venus and context init through libvirglrenderer; check: the peer's 2D commands and its recorded virgl stream draw right.
  * 83-4.4 Resources that GL and Venus share made exportable as dma-bufs (Debian's virglrenderer lacks `minigbm_allocation`); check: a GL-made buffer imported by Venus in the VM.
  * 83-4.5 Rach2's frames on the VM's KMS as the DRM master, the physical mode chosen from the scanout and the EDID, copy first, then scanning out the frame's buffer; counters; check: the peer's scanout appears with the right CRC on a virtual display.
  * 83-4.6 The backend's fuzzer; check: a fixed seed count clean under the sanitizers.
* Done: against the Linux peer, the backend renders and scans out with verified pixels, and its parser survives the fuzzer.

### 83-5a Buffers without copies

Map requests answered with lists of VM-physical pages by the VM's module, resolved by Rach2's KVM through its slots into frames mapped into the program as one object it can revoke; on the testing PC.

* Files: `rach2/sys/kvm/gpamap.c` (new: VM pages into a program, through 81-1b's slot lookup and 76-2's revocable mapping), `rach2/sys/kvm/sources.mk` (one line), the transport file of 83-3b (after it), `bed/vm/module/` (its page-list part), `dev-log/83-testbed/83-5.md` (new).
* Waits on: 83-3b, 83-4 (code); 81-1b, 76-2 (code).
* Labels: `linux-host`.
* Subtasks:
  * 83-5a.1 The module pins a mappable buffer through its dma-buf or memfd and reports its pages; check: Venus's host-visible memory on lavapipe reported as a page list.
  * 83-5a.2 Rach2 resolves the pages through the slots and maps them into the program with the cache mode of the GPU's mapping, held by page references, torn down on release, the VM's death and a slot's change; check: a program's writes seen in the VM and back.
  * 83-5a.3 Buffers that cannot be exported refused; fence order kept; check: rows for each refusal and a fence-ordered write.
  * 83-5a.4 The VM killed with mappings live: the program's access fails as Linux's revoked mappings do, nothing leaks; check: pinned-page counts back to before, as a case added to `machine.bed-shm.json` after 83-3b.
  * 83-5a.5 Measured against copying and against buffers allocated in the shared region, serial allocation apart from kept mappings; check: the numbers with QEMU's version and host in `83-5.md`.
* Done: on the testing PC, a Venus buffer of the VM is mapped into a Rach2 program, coherent both ways, and revoked cleanly when the VM dies.

### 83-5b Buffers without copies on the Lenovo's GPU

The same on a real GPU buffer, hasvk in the VM that owns the HD 4400, against Linux's baseline.

* Files: `dev-log/83-testbed/83-5.md` (after 83-5a).
* Waits on: 83-5a, 82-4a (code: the page lists and the handoff); 82-4b (`evidence`: the GPU given to a VM on the Lenovo).
* Labels: `lenovo`.
* Subtasks:
  * 83-5b.1 The trial root and the scripts; check: they run in QEMU.
  * 83-5b.2 "Lenovo notice:" ten minutes before, since the VM owns the screen; check: the comment's time.
  * 83-5b.3 Lenovo step, one trial boot of its own for 83-5b.3 and 83-5b.4, `mixer vol 1` after the boot: CPU-to-GPU and GPU-to-CPU visibility on a hasvk buffer; the VM killed with mappings live; check: the readbacks and the counts.
  * 83-5b.4 Lenovo step: blobs of 4 KiB to 64 MiB freed before the next, 32 held blobs, bandwidth through a 64 MiB mapping; check: each beside Linux's baseline in plan.md's What the second run changed in `83-5.md`.
* Done: as the plan's 83-5 Done on the Lenovo, with every figure beside Linux's.

### 83-6a The matched native-context stack

The research stack kept apart from the product's pins: Mesa 26.1.6 with `intel-virtio-experimental` for Rach2, virglrenderer 1.3.0 with `drm-renderers=i915-experimental` and trixie-backports' kernel for the VM, QEMU 11.1 from trixie-backports for the check.

* Files: `bed/nctx/` (new: the pins, the builds and the Readme).
* Waits on: the downloads card (`decision`: the sources of Mesa 26.1.6 and virglrenderer 1.3.0, trixie-backports' kernel, QEMU 11.1).
* Labels: `linux-host`.
* Subtasks:
  * 83-6a.1 Mesa 26.1.6 built with the Intel virtio option for Rach2's GNU root, kept apart from 76-13's pins; check: Crocus's `virtio` path present in the built driver.
  * 83-6a.2 virglrenderer 1.3.0 and QEMU 11.1 for Debian with the backports kernel, and the VM image's variant; check: QEMU lists the native-context capset when started on a host with i915.
  * 83-6a.3 The downloads and sizes in the Readme, as the downloads card lists them; check: the table.
* Done: the matched stack builds and is pinned.

### 83-6b Native context proved on Haswell under Linux

The stack checked in the usual direction on Linux, on the only Haswell there is: capsets and a triangle, then memory pressure, rebinding, repeated submissions and fence errors.

* Files: `bed/nctx/` (the check's scripts), `dev-log/83-testbed/83-6.md` (new).
* Waits on: 83-6a (code).
* Labels: `lenovo`, `owner-only` (a Debian session on p5, Problems item 12).
* Subtasks:
  * 83-6b.1 The session's scripts rehearsed in a Debian guest; the backports kernel installed on p5's Debian and removed afterwards; check: the rehearsal runs to its end.
  * 83-6b.2 Lenovo step, headless, a Debian boot of its own: capsets and a triangle, then pressure, rebinding, repeated submissions and fence errors; check: each result in `83-6.md`.
* Done: the rung's entry condition (plan.md, Risks, item 4) is met or refused with the reason.

### 83-6c Native context in the reversed direction

The rung over Rach2's transport on the Lenovo, with the relocation cost measured.

* Files: `bed/nctx/` (Rach2's side), `dev-log/83-testbed/83-6.md` (after 83-6b).
* Waits on: 83-5a, 82-4a (code: mappable buffers through page lists, and the handoff); 83-6b, 82-4b (`evidence`: the rung enters only after 83-6b, and the GPU given to a VM on the Lenovo).
* Labels: `linux-host`, `lenovo`.
* Subtasks:
  * 83-6c.1 Crocus's virtio path on Rach2 over the transport, built from 83-6a; check: in QEMU it reports the native-context capset as absent and falls back plainly.
  * 83-6c.2 "Lenovo notice:" ten minutes before; check: the comment's time.
  * 83-6c.3 Lenovo step, a trial boot of its own, `mixer vol 1` after the boot: a triangle and glmark2 through native context with verified pixels, the relocation cost per batch measured; check: the numbers in `83-6.md`.
* Done: native context runs reversed on the Lenovo with verified pixels and its relocation cost recorded, or 83-6b's refusal stands.

### 83-7 Measurement and device loss

The same scenes native on Rach2, native on Linux, forward, and reversed by each protocol; the VM killed under each load, the session restarting Weston on llvmpipe and then on Rach2's own driver; the report.

* Files: `bed/measure/` (new: the harness), 76-14's session (after 76-14b and 80-4: the restart on the VM's death), a case in `machine.bed-shm.json` (after 83-5a), `dev-log/83-testbed/report.md` (new).
* Waits on: 83-1b, 83-2a, 83-5a, 82-4a (code: the vtest rung, the forward scenes, the buffers and the handoff); 76-14b, 80-4 (code: the session it extends, after 80-4's change to it, Problems item 10); 83-2b, 83-5b, 83-6c, 82-4b (`evidence`: the Lenovo's runs it measures beside).
* Labels: `lenovo`.
* Subtasks:
  * 83-7.1 The harness: glmark2, piglit's chosen tests, a WebGL scene and a game route, each writing frame times, CPU per frame, latency, bytes copied, exits and ring round trips; check: one line of numbers per run in QEMU.
  * 83-7.2 The session restarts Weston on llvmpipe when the VM dies, and on Rach2's driver once it has the GPU back; check: in QEMU, the VM killed under load and Weston back on llvmpipe, as the `bed-shm` case `vm-death`; from this run and 83-7.1's, each timed, the minutes of 83's slice recorded in the plan's Checkpoints.
  * 83-7.3 The vtest rung over the assigned GPU, moved here from 83-1 (Problems, item 13); check: its numbers.
  * 83-7.4 "Lenovo notice:" ten minutes before; check: the comment's time.
  * 83-7.5 Lenovo step, one trial boot of its own for 83-7.5 and 83-7.6, `mixer vol 1` after the boot: every rung's numbers, each Linux figure with its IOMMU configuration named; check: the table in `report.md`.
  * 83-7.6 Lenovo step: the VM killed under each load; every wait fails, Weston restarts on llvmpipe, then on Rach2's driver with the panel lit; check: the session's log and a pipe CRC.
* Done: as 83's Done (plan.md, 83).

### Checkpoint slices for 80, 81, 82 and 83

Each slice is a fixed list of cases, given to `tests/run` by `RACH2_CASES`, that proves its milestone's goal in minutes. In QEMU on WSL unless said. A Lenovo trial boot only where the milestone touched the Lenovo's hardware.

#### 80, ten cases from 80's families, on the virgl profile

* `games-wine:d3d9-64` and `games-wine:d3d9-32`: the pinned Wine draws Direct3D 9 through Xwayland on virgl, 64-bit and under WoW64, checksums against Linux's in the same QEMU (80-3).
* `games-wm:fullscreen`: `xplace` at 1366x768 at the origin, unframed, focused and confined, with relative motion (80-4).
* `games-wm:tone`: a Wine program's tone recorded through the loopback (80-4).
* `games-umu:d3d9`: the Direct3D 9 probe in the pinned runtime with the same settings as directly (80-8b).
* `nslimits`: `nsmusl` and 80-8a.3's bubblewrap row: the 33rd mount namespace and a 340-extent map (80-8a).
* `xattr`: 80-10a's host rows: user attributes on ext2 set, read, listed and removed, a forced stop, then `e2fsck -fy` and a clean second `e2fsck -fn` (80-10a).
* `wine-gaps`, 80-10b's musl rows: comm written, `STATX_MNT_ID`, `fdinfo`'s `mnt_id`, `name_to_handle_at`, a renamed file's `/proc/PID/fd` link and the new `/sys` files, against Linux's answers (80-10b).
* `games-native:replays`: SuperTuxKart's and 0 A.D.'s replays run to their end on llvmpipe (80-7c).
* `save-update`, 80-9.1's rehearsal and family: a prefix and a save on a data-partition image survive a rehearsed update (80-6, 80-9).
* Why these: each is the smallest run of one part of the route that 80-9 uses, so a review fix that breaks the route breaks a case. 80 changes no Lenovo driver, so no trial boot of its own; if the fix batch touches 76-14's session, 78's driver, 80-3's or 80-4's route or 80-8b's container, 80-9.3 runs again on a trial root, and the manager decides whether a fix touches them.

#### 81, twenty-two cases of the `kvm` family, on the TCG profile with `svm` and `npt`, in the minutes 81-2.6 records

* kvm-unit-tests, ten: `msr`, `apic-split`, `ioapic-split`, `emulator`, `eventinj`, `xsave`, `smptest`, `tsc`, `realmode`, `vmexit_cpuid`. They cover the state ioctls, the vLAPIC and the split irqchip, the decoder, injection, XSETBV, several vCPUs, the TSC, unrestricted entry and one exit's count.
* KVM selftests, eight: `kvm_create_max_vcpus`, `set_memory_region_test`, `dirty_log_test`, `state_test`, `sync_regs_test`, `xapic_state_test`, `cpuid_test`, `userspace_io_test`. They cover the slots, dirty logging, save and restore, the run page and the exits to QEMU.
* `kvm:guests`: Debian's cloud image to its login token and Rach2's i686 `os-simple` to its prompt under `-accel kvm`.
* `kvm:slots` (81-2.7): a slot over a `/dev/mem` device mapping and one over a huge-page object, the guest going on in both, and slots following fork, `mprotect` and `munmap`.
* `kvm:leak` (81-2.7): QEMU killed under load, then no pinned page, descriptor or mapping left.
* `kvm:nested`: on the testing PC's nested-KVM profile, the Debian guest to its token once, the check against real SVM.
* With them, the host suite's `kvm_decode_hosttest`, `kvm_hosttest` and `kvm_irq_hosttest`, and the decoder's corpus.
* Lenovo trial boot, silent and headless: the same eighteen corpus rows under VMX, both guests to their tokens, and `vmexit_*` timings recorded beside 80-2's (not judged). 81 changed the PCI walk and added VMX, both of which run on the Lenovo's hardware.

#### 82, seven cases of the `assign` family, TCG inside Rach2, about 4.1 minutes from the author case timings

* `machine.assign.boot`: Rach2 boots with the emulated unit and its disks, network and display work under remapping, the NVMe disk and virtio-gpu on MSI-X among them (82-1.4, 82-3.1).
* `machine.assign.nic-tcg`: the NIC given to a Debian guest, which binds and pings.
* `machine.assign.dma`: `edu`'s DMA outside its domain blocked and counted.
* `machine.assign.index`: an MSI with a wrong index blocked and counted.
* `machine.assign.compat`: a compatibility-format MSI blocked and counted, the host's disk and network under traffic throughout and without error.
* `machine.assign.kill`: the guest killed under traffic, the group back, pins and vectors back to their counts.
* `machine.assign.reassign`: the same device given to a second guest after the reset (82-3.5).
* Lenovo trial boot, announced since the guest takes the screen: 82-4b's route once, the GPU to Debian, glmark2's ratio recorded, a clean exit and a kill under load each ending with Rach2's pipe CRC equal to the mode's before. 82 touched the VT-d units and the GPU.

#### 83, seven cases of the `bed-*` families, in the minutes 83-7.2 records

* `bed-vtest:triangle`: a virpipe triangle through the relay, pixels checked (83-1b).
* `bed-forward:virgl`: a Debian guest of QEMU on Rach2 draws through Rach2's virgl, in-guest checksum (83-2a).
* `bed-shm:virgl`: a Rach2 program draws through the VM over the transport, pixels checked (83-3b).
* `bed-shm:venus`: vkcube off-screen through the VM, read back (83-3b, 83-4).
* `bed-shm:nocopy`: a Venus buffer of the VM mapped into a Rach2 program, coherent both ways, then the VM killed with the mapping live (83-5a).
* `bed-shm:vm-death`: the VM killed under glmark2, every wait failed, Weston back on llvmpipe (83-7).
* `bed-shm:fuzz`: both parsers' fuzzers for a fixed seed count.
* Lenovo trial boot, announced: one scene through the VM on the panel with a CRC, Venus read back on hasvk through the VM, the VM killed under load and the panel back under Rach2 with Weston on Rach2's driver.

### Order and parallelism

#### Starting at once

* No wait at all: 80-2 (on the Lenovo's Debian), 81-6a, 82-1, 83-1a, 83-3a, and 83-6a once the downloads card is answered.
* As soon as an early task of 76 closes: 80-7a and 81-6b (76-13a's fetcher); 80-10a (76-15a's forced ext2 test); 81-1a (76-1, 76-2a).
* As soon as one of the above closes: 81-4 and 81-5 (81-1a); 80-8a (80-10a, 76-18a); 83-4 (83-3a); 83-6b (83-6a, a Lenovo step); 81-1b (81-1a and 76-2b).

#### Critical paths, in tasks

* 80: inside the milestone 80-7a, 80-1, 80-3, 80-8b, 80-9, five tasks; across the stack 76-1, 76-2a, 76-4a, 78-2a, 78-2b, 78-3a, 78-5a, 78-5b, 78-8b, 78-11a, 78-11c, 80-5, 80-9, thirteen with the waits on evidence, the longest of the stack with 83-7's; by code alone eleven, through 78-8a and 78-12a.
* 81: 76-1, 76-2a, 81-1a, 81-1b, 81-3a, 81-3b, 81-7, seven tasks, with 81-4, 81-5 and 81-6a to 81-6c beside them.
* 82: 76-1, 76-2a, 76-2b, 79-1b, 79-4, 79-3, 79-9, 82-2, 82-3, 82-4a, 82-4b, eleven tasks, since 82-2 takes the x86-64 entry files after 79-9; 82-4a's wait on 78-10b is shorter.
* 83: 76-1, 76-2a, 76-4a, 78-2a, 78-2b, 78-3a, 78-5a, 78-5b, 78-8b, 78-11a, 78-11c, 83-2b, 83-7, thirteen with the waits on evidence; by code alone eleven, through 82-4a. 83-5b and 83-6c run beside 83-2b.

#### The Lenovo's queue, 80 to 83

Steps of 76 to 79 interleave; these are 80 to 83's, in the order their waits allow. A loop that hands a task back moves on, so the machine is never why a worker waits.

1. 80-2: the Debian session, silent steps first (81-1's KVM trace among them), then the announced screen steps.
2. 81-6d: one read-only trial kernel from the branch's head once 81-6a and 82-1 are on it; silent.
3. 83-6b: a second Debian session, headless, with trixie-backports' kernel removed afterwards; 78-1b's Debian expiry is a boot of its own (78's L1).
4. 80-6: the install onto the data partition, silent.
5. 80-5: Crocus probes under a headless Weston, silent.
6. 81-3b, then 81-7, each a trial boot of its own, 81-7 on 81-3b's kernel when nothing changed between them; silent.
7. 80-7b, announced (sound at volume 1 and the screen).
8. 80-9, announced.
9. 82-4b, announced, after its Debian reference boot.
10. 83-2b, 83-5b, 83-6c and 83-7, on the same trial kernel and root where they are ready together, each in a boot of its own since each takes the GPU; announced.
11. The sitting is milestone 84, filed after the stack's release (the Sitting section below); nothing of 76 to 83 waits on it.

#### What would make workers wait or collide, and the fix

* Files outside the plan's list that two tasks running at once would edit; each is answered by the rule at the top of this file or by an order above:
  * `rach2/sys/fs/devfs.c`: 76-1 and 76-2a alone; 76-3, 76-4, 78-1a, 81-1a, 82-2 and 83-3b register through 76-1's device registry.
  * `/dev` subdirectories in `synthfs.c` (`:1543`) and `syscall_fs.c` (`:13909`), where `input` is a special case: 76-1.5 makes them a table with `dri` its row, so 82-2 adds `vfio` as a row.
  * `/sys` in `procfs.c` and `synthfs.c`: 76-9a, then 76-9b, then 80-10b; 76-19 edits neither, and 82-2 registers through 76-9a.6's interface.
  * `syscall_fs.c`: 76-1 (the `/dev` table near :13909), 76-3a (lseek and fstat on anonymous descriptors) and 80-10a (`xattr_call` and the calls at :4616-4708) edit far-apart hunks and take no order; 80-10b comes after 80-10a; 82-2 adds only a row of 76-1's table.
  * `rach2/sys/fs/vfs.h`: 80-10a, then 80-8a; `vfs.c`: 76-18a, then 80-8a.
  * `rach2/sys/pci.c`: 76-2b's BAR windows and 81-6a's walk edit different functions and take no order; 76-7 keeps its MSI-X in `rach2/sys/virtio/virtio_pci.c` and edits no `pci.c`; 82-1's hooks in `pci_msi_enable` and `pci_msix_enable` are far from both; 82-2 comes after 81-6a.
  * `rach2/sys/io/eventfd.c`: 81-1a alone; 82-2 and 83-3b use its interface.
  * `tools/etude/processor.c`: 81-6c alone; 76-16a edits `qemu_profile.c`, not this file.
  * `vendor/gnu/install_root.sh` and the fetcher's lists: one call line per layer, and one list file per task (`games/layer.list`, `games/native.list`, `games/umu.list`, `kvm/qemu.list`, and 77's to 79's under `debian/`), which 76-13a.1's fetcher reads (Problems, item 2).
  * `man/man7/limits.7`, `man/man5/sysfs.5`, `man/man4/kvm.4`, `man/man1/rach2-game.1` and `vendor/gnu/Readme.md`: a section per task, under the section rule.
  * The x86-64 entry files (`idt.c`, `isr_handler.s`, `percpu.*`): 79-1a, 79-3, 79-9, then 82-2; `isr.c`: 76-19 and 82-2 at far-apart hunks.
  * `rach2/sys/kvm/sources.mk`: 81-1a lists every file of 81 and stubs them, so no later task of 81 edits it; 83-5a adds one line.
  * 76-14's session: 80-4, then 83-7.
* The mapping code in `mm/`: 76-3, 77-3 and 81-1b all wait on 76-2 and could then run at once in `mm/`. Fixed: 76-2b.7 delivers the change notice, so none of them edits `mm/`.
* 81-2 and 81-3b wait on 81-6b, which waits on 76-13's fetcher. If 76-13 were one large task, the hypervisor's backends would wait on the desktop's packaging. Fix: the 76 split gives the fetcher its own first task, 76-13a, and 81-3a waits on no packaging.
* The Lenovo: each step that takes the screen or the GPU has a boot of its own; silent steps run whenever ready.

## Sitting

The steps that need the owner at the Lenovo. They leave 76's, 78's and 80's tasks and Dones; nothing in 76 to 83, no checkpoint and no release, waits on them. The manager files them after the stack's release as a milestone of their own, 84 sitting, with no branch, since they carry no code, as the line that the plan's process fix 4 adds to `AGENTS.md` allows. Each is `lenovo` and `owner-only`, and the owner names the sitting's time once the machine is ready, as the `machine` skill says.

### 84-1 Touchpad finger events and the console's keyboard across a handover

* Files: `dev-log/76-display/lenovo-touchpad.md` (after 76-10c), `dev-log/76-display/lenovo-console.md` (after 76-11b).
* Waits on: 76-17b (`evidence`: the released system with the touchpad driver).
* Labels: `lenovo`, `owner-only`.
* Subtasks:
  * 84-1.1 With the owner's finger on the pad: one-finger motion, two-finger scroll, a tap and a click, read with `evprobe` over SSH beside Debian's events for the same gestures; check: the events and their comparison in the record.
  * 84-1.2 Held over from 76-11b, which reads the handover over SSH: the owner types at the Lenovo's keyboard while `drmprobe hold 10`, started over SSH, holds the screen, and again after it exits; check: the keys typed during the hold reach no program and are not echoed when `term` takes the screen back, and the keys typed after reach `term`, recorded in `lenovo-console.md`.
* Done: each gesture gives the events Linux gives on the same device, and the console's keyboard is off while a DRM master holds the screen and on again after.

### 84-2 An HDMI monitor

* Files: `tests/graphics/ref/lenovo-hdmi/` (new: Linux's dump and CRCs), `dev-log/78-intel/lenovo-hdmi.md` (new).
* Waits on: 78-10d (`evidence`: the display proven by EDID injection).
* Labels: `lenovo`, `owner-only`.
* Subtasks:
  * 84-2.1 Debian with the monitor attached, in a Debian boot of its own: the monitor's EDID, the register dump of the DDI and PLL, and IGT's CRC of one pattern at each mode; check: the dump and CRCs in the tree.
  * 84-2.2 Rach2 on the released system, in a boot of its own, `mixer vol 1` after the boot: each mode set with the CRC read, HDMI audio's infoframes read back, and hotplug by unplugging and plugging the cable; check: the CRCs equal Linux's and hotplug reports both events.
* Done: the monitor runs at each mode with CRCs equal to Linux's, and hotplug is seen.

### 84-3 Crysis's whole first level

* Files: `dev-log/80-games/80-9.md` (after 80-9).
* Waits on: 80-9 (`evidence`: the automatic acceptance).
* Labels: `lenovo`, `owner-only`.
* Subtasks:
  * 84-3.1 The owner plays the whole first level on the released system through umu, on 80-9's route with working sound at `mixer vol 1` unless the owner sets another, in a boot of its own; check: the owner's report, recorded in `80-9.md`.
* Done: the first level played through to the owner's criterion, with the report recorded.

### 84-4 The watchdog's expiry, watched

* Files: `dev-log/78-intel/78-1b.md` (after 78-1b).
* Waits on: 78-1b (`evidence`: the driver armed, fed and stopped on the Lenovo, and Debian's expiry that left the machine down).
* Labels: `lenovo`, `owner-only`.
* Subtasks:
  * 84-4.1 Debian on p5, 78-1b's watchdog session held over to the sitting, with the owner watching the screen: `/dev/watchdog0` armed at 30 s and left unfed, first as 78-1b ran it (TCO_EN left set), then, if the machine comes back or once it is powered on, in a second boot with `iTCO_wdt.turn_SMI_watchdog_clear_off=2`, which clears TCO_EN so that only the chipset's second stage resets; SMI_EN read before each; check: what the screen showed at each expiry, the times to return or not, recorded in `78-1b.md`.
  * 84-4.2 Rach2's trial expiry, step 13 of `78-1b.md` (`wdtrial 30 120`), on the released system, with the owner watching the screen; if only the run with TCO_EN cleared came back under Debian, the Rach2 driver gains Linux's option first and the trial uses it; check: the reset within 60 s of the last feed and the release back, or what the screen showed.
* Done: the cause of the Lenovo's failure to return from a TCO expiry is known, and the watchdog either returns the machine under Rach2 or is recorded as unable to.
