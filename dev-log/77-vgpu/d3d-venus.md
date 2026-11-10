# Direct3D on Venus: what the programs refuse and what the host had to change

Task 77-6 runs the 76-21 programs, Wine's Direct3D units and vkd3d-proton's suite through GE-Proton11-12 on Venus over the testing PC's lavapipe, with Mesa 26.1.6's Venus driver in the guest, in the Debian reference guest and on Rach2 alike (`tests/run d3dvenus`, [the graphics tests' Readme](../../tests/graphics/Readme.md#direct3d-on-venus)). This file records what a program refuses on Venus, by the Vulkan feature or extension behind it, and the faults the first Linux runs met that were not Venus's or Rach2's.

## Refusals

No program refuses to start on Venus with Mesa 26.1.6. In the Debian guest on Venus each of the six probes, Direct3D 9, 11 and 12 through DXVK and vkd3d-proton and through WineD3D on Vulkan and Wine's vkd3d, made its device, drew and read both pixels back right, and every one of Wine's units and vkd3d-proton's 578 tests started. So no refusal is a blocker.

| Program | What it refuses or limits | The Vulkan feature behind it, from the Venus device's `vulkaninfo` (`tests/graphics/ref/venus-wsl/vulkaninfo/venus.out`) |
|---|---|---|
| DXVK 3.1.1 (Direct3D 9, 10 and 11) | nothing with Mesa 26.1.6; with 25.0.7's Venus driver it found no adapter | `VK_KHR_load_store_op_none`, which 25.0.7's Venus lacks and 26.1.6's has (the [Mesa qualification](mesa-qualification.md)) |
| vkd3d-proton 3.1 (Direct3D 12) | feature level 12_0 and above; the device's highest is 11_1 | tiled resources tier 2, which 12_0 requires with binding tier 2 and the additional typed UAV loads: Venus reports `sparseBinding`, `sparseResidencyBuffer`, `sparseResidencyImage2D`, `sparseResidencyAliased`, `shaderResourceResidency`, `shaderResourceMinLod` and `filterMinmaxSingleComponentFormats`, but `residencyNonResidentStrict` is false, so `d3d12_device_determine_tiled_resources_tier` (`libs/vkd3d/device.c` at 8293130eb) stops at tier 1 |
| vkd3d-proton 3.1 | shader model 6.1 and above; the highest is 6.0 | SM 6.2 needs FP32 denormal control, `shaderDenormPreserveFloat32` and `shaderDenormFlushToZeroFloat32`, both false with `denormBehaviorIndependence` ALL; vkd3d-proton names nothing between 6.0 and 6.2 |
| Wine's vkd3d (Direct3D 12) | feature level 12_0 and above | the same device; it reports shader model 6.2, its own rule, and tiled resources tier 0 |
| WineD3D on Vulkan (Direct3D 9, 10 and 11) | device creation under Xvfb, without `MESA_VK_WSI_DEBUG=sw` | not a Vulkan feature: Mesa's X11 WSI offers a GPU device presentation only over DRI3, which Xvfb lacks, so `vkGetPhysicalDeviceSurfaceSupportKHR` says no ("Queue family does not support presentation on this surface"), and WineD3D, which makes a swapchain with its device, failed `D3D11CreateDevice` with `E_OUTOFMEMORY` and Direct3D 9's `CreateDevice` with `E_FAIL`. `run.py` sets `sw` for every Venus row, which presents through the CPU |

vkd3d-proton's suite skips tests for the features above and for others no D3D12 program needs to start: in the Debian guest on Venus, 15 tests for AMD's FP8 WMMA, 10 for work graphs, 8 for device-generated commands, mesh shaders, variable-rate shading, shader models 6.1 to 6.9, conservative rasterization, FP64 and native 16-bit arithmetic, each by the test's own "Test skipped" line. The reference stores each test's skip count, and a Rach2 run may skip no more.

## The host's udmabuf limit

Every first Linux run on Venus met the same fault: the host refused a blob (`ctrl 0x10c, error 0x1200`, then `ctrl 0x208, error 0x1203`), the render server logged "failed to look up object N of type 8" (a `VkDeviceMemory`), the context's ring went fatal, and the guest's process aborted or waited for ever; vkd3d-proton's `test_unused_attachments_mix_and_match` waited 11 minutes with no CPU time before it was stopped. The cause is the host's udmabuf module, which makes no buffer over its `size_limit_mb`, 64 MiB by default: lavapipe exports through udmabuf the memory of each allocation that Venus maps.

* On the host, `UDMABUF_CREATE` on a sealed memfd made buffers of 63 and 64 MiB and refused 65 and 256 MiB with EINVAL.
* In the Debian guest on Venus, a Python program allocating host-visible, coherent memory through `libvulkan` allocated, mapped and wrote 16, 63 and 64 MiB; at 65 MiB `vkMapMemory` returned `VK_ERROR_MEMORY_MAP_FAILED`, the host logged the blob error and "failed to look up object 6 of type 8", the ring went fatal and the process aborted.

The same lines are those of the dEQP-VK `cube_array` crash that the Mesa qualification left untraced. A host whose Venus runs on a GPU exports no memory through udmabuf, so the limit is the testing PC's alone. `size_limit_mb` was set to 16384 from the owner's account at 12:00 UTC+6 on 2026-11-10, without reloading the module, and `tools/setup/wsl-udmabuf/load.sh` now sets it at each load. Every Linux run of this task was then taken again; none of them logged the fault.

## Stalls in the Debian guest

With the limit raised, two units stall on Venus in the Debian guest, their processes alive with no CPU time and no output for minutes, and the reference stops each where it stalled, as the lavapipe family stops WineD3D's DXGI unit:

* Wine's `d3d9:device` through DXVK, after `device.c:9457: Test failed: Expected non-zero pitch, got 0.`, at the same line in both runs.
* vkd3d-proton's `test_multithread_command_queue_exec`, after its 26th `test_multithread_command_queue_exec:4176: Test failed: Got 0x00000000, expected 0xff00ff00 at (0, 0, 0).`

WineD3D's DXGI unit on Vulkan stalls where it does on GL, at `dxgi.c:8040`.

## The test account's groups on Rach2

The first Rach2 run of the probes failed in each Vulkan process: DXVK's Direct3D 11 probe found no DXGI adapter, and Wine logged "Failed to enumerate physical devices, res -3". Booted by hand in the same QEMU, the first `vulkaninfo` as the test account found Venus and the later ones failed, Venus logging "failed to open /dev/dri/renderD128". The GNU root's desktop service starts systemd-udevd and `udevadm trigger` some seconds after boot, and Debian's `50-udev-default.rules` makes the render node `root:render` 0660; until then the kernel's mode, 0666, lets anyone open it. The test account was in no group but its own, while the Debian reference guest's user is in `video` and `render`. The GNU root's `/etc/group` now puts `test` in both, which the `venus` suite, also run as `test`, needs as well.
