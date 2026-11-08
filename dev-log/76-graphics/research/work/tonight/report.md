# The night of 2026-11-04: what was measured, and the plan's review

Written by the chat for you, from 04:10 to 07:00 (UTC+6), on your order: check nested virtualization, add the group, then research and experiments on the Lenovo and in QEMU on WSL with sample programs, and a review of the plan by Codex and Fable at the end.

## The answer first: Windows and nested virtualization

* Yes. The Windows updates KB5132402 and KB5127936, installed on 2026-11-03, give WSL nested virtualization: AMD's SVM on all 32 threads, the kvm_amd module loaded with nesting on, and `/dev/kvm` present. Your `.wslconfig` already asked for it; nothing was changed on Windows.
* QEMU in WSL now runs guests under KVM as the ordinary user `testing`. Rach2 boots that way to its login prompt in 2.4 s, and a Debian 13 guest ran all night that way.
* The catch, measured: a VM inside that VM (a third level) is slow wherever it hands control back to the hypervisor, about 0.8 ms each time against 6 microseconds one level up. A small Linux boot took 60 to 71 s there under KVM and 2.9 s under QEMU's emulator (TCG). Rach2 took 3 minutes under KVM and 6 s under TCG; 138 s of the 3 minutes went to Rach2 searching every PCI bus once per driver. So Rach2's hypervisor work keeps both ways on the testing PC, and the plan now has Rach2 scan PCI once at boot.
* All seven test accounts (testing, ghrunner and the five research accounts) are in the `render` group, as you said. They are also in the `kvm` group, which tonight did not change.
* A side effect you should know: a test run that does not name its emulator now takes KVM on the testing PC, where before the update it could only take TCG. The release's runs name TCG and are not affected. The CI's full run does not name it and would switch to KVM when the GitHub runners come back. Nothing runs tests now. Codex and Fable advise one check before any run is allowed KVM (measurement 3 below).

## The Lenovo under Linux

Linux could not be Debian tonight without a new boot path. The Lenovo ran Alpine Linux 3.24 instead, the way milestone 70 ran it: one boot entry used once, SSH at boot, the screen dimmed, the sound driver not loaded. It ran from 04:37 to 05:15 and came back in Rach2 by itself at 05:17. Its boot partition was saved first and written back afterwards, and it reads back identical to the saved copy.

What works on the HD 4400 under Linux:

* OpenGL 4.6 and OpenGL ES 3.2 (Mesa's Crocus driver); Vulkan 1.2 (hasvk; 1.3 with one driver option).
* WebGL 1 and 2 in Chromium 156 and Firefox 151. WebGPU in Chromium when started with two switches (`--enable-unsafe-webgpu` and `--ignore-gpu-blocklist`), with compute and drawing read back correct.
* Mode setting, legacy and atomic; Vulkan straight to the display.
* Video decoding on the GPU in Firefox and ffmpeg (H.264); Firefox dropped 23 of 340 frames in its first 12 s. Chromium's GPU decoding fails on Haswell and falls back to the CPU, which played 1080p without dropped frames.
* Direct3D 11 through Wine's own WineD3D, on OpenGL and on Vulkan.
* Venus, the Vulkan transport for VMs, runs on the HD 4400: a test cube at the full 60 frames per second through it.
* VT-x is on in the firmware, so your sitting needs no step for it. KVM boots a Linux guest in 1.1 s, against 8.8 s without it.
* The chipset's watchdog is there and unused, so the Intel driver work can use it as a safety net.

What does not work: DXVK 2.7.1, the version tried, cannot use this GPU. It needs 64-bit integers in shaders, which Haswell's Vulkan driver lacks. Crysis stays on WineD3D, as you decided.

## QEMU on the testing PC

* OpenGL in a Linux guest runs on the RTX 4080 (OpenGL 4.2 and OpenGL ES 3.1 in the guest), 2.3 to 2.6 times faster on screen than on the CPU. WebGL works there in Chromium 156, Chrome 157 and Firefox ESR 153 without switches. A draw call costs twice what it does on the Lenovo, a cost of the path through Windows' Direct3D 12.
* Vulkan in the guest runs only on the CPU, at version 1.2, with no way to show a picture. The cause is one feature missing from WSL's Linux kernel (udmabuf). So DXVK and vkd3d-proton cannot run there directly.
* Tested tonight, decision A's first option: a Debian VM on the testing PC, whose kernel has udmabuf, as the host for Vulkan. A guest inside it gets Vulkan 1.3 with a swapchain, the test cube ran 300 frames, and DXVK 2.6 ran a small Direct3D 11 program on it with a correct result. What it lacks is a picture on QEMU's display, so tests there read their results back instead. vkd3d-proton (Direct3D 12) was not tried.
* WebGPU in QEMU takes Chromium's own software renderer (SwiftShader) on either host, because Vulkan there runs on the CPU. WebGPU through Venus needs a host whose Vulkan runs on a GPU, such as the Lenovo under Linux.
* Display facts: a QEMU display device without VGA makes Debian's boot loader reset forever; QEMU prefers 1280x800 but offers screens up to 5120x2160, which glmark2 takes unless told otherwise; QEMU's screen capture does not work with its GL display.

## The plan

* Codex and Fable each reviewed the plan against these results, then argued their differences in a second round. They now agree on nearly everything: WebGPU in QEMU counts only on Venus, with the software renderer as a control; the unsafe WebGPU switches stay in the test launcher; the KVM side effect is a check on the testing PC, not a plan decision. Where they still differed, the plan does both in order (`tonight/argument.md`).
* Draft 5 of the plan (`graphics-research/plan.md`) folds it all in; draft 4 is kept as `plan-v4.md`. Its section "What the night changed" lists every change. It is not approved, and nothing is broken into tasks.
* None of your earlier decisions has to change. Three new ones are yours (A, B and C in the plan), each with a recommendation:
  * A. Which host carries Vulkan for VMs in QEMU: a Debian VM on the testing PC (nothing changes on Windows; tested tonight and working, with results read back; recommended), or a custom WSL kernel named in your `.wslconfig`, or leaving DXVK and vkd3d-proton in QEMU for later.
  * B. Chromium's GPU video decoding on the Lenovo: check again on Debian, and if it still fails, accept Firefox on the GPU and Chromium on the CPU (recommended), or require GPU decoding in Chromium too.
  * C. WebGPU for users on the Lenovo: test-only, so the desktop starts Chromium without the unsafe switches and a user may turn them on (recommended by the chat, Codex and Fable), or the desktop passes them for everyone.

## What to measure next

Proposals from Codex and Fable, merged; none runs before you say so.

1. In the Debian VM: whether Chromium can be made to take Venus for WebGPU instead of its software renderer.
2. In the same VM: Direct3D 12 through vkd3d-proton on Venus, the pinned Proton's DXVK, and a QEMU display that might show Venus's frames.
3. On the testing PC: whether Rach2's tests give the same results under KVM as under TCG, before any test run may use KVM.
4. On the Lenovo at its next Linux boot: Venus through QEMU with a Linux guest, Chromium's WebGPU in that guest, each WebGPU switch alone, a WebGPU picture on screen, the watchdog armed once, and the IOMMU turned on.
5. On the Mac: whether Rach2's own boot loader starts with a display device that has no VGA.
6. In a QEMU guest: why Chromium's GPU process ran without its sandbox.
7. On the Lenovo, the Debian reference: Crysis on WineD3D's two renderers, both browsers' decoders on a longer clip, and the GPU test suites.
8. On the Lenovo, the first trial of the Intel driver work: the watchdog's expiry, then a GPU hang with other work running.
9. On the testing PC: the smallest hypervisor tests at the third level, with exits counted, to see whether 78's tests can run there.
10. On the testing PC: Rach2's boot with exits counted, before and after one PCI scan at boot.

## What was changed, and what was downloaded

* The testing PC: the `render` group for seven accounts; the two GitHub runner services stopped and kept off at boot, on your GitHub stop (`systemctl enable --now` turns them back on). The Debian guest's disk, 7.3 GiB, stays in the testing clone's `build/cache/gfxref-20261104/`; everything installed for the night lives inside it. Nothing else, and nothing on Windows.
* The Lenovo: its boot partition, for one Linux boot, then restored exactly. Nothing else on its disk.
* The Mac: files only under the project's `build/`.
* Downloads, all from official sources and checked against their checksums: Alpine 3.24's netboot files and packages (alpinelinux.org); Fedora's DXVK packages (fedoraproject.org); Debian's DXVK package (debian.org); the Debian 13 cloud image and the guest's packages (debian.org); Google Chrome's package (dl.google.com). Nothing from GitHub.
* Costs: no money spent. Codex and Fable time on your accounts for the checks and the reviews.

## Where everything is

* This report: `build/flow/coordinator/graphics-research/tonight/report.md`.
* The Lenovo: `tonight/lenovo.md`, raw outputs in `tonight/lenovo-out/`.
* QEMU: `tonight/qemu-wsl.md`, raw outputs in `tonight/qemu-wsl/`.
* The sample programs: `tonight/probes.md`, which lists each one; the programs in `tonight/lenovo-probe/` and `tonight/qemu-wsl/scripts/`.
* The reviews: `tonight/codex-review.md` and `tonight/fable-review.md`; the argument: `tonight/codex-argue.md`, `tonight/fable-argue.md` and `tonight/argument.md`.
