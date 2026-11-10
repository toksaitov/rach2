# Chromium's WebGPU adapter on Venus

The comparison runs Chromium 156.0.8078.79 on the testing PC under QEMU 10.2.1, with KVM, four processors, 8 GiB and the `venus` display profile. Chromium is a Wayland client of a private headless Weston with Pixman. Its GL renderer is ANGLE on virgl over the host's llvmpipe; its Vulkan ICD is Mesa 26.1.6's Venus alone, `/usr/share/vulkan/icd.d/venus_icd.x86_64.json`. Vulkan names `Virtio-GPU Venus (llvmpipe (LLVM 21.1.8, 256 bits))`, a CPU device, with API 1.4.307 and driver 26.1.6.

## The reference

The Debian 13 guest uses the same QEMU profile, processors and memory. Its two references are in `tests/graphics/browser/ref/chromium-webgpu-venus/` and `chromium-webgpu-venus-native/`: `machine`, the guest and host `record`, the full switches and browser version, `result.json`, `vulkaninfo.out` and its stderr, `gpu-info.json`, Dawn's list in `gpu.txt` and the browser log. Both use the browser acceptance launcher's switches, with `--enable-unsafe-webgpu`; the second also uses `--use-vulkan=native`.

Both default `requestAdapter({})` and `requestAdapter({forceFallbackAdapter: true})` return Google's SwiftShader adapter, with architecture `swiftshader`, empty description and device strings, and `isFallbackAdapter` true. Dawn's list names Venus and marks it as a CPU adapter. Both runs count zero Venus readback passes. The requests are recorded separately; no fallback result is substituted for a default request that returns null.

## Rach2's result

The two new cases passed on Rach2 on 2026-11-10, first in 106 seconds and again in 195 seconds with the diagnostic collector retained. [The second run's output](webgpu-rach2.out) contains the complete records, switches, Vulkan device summaries, Dawn lists and browser logs. Its code is `a709e4f8a`, including the cases from `115479f27`. These were the author's runs of the two new cases alone, before the stack's review and checkpoint.

| Switch set | System | Default request | Fallback request | Dawn's Venus entry | Venus passes |
|---|---|---|---|---|---|
| Normal | Debian | SwiftShader, fallback | SwiftShader, fallback | CPU | 0 |
| Normal | Rach2 | SwiftShader, fallback | SwiftShader, fallback | CPU | 0 |
| Forced native Vulkan | Debian | SwiftShader, fallback | SwiftShader, fallback | CPU | 0 |
| Forced native Vulkan | Rach2 | SwiftShader, fallback | SwiftShader, fallback | CPU | 0 |

Both Rach2 answers equal Debian's, including the adapter fields and null readbacks. The judge also passed the comparison of browser versions, switches, Wayland platform and all ten Vulkan device and driver fields. No compute or render pass was counted on SwiftShader. Forcing native Vulkan did not expose Venus to the page on either system.

## The trace

[The graphics reference trace](../80-games/80-2.md#webgpu-on-venus) found that Dawn's Linux Vulkan backend requires exportable and importable opaque-fd semaphores before Chromium may give a page an adapter. Venus offers sync-fd semaphores and no opaque-fd semaphore features on both lavapipe and hasvk. Chromium's GL interoperability check is an earlier condition; `--use-vulkan=native` bypasses that condition. The second case tries that switch on both systems and records the adapter answer. It changes no driver or Chromium source.

The diagnostic page fills its Dawn list after its headings. The probe waits for a Vulkan backend entry before saving the list. `vulkaninfo --summary` names the device but reports `ERROR_SURFACE_LOST_KHR` while querying the private Pixman Wayland surface; its stderr is retained separately from its successful device enumeration.

## The cases

`machine.webgpu.chromium-webgpu-venus` and `machine.webgpu.chromium-webgpu-venus-native` use the existing browser scenario. They compare both adapter answers, the browser version and switches, the Wayland platform and the Vulkan device and driver fields with Debian. Missing evidence fails. The forced-native case also requires Dawn to enumerate Venus.

A non-fallback adapter whose description names Venus runs the same 1024-word compute and one-pixel render readbacks as the browser acceptance probe. Both must be correct before `venus_passes` is incremented. A null answer or an adapter of another name records no readback and zero passes. Matching Debian's refusal passes the comparison; it does not count as WebGPU working on Venus.
