# The VM's GPU backend

`bed/vm/` is the Linux side of the test bed's shared-memory GPU transport ([`../shm/`](../shm/Readme.md)): a Debian 13 virtual machine whose backend serves Rach2's virtio-gpu from the region that QEMU's ivshmem device shares with the host. [`module/`](module/) is `vgpushm.ko`, which binds the ivshmem device in the VM and hands its region and doorbell vectors to the backend. [`backend/`](backend/) is `vgpu-backend`, which runs the transport's device side, renders virgl and Venus through libvirglrenderer and shows the scanout on the VM's KMS as its DRM master. [`image/`](image/) prepares the VM from Debian's cloud image and checks the whole path against [`peer.c`](../shm/peer.c), which plays Rach2's driver on the host.

## Running

    sh bed/vm/image/vm.sh prepare [-n]
    sh bed/vm/image/vm.sh check [-V] [-c CPUS] [-m MIB]
    sh bed/vm/image/vm.sh rach2 [-o RAW]

`prepare` boots an overlay of the cloud image that [`tests/graphics/debian-ref/image`](../../tests/graphics/debian-ref/image) pins once with a cloud-init seed. [`image/setup.sh.in`](image/setup.sh.in) installs [`image/packages`](image/packages) from Debian's snapshot of the date that file names, rebuilds Debian's virglrenderer with GBM allocation, builds vkms with [`image/vkms.patch`](image/vkms.patch), adds `intel_iommu=on iommu=pt` to the kernel's command line and vfio's leave to take unremapped interrupts, and adds the service that writes the versions and `bed-vm: ready` to the serial console at each boot. The result is `prepared.qcow2`, read-only, in `build/cache/bed-vm` of the main clone, which its worktrees share, or in `BED_VM_CACHE`, with the key of the seed's files beside it; `-n` keeps an image of the same key, and `check` refuses one of another.

`check` builds the peer into `build/bed/vm` and starts it as QEMU's ivshmem server with a region of 32 MiB, boots a new overlay with 4 CPUs and 4096 MiB (`-c`, `-m`) and the region on an `ivshmem-doorbell` of two vectors, copies the tree's `module/`, `backend/` and `rach2/sys/virtio/vgpushm.h` into the VM and builds them there, and runs the backend as the device while the peer drives it. With `-V` the backend takes the device through vfio-pci under QEMU's emulated intel-iommu in place of the module. The accelerator is KVM where `/dev/kvm` opens, as [`rach2/qemu_devices.sh`](../../rach2/qemu_devices.sh) chooses.

`rach2` makes the VM that Rach2 drives over its own transport under its own KVM: `rach2.qcow2` beside the prepared image, an overlay of it with the tree's module and backend built and installed and a service, `bed-vm-backend`, that at each boot loads the module and runs `vgpu-backend -v` surfaceless on the ivshmem device, writing each of its lines to the console after `bed-vm: `. With `-o` it also writes the image whole as the raw disk `RAW`, which `tests/run bed-shm` boots ([kvm](../../tests/kvm/Readme.md)).

In the VM the backend runs as root, so that it can be the KMS device's master:

    LIBGL_ALWAYS_SOFTWARE=1 vgpu-backend [-m NODE | -V BDF | -f FILE] [-k CARD] [-g DEV]
                                         [-d] [-w] [-x | -I] [-p PEER] [-1] [-v]

* `-m NODE` takes the region and vectors from the module's node, `/dev/vgpushm0` by default; `-V BDF` from the ivshmem device at PCI address `BDF` bound to vfio-pci; `-f FILE` maps a file shared another way and polls.
* `-k CARD` shows the scanout on the KMS device `CARD`; `-g DEV` names the DRM device virglrenderer allocates on through GBM, `CARD` by default. Without either, virglrenderer runs surfaceless and each flush prints its CRC.
* `-d` scans a resource's own buffer out when it can; `-w` checks each frame through the device's writeback connector.
* `-x` starts virglrenderer without Venus; `-I` imports each resource a Venus context takes into Vulkan and reports its CRC.
* `-p PEER` sets the peer id the backend offers; `-1` exits when the driver detaches; `-v` reports each epoch.

`LIBGL_ALWAYS_SOFTWARE=1` keeps Mesa's GBM and EGL on llvmpipe over vkms, which has no render node; without it Mesa looks for a hardware driver for the card, prints `kmsro: driver missing`, and EGL finds no device.

## Results

`check` prints the backend's lines and the peer's, each prefixed `bed-vm:`, then one line for each check and a last line, `bed-vm: result: the VM drew and showed the peer's frames`, or the reason it failed. It writes the VM's record to standard error as `bed-vm: record:` lines: the versions of `libvirglrenderer1`, `libgl1-mesa-dri`, `mesa-vulkan-drivers`, vkms and the kernel, the accelerator, QEMU's version, the image with its SHA-512, the snapshot and the device's path, `vgpushm` or `vfio-pci`. Its checks:

* frames written back: the CRC of each frame vkms's writeback copied, in order, equals the CRC the peer expects: its 2D image, the display frame whole, and its top-left quarter;
* Venus's import: the CRC of the frame's buffer as Vulkan imported it equals the CRC of its rows as the peer read them back;
* direct scanouts: the whole frame was scanned out from its own buffer, once, and the quarter copied;
* the backend was woken by the queue's doorbell and the peer by the VM's, and the peer ended well.

The backend's own lines: `backend: scanout: writeback WxH of mode MxN, crc X` for each frame checked; `backend: kms: mode MxN for a scanout of WxH` for each mode set; `backend: kms: resource ID is copied: REASON` the first time a resource cannot be scanned out directly; `backend: venus: imported resource ID, WxH, crc X`; and at the end `backend: result:` lines with the device's counts (epochs, commands, errors, fenced commands, completions, doorbells rung, faults, wakes by each vector and steps that found work no doorbell announced), the module's (interrupts and eventfd signals by vector, doorbell writes) and the display's (flushes, copies, direct scanouts, modesets, page flips, skipped flushes, writebacks).

Measured on the testing PC under KVM, QEMU 10.2.1, the VM with virglrenderer 1.1.0-2+rach21 and Mesa 25.0.7: `prepare` took 2 minutes, and `check` passes both ways. The frames' CRCs are a9e0acd7 for the 32 by 32 2D image, f10af47a for the 1024 by 768 frame as shown, faccd412 for its quarter, and 6e8e217c for the frame's rows as stored, which the import reads. Through the module the device served 26 commands, 5 of them fenced, and was woken 26 times by the queue's vector and twice by the control vector, with 26 and 3 interrupts in the module, and polled none; the peer rang 29 doorbells and was woken 28 times. Through vfio the device's and the peer's counts were the same but for 3 wakes by the control vector. The runs' output is in [`dev-log/83-testbed/83-4/`](../../dev-log/83-testbed/83-4/).

## Interfaces

The module binds every ivshmem device (1af4:1110) and makes a misc node, `/dev/vgpushmN`, that one process may hold open at a time. Its ioctls, in [`module/vgpushm_uapi.h`](module/vgpushm_uapi.h):

| Request | Argument | Does |
|---|---|---|
| `VGPUSHM_IOC_INFO` | `struct vgpushm_info` out | the region's size, the VM's peer id from IVPosition, and the MSI-X vectors enabled: 2 for an `ivshmem-doorbell`, 0 for an `ivshmem-plain`, whose peer id is ~0 |
| `VGPUSHM_IOC_EVENTFD` | `struct vgpushm_eventfd` in | binds an eventfd to a vector, or unbinds it with fd -1; the vector's interrupt signals it |
| `VGPUSHM_IOC_RING` | `__u32` in | writes the value, `VGS_DOORBELL(peer, vector)`, to the doorbell register |
| `VGPUSHM_IOC_COUNTS` | `struct vgpushm_counts` out | interrupts and signals by vector and doorbell writes since the device was bound |

`mmap` at offset 0 maps the region, BAR 2, shared. A removed device fails each call with `ENODEV` while its node is held.

## Sources and license

The module is GPL-2.0-only, as a Linux module must be, and its UAPI header GPL-2.0-only with the Linux syscall note; the backend is GPL-2.0-or-later, as QEMU's virtio-gpu code is, whose use of libvirglrenderer its renderer follows. Each file's SPDX line says which.

## Local changes

The VM changes two of Debian's packages, in `image/setup.sh.in`:

* virglrenderer 1.1.0-2 is rebuilt from Debian's own source with `-Dminigbm_allocation=true` as version `1.1.0-2+rach21`, held at that version. Debian builds it without GBM allocation, and only a resource allocated through GBM has a dma-buf: without one a GL resource can be neither scanned out directly nor attached to a Venus context, which takes resources only as file descriptors.
* vkms, which Debian's kernel does not build, is built from `linux-source-6.12` of the running kernel's version with `image/vkms.patch`, two fixes Linux's master has and 6.12.115 lacks: the encoders' possible clones, without which a commit with the writeback connector is refused, and the writeback's rows, which a reflected plane wrote in the plane's order. [dev-log/83-testbed/upstream.md](../../dev-log/83-testbed/upstream.md) has both faults.

## Design

### The module

The module enables the PCI device, maps BAR 0's registers and BAR 2's region, reads the VM's peer id from IVPosition and asks for MSI-X vectors; a device without them is served without doorbells. Each vector's handler signals the eventfd bound to it under a spinlock and counts. The device's lifetime is a reference count that the node's open holds, and a mutex with a `gone` flag keeps the ioctls and `mmap` from a device being removed. `mmap` maps the region with `vm_iomap_memory`.

### The backend

[`backend/device.c`](backend/device.c) is the transport's device side, in C99 with only libc, so that the fuzzer links it alone: the handshake of each epoch, as the transport's Readme gives it, the two queues, and virtio-gpu's commands, each body's size checked, each id found in the epoch's table, each memory entry checked with `vgs_check_range` and charged to the device's backing limit before the renderer sees it. A fenced command's completion waits for its fence; an error or a response with data completes at once. The cursor queue's chains complete empty. A transport fault stores `NEEDS_RESET`.

[`backend/virgl.c`](backend/virgl.c) drives libvirglrenderer as QEMU's `virtio-gpu-virgl.c` does: EGL on GBM over the KMS device, or surfaceless without one, with Venus through the render server unless `-x`. The capsets offered are those virglrenderer gives a size, virgl's two and, unless `-x`, Venus's. A 2D resource is created as QEMU creates one, a render target with its first row at the top. Fences retire through virglrenderer's callbacks, on the global timeline or a context's ring. A read of a resource for the display gives the rows as a display shows them: a resource whose first row is the bottom one, as GL draws, is read from the mirrored rectangle and its rows reversed, as QEMU shows it.

[`backend/kms.c`](backend/kms.c) is the display, by atomic commits on the first connected connector, its crtc and its primary plane. A scanout's mode is one of its size, the preferred mode first, else the smallest that holds it, else the preferred one; the connector's preferred mode and EDID are what the device reports. A flush is shown one of two ways: directly, when `-d` is given and the resource's dma-buf is exactly the scanout's rectangle and the mode, imported as a framebuffer and shown through the plane with `reflect-y` when its rows are bottom first; otherwise copied into the dumb buffer not shown, which then is. The display's open of the card is DRM's master, and GBM has an open of its own, since GEM handles belong to an open file and the display closes those it imports. With `-w` each commit that shows a frame also attaches the writeback connector and waits for its fence, and the CRC of the frame's top-left corner, the scanout's size, is printed.

[`backend/vkprobe.c`](backend/vkprobe.c) is `-I`'s check of Venus's path: when a resource is attached to a Venus context, which virglrenderer passes to Venus's render server as a dma-buf, the backend exports the same dma-buf, imports it into Vulkan's device memory on the first device that imports dma-bufs, lavapipe in the VM, maps it and prints the CRC of its rows.

[`backend/main.c`](backend/main.c) takes the region and its doorbells. Through the module it binds an eventfd to each vector and rings with `VGPUSHM_IOC_RING`. Through vfio it opens the device's IOMMU group in a type-1 container, maps BAR 2, turns bus mastering on, reads IVPosition and rings by writing BAR 0's registers through the device, and binds the two MSI-X vectors to eventfds with `VFIO_DEVICE_SET_IRQS`. The loop steps the device until it is idle, then sleeps on the eventfds and virglrenderer's poll descriptor, a second at most with doorbells accepted and a millisecond while a fence is pending or without them.

### The check

The peer's frame comes from `peer -W`, a stream that clears a 1024 by 768 scanout render target and draws a box off its centre; the peer reads it back and computes the CRC of its rows as stored and as a display shows them. vkms's writeback copies the composed frame, so its CRC tests the whole display path, the mode, the copy or the direct scanout and the reflection, against what the peer drew.

## Limits

* The vfio way needs QEMU's intel-iommu with `intremap=off` and the VM's vfio to take unremapped interrupts: with remapping, QEMU 10.2.1's ivshmem routes its vectors before the VM programs them, which remapping refuses, and no doorbell reaches the VM ([upstream.md](../../dev-log/83-testbed/upstream.md)).
* The backend shows one scanout, and the cursor queue's commands are taken but not drawn.
* Blob resources are created and attached, but `RESOURCE_MAP_BLOB` is refused: the region is the only memory the two sides share, and a host-visible blob would need a second window.
* The VM renders on llvmpipe and lavapipe, which a GPU passed to the VM would replace.

## Tests

`sh bed/vm/image/vm.sh check` and `check -V` are the backend's checks against the peer; they run no suite. The device's parser is fuzzed by [`tests/fuzz/vgpudev_fuzz.c`](../../tests/fuzz/vgpudev_fuzz.c), which the host suite replays over its corpus.
