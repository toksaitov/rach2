# The argument round on the graphics plan, night of 2026-11-04

Codex and Fable each reviewed draft 4 of the graphics plan against the night's measurements, apart: `build/flow/coordinator/graphics-research/tonight/codex-review.md` and `tonight/fable-review.md`, answering `tonight/review-question.md`. Since then:

* The Lenovo results are complete in `tonight/lenovo.md`: DXVK 2.7.1 loads (once Fedora's "Wine builtin DLL" mark is cleared) and takes no adapter on the HD 4400: "Device does not support Vulkan 1.3" with hasvk at 1.2, "Device does not support required feature 'shaderInt64'" with hasvk reporting 1.3. Venus over hasvk through `virgl_test_server --venus` works: vulkaninfo shows an integrated GPU, 113 extensions, Vulkan 1.2 (1.3 with the option); vkcube on Wayland did 300 frames in 5.07 s through Venus against 4.99 s directly. Linux's KVM boots Alpine's kernel to its first process in 1.11 s against 8.84 s under TCG. The Lenovo is back in Rach2 with its boot partition restored.
* `tonight/qemu-wsl.md` has grown: a third-level guest on the testing PC boots Linux 20 to 25 times slower under nested KVM than under TCG; Debian's kernel in the guest has udmabuf; DXVK in the guest refuses Venus at 1.2.
* The chat wrote draft 5, `build/flow/coordinator/graphics-research/plan.md`, from both reviews; draft 4 is `plan-v4.md`. Read draft 5's "What the night changed" and "Decisions the night raised" first, then whatever else you need.

Answer in at most 120 lines, ASCII, `*` bullets, no em-dashes, terse, `file:line` for every claim:

1. Where the other reviewer is wrong or overstates, with the evidence; where you now agree with it against your own review, say so.
2. Where draft 5 is wrong, overstates a measurement, or misses a point either review made that should change the plan: quote the line and give the replacement.
3. Decisions A and B in draft 5: is each the right question with the right options, and is the recommendation right? Is any other decision the owner must make now missing?
4. The ten next measurements, merged and in priority order, each with the machine, the command or step, and what each outcome would change in the plan.

Rules: read and change nothing; start no subagents; no GitHub; say plainly where you are unsure.

Since both reviews were written, draft 5 has folded in each one's points that checked out against the code (both answers are in this directory). The differences the chat sees, to settle first:

* WebGPU in QEMU: Codex (`codex-review.md`, 74-6 and section 3) counts it only on an identified Venus adapter; Fable (`fable-review.md`, decision C) would keep a run that records SwiftShader on a CPU host, with a Done that does not require Venus. Which, and why? And on the Lenovo, should the desktop launch Chromium with `--enable-unsafe-webgpu`, or keep WebGPU a test-only switch?
* Fable's decision D, Rach2's suites under KVM on the testing PC: `tools/dev/testing.sh:337` pins `RACH2_QEMU_ACCEL=tcg` for the release's WSL runs, and etude otherwise picks KVM where `/dev/kvm` opens and refuses a `/dev/kvm` this user cannot open (`tools/etude/Readme.md:68`). Is anything at risk now, and what should be measured before any change?
* 78-7 on the testing PC: Fable says its numbers are Hyper-V's nesting cost and never compared; Codex says separate boot, exit microbenchmarks and workload throughput. Reconcile.
* The first measurement next: Codex put the DXVK logs first (now done: DXVK 2.7.1 refuses hasvk for shaderInt64); Fable puts Venus with blobs through QEMU on the Lenovo first. What comes first now?
