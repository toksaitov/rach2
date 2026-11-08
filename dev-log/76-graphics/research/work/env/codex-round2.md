Labels: [measured] means supplied measurements, not repeated here; [read] means documentary evidence; [inferred] includes proposed experiments.

1. [inferred] Try the module first. Fable and the chat have the better order; my preference for replacing the kernel imposed an avoidable WSL outage.
[measured] The running kernel permits modules but lacks udmabuf, vmap_pfn and Module.symvers. [read] A matching full build supplies symbol versions; `modules_prepare` alone does not. [Kernel documentation](https://docs.kernel.org/kbuild/modules.html)
[inferred] First establish matching symbols and review the page-based vmap replacement, including unmap and lifetime handling. `CONFIG_UDMABUF` being boolean means this is an external compatibility module, not an ordinary `modprobe`.
[inferred] Switch to the exact WSL kernel with built-in udmabuf if required symbols remain unavailable, the compatibility patch grows beyond the driver, or module-specific mapping/export tests fail. A presentation failure alone does not establish a kernel fault.
[read] Selecting that kernel through `.wslconfig` requires restarting WSL, not Windows. [inferred] Drain WSL work first; retain stock-kernel rollback. [WSL configuration](https://learn.microsoft.com/en-us/windows/wsl/wsl-config)

2. [inferred] Build the matching, pinned alpha10 QEMU and renderer as a diagnostic baseline, privately, with only any necessary XSAVE repair recorded separately. My earlier suggestion to reduce/rebase the port came too early.
[read] The April renderer date comes from the supplied comparison; the NVIDIA issue reports a guest freeze on a 3070 Ti, not a demonstrated Windows GPU reset or a result on this 4080 SUPER. Fable's "without evidence" was wrong. [Issue 2](https://github.com/cmspam/winq-emu/issues/2)
[inferred] A broad rebase changes too many variables before reproducing anything. Rebase the necessary Windows changes after Linux demonstrates useful operation, or cherry-pick a specific upstream fix tied to the failure.
[inferred] After an RTX freeze, preserve logs and establish host recovery; the very next graphics test is the same Linux guest, binary and failing workload on the Radeon. Verify both Vulkan and presentation adapter selection. Do not reassign either GPU.
[inferred] Radeon success earns an AMD feature-testing environment and narrows NVIDIA investigation to mapping, synchronization and presentation differences. It does not prove an NVIDIA driver defect.
[inferred] Failure on both cards promotes native Windows gfxstream with a matching Linux guest driver. Use an available second Linux machine as the independent reference immediately; propose acquiring one if neither Windows transport passes or requires substantial ongoing repair. Muse's separate Linux reference is sound, but buying first is premature.

3. [inferred] Use G: the existing Debian 13 amd64 reference image, with frozen kernel, Mesa, Wine, DXVK, vkd3d-proton and Chromium versions. For gfxstream, record its required guest-driver variant.
[inferred] C means the supplied 64-bit DX9 route: `Crysis.exe -DEVMODE +map island +exec benchmark_gpu`, with identical assets, settings and resolution. C-W uses WineD3D; C-D uses DXVK.
[inferred] V means vkcube with finite frame counts; D means a DXVK D3D11 draw/readback probe; P means a D3D12 draw/readback probe using vkd3d-proton specifically; W means Chromium WebGPU compute/readback and rendered output.
[inferred] Run capability checks, then V/D/P/W, then C. Record unsupported stages explicitly. Verify the actual adapter and DLLs; SwiftShader or guest lavapipe cannot count as hardware success.
[inferred] First prove each row with Linux. Later substitute Rach2 while preserving that row's virtual hardware, host renderer, GPU and application versions. Compare pixels with declared tolerances, completion counters, displayed frames, resource release and frame-time distributions.
[inferred] Guest lavapipe is the software control once per accelerator. A Linux pass establishes possibility for that workload; repeated Linux failure rejects that configuration as a Rach2 reference until explained.

| Environment [inferred] | Linux reference | Workloads | Comparison and pursue/drop decision |
|---|---|---|---|
| WSL QEMU/KVM, virgl | G, virgl | C-W; capability checks for V/D/P/W | Keep for GL if C renders correctly and completes; compare with Windows virgl. No hardware-Vulkan claim. |
| WSL QEMU/KVM, Venus on host lavapipe after udmabuf | G, Venus | V/D/P/W, then C-D | Compare with guest lavapipe. Keep passing transport/readback tests; require host-visible frames separately before accepting presentation. Slow C does not invalidate a correctness reference. |
| Stock Windows QEMU/WHPX, virgl/ANGLE | G, virgl | C-W; capability checks for V/D/P/W | Compare identical C-W against WSL. Pursue if correct, unattended and useful in performance; drop as a Vulkan route if no usable Vulkan capset exists. |
| Windows WHPX, alpha10 Venus; RTX then Radeon | Same G on each GPU | V/D/P/W, C-D | Compare cards with everything else fixed. Qualify each workload independently; full Vulkan use requires all target probes, real-adapter W and presentation. Retain Radeon feature coverage even if C is slow. |
| Windows WHPX, gfxstream, conditional | G with matching gfxstream ICD | V/D/P/W, C-D | Compare against Venus on the same GPU. Pursue if it passes missing workloads reliably; drop if Linux cannot pass the required features without substantial new transport work. |
| Plain Hyper-V Linux host, nested QEMU/KVM, host lavapipe, conditional | Debian host with udmabuf; G guest | V/D/P/W, C-D | Compare with WSL Venus/lavapipe. Keep only if WSL cannot be changed and transport checks pass; otherwise avoid duplicate administration. |
| Second bare-metal Linux host, QEMU/KVM, virgl/Venus | G; host RADV or ANV | C-W, C-D, V/D/P/W | Compare guest with native Linux workloads on that same machine. Pursue if required guest features pass; cross-machine speed differences do not identify a Windows transport defect. |
| WSL Dozen behind Venus, preliminary only | WSL Linux vtest; G only after it passes | Capability, memory/sync and readback checks; then V/D/P/W and C-D conditionally | Pursue only demonstrated features. Stop at the first missing contract required by the target workload; enumeration alone earns no full guest run. |

4. [inferred] Inclusion decisions:

* DDA: exclude. [read] Microsoft excludes Pro from supported DDA hosts; the reported refusal is additional evidence, not a measurement here. [Microsoft guidance](https://learn.microsoft.com/en-us/troubleshoot/windows-server/virtualization/troubleshoot-hyper-v-gpu-assignment-partitioning-passthrough-issues)
* Deliberate GPU hang: exclude. [inferred] Use finite concurrency and QEMU process-loss tests first; deliberately threatening desktop recovery does not establish application feasibility.
* Plain Hyper-V lavapipe host: include conditionally when both WSL module loading and restarting WSL are unacceptable. [inferred] It solves kernel independence, not GPU access.
* Dozen behind Venus: include only the bounded preliminary check above. [read] The participants inspected older Mesa; missing fence-fd support alone is not decisive under current Venus requirements. [inferred] Muse's small check is better than a categorical rejection. [Venus requirements](https://docs.mesa3d.org/drivers/venus.html)