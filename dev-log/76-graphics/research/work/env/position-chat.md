# The chat's position, written before reading Codex, Fable or Muse

Claims are marked M (measured here), R (read in documents or source), I (inferred).

## The framing

Two separate questions were mixed together.

* Where the guest's CPU runs: a WHPX partition beside WSL (one level), KVM in WSL (two levels), a VM in that VM (three), or TCG.
* Which driver stands at the far end of the virtual GPU: NVIDIA's Windows Vulkan driver, D3D12 over /dev/dxg, or lavapipe.

Only a Windows process can reach NVIDIA's Vulkan driver (M: Windows has Vulkan 1.4.357 on the RTX; WSL has no NVIDIA ICD). So every Vulkan route to the RTX on this PC without whole-GPU assignment goes through a Windows process. That is why leaving native QEMU out was the real error: it was the only place the question could be answered.

## The environments

| Env | Chain from a Rach2 program | Vulkan 1.3 | DXVK, vkd3d | WebGPU on a real adapter | Work | Change to the machine |
|---|---|---|---|---|---|---|
| A. QEMU in WSL, virgl | Mesa virgl, virtio-gpu, QEMU, virglrenderer, Mesa d3d12, dxg, Windows, NVIDIA | no, GL 4.2 only (M) | WineD3D only | no | none | none |
| B. QEMU in WSL, Venus on lavapipe, udmabuf module | Venus, virtio-gpu, QEMU, virglrenderer, lavapipe, CPU | yes, CPU (I, from the Debian VM run) | yes on CPU | no, Dawn sees a CPU device (M) | a kernel build for the symbol checks and a small driver patch (M) | load one module |
| C. Debian VM in WSL | as B, one level deeper | yes (M) | DXVK yes (M) | no (M) | none | none; replaced by B |
| D. QEMU on Windows, stock | virgl, virtio-gpu, QEMU.exe, ANGLE, D3D11, NVIDIA | no: `vkr_` absent from its DLL (M) | WineD3D at best | no | none | none |
| E. QEMU on Windows, WINQ-EMU's Venus port | Venus, virtio-gpu, QEMU.exe, virglrenderer fork, NVIDIA Vulkan | probably, unproven on NVIDIA (R) | probably (I) | probably, a real adapter (I) | build two forks; carry our XSAVE patch | none |
| F. Hyper-V Linux VM, GPU-P | as A, in a VM we own | no, same D3D12-only stack (I) | no | no | a VM | new VM |
| G. Hyper-V DDA | Linux VM with NVIDIA's Linux driver | yes if allowed | yes | yes | a VM | RTX leaves Windows; refused on client Windows, 0xC035001E (R) |
| H. Rach2 on Hyper-V | VMBus, dxgkrnl protocol, libd3d12 | no | no | no | a new driver family | none |
| I. VMware, VirtualBox | their own 3D devices | no | no | no | new drivers | new hypervisor mode |
| J. lavapipe inside the guest | lavapipe in Rach2, CPU | yes, CPU | yes, CPU | SwiftShader | none beyond the plan | none |

## Answers

* Hyper-V against QEMU: QEMU with WHPX already runs on Hyper-V's hypervisor, in a partition beside WSL, at one level. Hyper-V proper adds nothing for the GPU on Pro: GPU-P gives only the D3D12 stack, DDA is refused. It would also cost a new device family in Rach2.
* RTX or Radeon to a VM: DDA is refused on Pro, so neither (R; unmeasured on this build). If it ever worked, the RTX is the likelier one: an AMD iGPU needs its ROM, and Hyper-V offers no ROM file (I).
* Sharing: on Windows each QEMU is one process and WDDM schedules them (R). Nothing isolates VRAM, and a TDR can reset the adapter under every user, WSL included (I). So give the real GPU a few slots, through the existing admission ledger, and use the Radeon for work likely to hang it.
* lavapipe: the better default for volume, and the wrong answer for GPU truth. In the guest it needs no virtio-gpu and runs on any machine; under TCG it is slow. Through Venus on the host it tests the transport at native CPU speed even when the guest is on TCG. It can never settle speed, driver bugs of real GPUs, or real-adapter WebGPU.

## Recommendation

1. E is the fast real-GPU environment, if it survives its first test on NVIDIA and without a desktop session. It removes nesting, keeps agents' accounts in WSL (interop starts Windows programs, M) and changes nothing on the machine.
2. B is the many-agents correctness environment, in the agents' own home. Worth the module even if E works.
3. J for the userland, in any QEMU, and the Lenovo stays the truth for real hardware.

Closed: C, F, G, H, I. Open: a small second Linux machine, if E fails on NVIDIA, as the owner's money decision.

## Next experiment run, by priority

1. Build WINQ-EMU's two forks from source in `D:/rach2/build/cache`, review the diff, boot the existing Debian guest with `blob=on,hostmem=4G,venus=on`, on the RTX, then on the Radeon: vulkaninfo, vkcube, DXVK, vkd3d-proton, Chromium WebGPU.
2. The same from Session 0, and from a task in the disconnected session 2: does it need a desktop?
3. Stock QEMU with `virtio-vga-gl` and `-display egl-headless`: does virgl work at all?
4. Two, four and eight guests at once, then a deliberate GPU hang: who dies?
5. The udmabuf module in WSL, then environment B.
