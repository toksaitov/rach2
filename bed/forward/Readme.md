# The forward direction

`bed/forward/` runs a virtual machine's graphics the usual way round, on Rach2: a Debian guest of QEMU on Rach2 draws on Rach2's GPU through QEMU's own virtio-gpu, first virgl and then Venus with blobs. The testbed's other rungs reverse it, Rach2 drawing through a Linux machine's GPU ([the testbed](../Readme.md)); this one is their comparison on the same machine, and it is also what a user of QEMU on Rach2 gets. [`forward.sh`](forward.sh) is the runner on Rach2, [`scene.sh`](scene.sh) the scenes that draw and check inside the guest, and [`guest.sh`](guest.sh) makes the guest's disk on the build host. The `bed-forward` family runs them on the testing PC.

## Running

On the x86-64 GNU root, as root, with the guest's disk, the key of its user and the scenes' archive:

    sh forward.sh -d /dev/disk2 -k id_ed25519 -g guest.tar virgl
    sh forward.sh -d /dev/disk2 -k id_ed25519 -g guest.tar venus
    sh forward.sh -a tcg -c 4 -m 4096 -o /tmp/out -d /dev/disk2 -k id_ed25519 -g guest.tar virgl

`forward.sh` boots the guest in the root's `qemu-system-x86_64` with `virtio-vga-gl` on `-display egl-headless,rendernode=/dev/dri/renderD128`, waits for the guest's SSH server on port 2222 of the loopback address (`-p`), copies the archive in, runs `scene.sh` as the guest's user `gfx`, copies the scene's output out and powers the guest off. `-a` is the accelerator, `kvm` by default and `tcg` for QEMU's emulator; `-c` and `-m` are the guest's processors and memory, 4 and 4096 MiB; `-o` is the directory that gets the guest's console, QEMU's messages, the scene's output and its logs, the working directory by default. `-f` is the disk's format, `raw` for a device and by default, or `qcow2` for the guest's image file itself. `-r` names, for `venus`, the frames file of `vkcube.sh` whose checksums vkcube's frames must have, in place of the archive's `vkcube.ref`, or is `none`, which records the frames and judges none. `RENDER_NODE` names another render node, `QEMU` another QEMU, and for `venus` `VK_DRIVER_FILES` the Vulkan driver the guest's calls are made on, by default Venus's, and `HOSTMEM` the size of the window blobs are mapped in, 4G.

On a machine with a GPU of its own, such as the Lenovo, the runner runs under KVM, the default, and the guest's disk is the image file; `venus` names the GPU's own Vulkan driver, hasvk on the Lenovo, and the frames that Debian in Rach2's place drew on that GPU, since a frame's checksum depends on the GPU that draws it; the Lenovo's are [tests/graphics/ref/lenovo/forward/vkcube.frames](../../tests/graphics/ref/lenovo/forward/vkcube.frames), which the second line passes as a copy in the working directory:

    sh forward.sh -f qcow2 -d guest.qcow2 -k id_ed25519 -g guest.tar virgl
    VK_DRIVER_FILES=/usr/share/vulkan/icd.d/intel_hasvk_icd.json \
        sh forward.sh -f qcow2 -r vkcube.frames -d guest.qcow2 -k id_ed25519 -g guest.tar venus

The same lines run on Debian in Rach2's place, with `-r none` for `venus` when they take the reference.

The runner bounds silence, never the run: it fails when QEMU ends early, or when neither the guest's console nor the scene's output has grown for 600 seconds (`-w`).

The guest's disk and the archive are made on the build host:

    sh bed/forward/guest.sh build/tests/cases/bed-forward/guest.qcow2 build/tests/cases/bed-forward/id_ed25519
    make ARCH=x86_64 build/tests/cases/bed-forward/forward.ext2

`guest.sh` prepares the graphics tests' [Debian reference guest](../../tests/graphics/Readme.md#the-reference-guest) where the clone has none yet, and writes its prepared image whole, with no backing file, as one qcow2 image, with the private key of `gfx`. The make rule packs `guest.tar` (`make ARCH=x86_64 build/tests/cases/bed-forward/guest.tar` alone), which holds `scene.sh`, [`triangle.c`](../fixture/triangle.c), the graphics runs' `guest-env.sh`, their vkcube runner with its helpers, and vkcube's reference checksums, and puts it with `forward.sh` and the key on a small ext2 disk, which the family's cases mount on Rach2.

## Results

`forward.sh` prints the scene's facts, each a line `bed-forward: NAME: VALUE`, and last one result line, and exits with 0 on a pass:

    bed-forward: start: qemu-system-x86_64 -accel tcg -cpu max -smp 4 -m 4096 -device virtio-vga-gl,edid=on,xres=1280,yres=800 -display egl-headless,rendernode=/dev/dri/renderD128
    bed-forward: guest: SSH answered after 68 s
    bed-forward: triangle: llvmpipe f0b3cd1d6d2e2625, virgl (virgl (D3D12 (NVIDIA GeForce RTX 4080 SUPER))) f0b3cd1d6d2e2625
    bed-forward: glmark2: virgl (virgl (D3D12 (NVIDIA GeForce RTX 4080 SUPER)))
    bed-forward: glmark2: [build:use-vbo=true] at 1280x800: validation Success, 53 frames a second
    bed-forward: result: pass: virgl: triangle f0b3cd1d6d2e2625 on virgl (virgl (D3D12 (NVIDIA GeForce RTX 4080 SUPER))); glmark2 [build:use-vbo=true] validated at 53 frames a second

A failure's last line is `bed-forward: result: fail: REASON`, the scene's or the runner's own.

`scene.sh virgl` passes when the guest's GL renderer is virgl, the triangle drawn through it has the checksum of the same triangle on the guest's llvmpipe, and glmark2's scene validates. The triangle is the vtest rung's, built in the guest with its GCC: it reads its picture back and takes the FNV-1a checksum of the pixels, `f0b3cd1d6d2e2625` on every driver that draws it right. glmark2-es2-drm then draws `[build:use-vbo=true]` off screen at 1280x800 twice: once with `--validate`, which compares a frame with glmark2's own reference picture, and once as the benchmark, whose frames a second the scene reports.

`scene.sh venus` passes when the guest's Vulkan device is Venus and vkcube's pictures are the reference's, or, with `-r none`, when vkcube's frames were read, and then says `frames not judged`. [`vkcube.sh`](../../tests/graphics/runners/vkcube.sh) of the graphics tests presents vkcube through Venus to a Weston of its own on the guest's card, holds it after its 300th and 345th frames and reads each back inside the guest; both must have the CRC-32 that [`frames.out`](../../tests/graphics/ref/venus-wsl/vkcube/frames.out) records for the reference guest on the testing PC's lavapipe, `0x75d4ad32` and `0x1a0d31d7`. The scene reports the rate of the first 300 presents.

The measurements, on the testing PC and beside Linux in Rach2's place, are in [83-2.md](../../dev-log/83-testbed/83-2.md).

## Design

### The machine

The guest is q35 with a virtio disk, a virtio network card on QEMU's user network, which forwards the SSH port, a virtio random number generator and `virtio-vga-gl`, virtio-gpu with a VGA, since Debian's GRUB under SeaBIOS resets on a display that has none. The display is egl-headless: QEMU makes its GL context on Rach2's render node and shows nothing, and virglrenderer draws the guest's GL there.

For `venus` the device has `venus=on,blob=on,hostmem=4G` and the guest's memory is a shared memfd. virglrenderer runs each guest context's Vulkan calls in a process of its own, a worker of `/usr/libexec/virgl_render_server`, which the GNU root has from Debian's `virgl-server` ([the KVM layer](../../vendor/gnu/kvm/Readme.md)); a blob, the memory of a Venus buffer or image, is a dma-buf or a memfd that the worker passes to QEMU, which maps it and shows the mapping to the guest inside the window. `VK_DRIVER_FILES` leaves the worker one Vulkan driver, by default Venus's, so that the guest's one device is Rach2's GPU and never Rach2's lavapipe.

The disk is opened with `snapshot=on`. Rach2's ordinary kernel refuses every write to a disk's node ([disk(4)](../../man/man4/disk.4)), and Debian does not boot from a disk it cannot write: its root stays read-only and its units fail. QEMU keeps the guest's writes in a temporary overlay, an unnamed file in the directory `TMPDIR` names, by default `/var/tmp`, which the GNU root does not have; the runner sets `TMPDIR` to its output directory. The disk is the same after every run.

### The accelerator

`kvm` is `-accel kvm,kernel-irqchip=split -cpu host`, since Rach2's KVM has the split interrupt controller alone ([the KVM tests](../../tests/kvm/Readme.md)). It is the default and what a Rach2 on hardware uses.

The family's cases pass `-a tcg`. Their Rach2 is itself a guest of the testing PC's KVM, which is a guest of Hyper-V, so a guest of Rach2's KVM is at the third level, where every exit is emulated twice over. Measured there, Debian's kernel printed its first line more than two minutes after QEMU started, systemd's wait for a partition's device ran out, and Debian stopped in emergency mode after 12 to 15 minutes without its SSH server ([83-2.md](../../dev-log/83-testbed/83-2.md)). Under QEMU's emulator the same guest answers SSH in 38 to 68 s. The [testing skill](../../.agents/skills/testing/SKILL.md) has the same rule for every third-level guest of the testing PC.

### The scenes

Each scene checks a picture inside the guest, by reading it back there, so that the check holds without any display on Rach2's side: QEMU's is headless. The triangle proves the path exact, pixel for pixel against the guest's own software renderer; glmark2's validation and vkcube's two frames prove a real program's scene against a reference made on Linux. glmark2-es2-drm and vkcube's Weston run as root, since each takes the guest's card as its master.

## Limits

* On the testing PC the guest's processor is QEMU's emulator, so the rates there measure the emulator beside the graphics path. The rates under Rach2's KVM are the Lenovo's, task 83-2b.
* On the testing PC Rach2's own Vulkan is Venus on the PC's lavapipe, since WSL has no hardware Vulkan driver, so the `venus` case is Venus through Venus, drawn in the end by the PC's processors.
* The worker of each Vulkan context ends by a fault in Mesa's Venus driver once its work is done, and Rach2's log has a `vm: fault: killing pid` line for it; QEMU answers the unmap of each blob that vkcube shared with Weston with an error, eight in the scene. Both happen on Linux in the same way and neither changes a picture ([upstream.md](../../dev-log/83-testbed/upstream.md)).
* The runner takes one port of the loopback address, so two runs at once need different `-p`.
* Nothing the guest writes outlasts its run.

## Tests

`ARCH=x86_64 tests/run bed-forward` runs the family [`machine.bed-forward`](../../tests/cases/machine.bed-forward.json) on a Linux host: `virgl` on the display profile of that name, which gives Rach2 the testing PC's GPU through virgl, and `venus` on the profile that gives it Venus. Each case boots the GNU root with the data disk and an overlay of the guest's disk, copies the runner's files to `/tmp/bed`, runs `forward.sh -a tcg` there and keeps its output directory as evidence ([`bedforward_scenario.c`](../../tests/cases/machine/bedforward_scenario.c)). `RACH2_CASES=machine.bed-forward.virgl` runs one case, and milestone 83's checkpoint slice names `bed-forward:virgl`.
