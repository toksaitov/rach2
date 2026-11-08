Read AGENTS.md first. You are giving a second opinion, read-only, on a plan for Rach2, an educational x86 operating system (C89 kernel, Linux-compatible system calls). Use no subagents. Change no file.

The plan: build/flow/coordinator/graphics-research/plan-v2.md. The evidence behind it: comparison.md and findings-chat.md in the same directory. Upstream sources are cloned read-only under build/research/src/ (mesa, libdrm, weston, libinput, seatd, proton, wine, a sparse linux v6.12).

Your focus is the GPU path: milestones 73 (DRM/KMS core, dma-buf and fences, fbdrm, virtio-gpu 2D) and 74 (virgl, and the Haswell HD 4400 driver for Mesa's Crocus: GGTT, ring, hardware contexts, execbuffer with relocations, aliasing PPGTT and Linux's command parser, reset, display flips on the firmware's eDP mode).

Answer, at most 60 lines, the claim first and then the evidence (file:line in the tree or a URL):
1. What would stop Crocus or Weston from working on this plan: a missing kernel interface, a wrong order, a wrong assumption. Check against Mesa's and Weston's code.
2. Is the plan right that Haswell needs aliasing PPGTT and Linux's command parser before an ordinary user's batches can render correctly and safely? If not, what is the smallest correct design?
3. Where would virgl run reliably and headless on a Windows host whose QEMU runs in WSL2 without a DRM render node?
4. The one change you would make first.
