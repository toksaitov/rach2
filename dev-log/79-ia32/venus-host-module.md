# Venus's host module, 79-42

The testing host had no loaded udmabuf module when task 3685 investigated the failed checkpoint row `machine.d3dvenus.probe-proton-9-i386` on 2026-11-11. Debian and Rach2 both reported Vulkan 1.2.0 on that host. The host dependency was restored; the row's proof still waits for the manager's run after review.

## Measurements

The host ran `6.18.58.1-microsoft-standard-WSL2`, Mesa `26.0.8-1ubuntu0.3`, virglrenderer `1.2.0-2ubuntu2` and QEMU `10.2.1`. Neither `/dev/udmabuf` nor `/sys/module/udmabuf` existed. The matching module file was present in `/home/testing/rach2/build/cache/wsl-udmabuf/6.18.58.1-microsoft-standard-WSL2/udmabuf.ko`. `vm.compaction_proactiveness` read 20.

Before changing the host, two diagnostic boots ran under KVM with 8 CPUs, 8 GiB, the `venus` display and Mesa 26.1.6's Venus ICD alone. Each ran `VN_DEBUG=init vulkaninfo --summary` as its ordinary account and powered off. Debian used the prepared Debian 13 reference image. Rach2 used the release copy's GNU disk and kernel `0082bcd51949`, whose SHA-256 was `1f055381b8ab6b929fce6979516593ec548a1f53220adf79ad82b8cfa02f929a`, with a temporary disk overlay. Its boot reported three capability sets and a 4 GiB host memory window.

Both reports gave these device fields:

```text
apiVersion         = 1.2.0
driverVersion      = 26.1.6
vendorID           = 0x10005
deviceID           = 0x0000
deviceType         = PHYSICAL_DEVICE_TYPE_CPU
deviceName         = Virtio-GPU Venus (llvmpipe (LLVM 21.1.8, 256 bits))
driverID           = DRIVER_ID_MESA_VENUS
driverName         = venus
driverInfo         = Mesa 26.1.6-1~bpo13+1
conformanceVersion = 1.4.0.0
```

The instance version was 1.4.309 in both reports; DXVK checks the device version. Debian's reproduction with Linux's virtio-gpu driver settled whether this failure required a Rach2 kernel change, so the older Rach2 kernel `dfddf93fc` was not rebuilt or booted. No Rach2 source was changed.

The complete Debian output and machine record are in `build/runs/task3685/debian.out` and `debian-record` of the testing host's release copy. Rach2's console, summary and the module loader's output are in `build/runs/task3685/rach2-console`, `rach2-vulkaninfo` and `load.out` of its codex copy. Both diagnostic claims had left `/var/lib/rach2-etude/ledger/claims` before the module was loaded.

## Cause

The host's Mesa 26.0.8 and the guest's Mesa 26.1.6 select this result deliberately. The cited lines were read from those tags in Mesa's repository:

* Host [`lp_screen.c`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/mesa-26.0.8/src/gallium/drivers/llvmpipe/lp_screen.c), lines 1025 to 1026, opens `/dev/udmabuf` and initializes the fence functions. [`lp_texture.c`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/mesa-26.0.8/src/gallium/drivers/llvmpipe/lp_texture.c), `llvmpipe_dmabuf_alloc`, refuses an allocation when that descriptor is negative.
* Host [`lp_fence.c`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/mesa-26.0.8/src/gallium/drivers/llvmpipe/lp_fence.c), lines 319 to 342, leaves `dummy_sync_fd` at -1 when its dummy dma-buf cannot be allocated. `lp_screen.c`, line 213, then leaves `native_fence_fd` false.
* Host [`lvp_device.c`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/mesa-26.0.8/src/gallium/frontends/lavapipe/lvp_device.c), lines 1432 to 1435, exposes `VK_KHR_external_semaphore_fd` and `VK_KHR_external_fence_fd` only with `native_fence_fd`; lines 2778 to 2793 also require it for a sync-file semaphore's import and export properties.
* Guest [`vn_physical_device.c`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/mesa-26.1.6/src/virtio/vulkan/vn_physical_device.c), lines 1131 to 1148, sets `renderer_sync_fd.semaphore_importable` only when the renderer exposes `VK_KHR_external_semaphore_fd` and answers IMPORTABLE. Lines 1274 and 1334 require that import for `VK_KHR_synchronization2` with WSI; lines 547 to 548 cap `apiVersion` at 1.2 when synchronization2 is absent.

There is no upstream fault to record: the missing host device removes the capability on Linux too.

## Repair and proof

At 18:26 UTC+6, the existing module was loaded with the release copy's `tools/setup/wsl-udmabuf/load.sh`, passing the module path explicitly. The loader reported matching version magic, 35 matching import CRCs, no mismatches and four imports not present in any installed module. The module's SHA-256 was `b1b785b468e01feb23d0e5f1a74b87ebaba9aa07d990369b779885e240419179`. It created `/dev/udmabuf` as `root:kvm`, mode 0660, and set `size_limit_mb` to 16384. The testing account and the five team accounts were in `kvm`.

The graphics instructions now require the running account to check the device's access and the loaded module's size limit before each Venus run. No code needed compilation, and no test ran after the module was loaded. The proof remains the failed checkpoint row after review.
