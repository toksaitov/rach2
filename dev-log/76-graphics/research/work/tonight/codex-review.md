Paths below are relative to `build/flow/coordinator/graphics-research/`, unless stated otherwise. `linux/` and `mesa/` mean the trees under `build/research/src/`. The pending Lenovo DXVK, Venus and KVM results were not present in the reports at my last read.

* 1. Statements to replace.

* `plan.md:46`: "The testing PC has no nested virtualization (measured)"; `plan.md:337` repeats this, and `plan.md:404` lists it as a fact. Tonight's Windows updates expose SVM on all 32 threads, and Linux reached its final panic in a third-level KVM guest (`tonight/review-question.md:12`, `tonight/qemu-wsl.md:139`, `tonight/qemu-wsl.md:146`). Replace with: "The testing PC supports nested SVM under WSL. Keep KVM and TCG profiles: the measured third-level Linux boot took 60 to 71 seconds under KVM and 2.94 seconds under TCG."

* `plan.md:230`: "under TCG on the Mac and the testing PC (SVM)"; `plan.md:253`: "A nested guest under TCG". These are now unnecessarily restrictive, but replacing TCG everywhere with KVM would ignore the slowdown above. Replace with: "Use nested KVM and qualified TCG on the testing PC, and qualified TCG on the Mac. Select the development profile by the workload; retain both for comparison."

* `plan.md:298`: "The firmware may have VT-x off"; `plan.md:277`: "VT-x if the firmware has it off". `IA32_FEATURE_CONTROL=0x5`, `kvm_intel` loads, and `/dev/kvm` exists (`tonight/lenovo.md:20`). Replace with: "VT-x is enabled and locked on. No firmware change or owner sitting is needed for VMX bring-up."

* `plan.md:160`: "a setup change you approve"; `plan.md:322`: "needs your approval when 74-1 starts". All seven test accounts already joined `render` on the owner's order (`tonight/review-question.md:12`). Replace with: "The render-group change is complete. QEMU's egl-headless path works as an ordinary user with an explicit render node; verify access in each worker session."

* `plan.md:39`: "the GPU comes back from its reset with the display off"; `plan.md:365` repeats this. The raw Linux report says `gpu_reset_clobbers_display: no` and `has_reset_engine: yes` (`build/tmp/gfxref-lenovo/out/facts/debugfs/i915_capabilities:17`). Replace with: "An ordinary engine or GT reset does not imply loss of display state. Full mode setting remains necessary to recover from driver handover and VM death; those paths still need measurement."

* `plan.md:171`: "the reset, which on Haswell stops the whole GPU"; `plan.md:180`: "the whole-GPU reset Haswell has"; `plan.md:386`: "Haswell has only a whole-GPU reset". Linux reports `HAS_GPU_RESET=2` (`tonight/lenovo.md:57`), whose meaning is engine reset support (`linux/drivers/gpu/drm/i915/i915_getparam.c:87`). Linux tries engine reset before full reset (`linux/drivers/gpu/drm/i915/gt/intel_reset.c:1473`). Replace with: "Try recovery of the affected engine first, with a full GT reset as fallback. Match Linux's measured context-loss and recovery behavior."

* `plan.md:115` and `plan.md:380`: "DXVK needs Vulkan 1.3 where hasvk reports 1.2". The default remains 1.2, but the driver option reports 1.3.354; several features remain absent (`tonight/lenovo.md:27`). Replace the explanation with: "Keep WineD3D on OpenGL for Crysis. Hasvk can report Vulkan 1.3 through its driver option; eligibility for the pinned DXVK requires its actual feature checks and a successful run." The option changes the reported version in `mesa/src/intel/vulkan_hasvk/anv_device.c:974`; it does not prove application support.

* `plan.md:310`: "DXVK, vkd3d-proton and WebGPU on the Lenovo's GPU" are left out because of Vulkan 1.2. Chromium computed and rendered correctly through hasvk, including a Vulkan 1.2 run (`tonight/lenovo.md:38`, `tonight/lenovo.md:39`). Replace with: "Chromium WebGPU on Haswell is an experimental measured path, subject to pinned-stack and sandbox verification. DXVK remains unresolved. Vkd3d-proton remains unqualified; check its pinned feature requirements separately." The two silent DXVK timeouts establish neither success nor an API rejection (`tonight/review-question.md:23`).

* `plan.md:7`: "the host's GPU through virgl and Venus in QEMU"; `plan.md:156`: "GL and Vulkan test programs run on the host's GPU through virgl". The measured paths are virgl through D3D12 on NVIDIA, and Venus through CPU lavapipe (`tonight/qemu-wsl.md:62`, `tonight/qemu-wsl.md:68`). Replace with: "OpenGL uses virgl on the host's GL renderer. Vulkan uses Venus on a separately qualified Vulkan renderer. On current WSL, virgl reaches NVIDIA and Venus reaches lavapipe."

* `plan.md:163`: "Venus in QEMU over the host's lavapipe"; `plan.md:165`: "On Venus over the host's lavapipe (the testing PC's 32 threads, the Mac)"; `plan.md:310`: "They run in QEMU now". Current WSL Venus reports Vulkan 1.2, lacks synchronization2 and swapchain, and vkcube fails (`tonight/qemu-wsl.md:68`, `tonight/qemu-wsl.md:70`, `tonight/qemu-wsl.md:86`). Replace with: "Current WSL supports limited offscreen Venus work. Presentation and the pinned Direct3D stack require a host with the necessary external-memory and sync-file support. Qualify that host before promising 74-6. The Mac path remains unmeasured."

* `plan.md:402`: "lavapipe reports Vulkan 1.3 with everything DXVK and vkd3d-proton require" followed by "74-6 on Venus over the host's lavapipe". Tonight's host lavapipe reports 1.4.335, but its Venus device loses required features (`tonight/qemu-wsl.md:70`, `tonight/qemu-wsl.md:71`). Replace with: "Host Vulkan support does not establish Venus support. The current WSL kernel lacks CONFIG_UDMABUF, and this stack lacks the export and sync-file facilities needed for the planned presenting Venus path."

* `plan.md:165`: "Chromium's WebGPU on Vulkan" needs a more exact acceptance condition; `plan.md:167`: "pass or fail exactly as in the Linux guest" permits the present baseline to satisfy the wording without exercising Venus. Chromium selected SwiftShader for successful WebGPU runs (`tonight/qemu-wsl.md:101`, `tonight/qemu-wsl.md:110`). Replace with: "Require a successful compute and render readback on an identified Venus adapter. Record SwiftShader and guest-local lavapipe separately. Matching a baseline refusal documents a limitation but does not complete the promised Venus application path."

* `plan.md:188`: "both browsers' hardware decoding confirmed"; `plan.md:285`: "with GPU decoding in 75"; `plan.md:382`: "Debian's browsers can decode H.264 on the HD 4400". Firefox used VA-API; Chromium's surface export failed and it decoded on the CPU (`tonight/lenovo.md:45`, `tonight/lenovo.md:46`). Replace with: "Verify H.264 GPU decoding in ffmpeg and Firefox. Reproduce Chromium's i965 surface-export failure with the product pins, then either fix it or obtain an explicit acceptance change. Compare each browser against the same decoder path."

* `plan.md:301`: "77-2's vulkaninfo tells". The required extensions are exposed, but actual import, mapping, synchronization and presentation remain unproven (`tonight/lenovo.md:27`, `tonight/lenovo.md:67`). Replace with: "Extension enumeration clears the first prerequisite. Venus on Haswell requires buffer import/export, CPU/GPU visibility, fence and presentation tests through vtest, then through QEMU."

* `plan.md:175`: "The chipset's watchdog if the firmware allows it"; `plan.md:277`: "the watchdog measurement". Discovery is complete: iTCO version 2, inactive, nowayout 0; it was not armed (`tonight/lenovo.md:21`). Replace with: "Qualify the discovered iTCO watchdog by a controlled expiry and recovery before relying on it during hang experiments."

* `plan.md:217`: "whether the firmware enabled VT-x", plus the undifferentiated reference work in `plan.md:277`, is partly complete. Replace with: "Reuse tonight's hardware inventory, EDID, VBT, OpRegion and VT-x evidence. Complete the reference on pinned Debian with decoded dumps, conformance workloads, games and repeatable measurements." The missing work is explicit in `tonight/lenovo.md:63`; Alpine's versions differ from the product pins (`tonight/lenovo.md:8`, `plan.md:145`).

* 2. Task changes.

* 73-1, 73-9 and 73-18: add discovery by driver identity, rather than assuming card0 or debugfs minor 0. Tonight i915 was card1, renderD128 and debugfs dri/128 (`tonight/lenovo.md:16`). Use the captured query/debugfs inventory to make the contracts concrete (`plan.md:133`, `plan.md:141`, `plan.md:150`, `tonight/lenovo.md:57`).

* 73-2 and 74-3: qualify mappings of large, high-address PCI BARs explicitly. Tonight's 4 GiB host-visible window starts at 14 TiB (`tonight/qemu-wsl.md:55`), whereas the kernel MMIO window is 128 MiB (`rach2/sys/arch/x86_64/cpu.h:146`). The Done must cover selective mappings and revocation without requiring a permanent mapping of the whole BAR; give i686 its own addressable display profile (`plan.md:134`, `plan.md:162`, `plan.md:120`).

* 73-3 and 80-2: add a named qualification step for lavapipe's dma-buf export and sync-file needs when Rach2 hosts QEMU. Assign `/dev/udmabuf` compatibility, or another proven Linux-compatible allocation path, if the pinned software renderer needs it. This prerequisite is absent from the current task descriptions (`plan.md:135`, `plan.md:265`). The relevant capability checks are `mesa/src/gallium/drivers/llvmpipe/lp_screen.c:297` and `mesa/src/gallium/frontends/lavapipe/lvp_device.c:1289`.

* 73-10: specify the Lenovo's PS/2 Synaptics touchpad and i8042 keyboard. An I2C HID driver is unnecessary for this machine (`plan.md:142`, `tonight/lenovo.md:19`). Keep the USB input cases.

* 73-6, 73-8 and 73-16: make virtio-vga variants the measured SeaBIOS baseline. Keep non-VGA virtio-gpu as a separate firmware/boot-path qualification. Debian's GRUB reset before loading Linux with both GL and plain non-VGA devices; virtio-vga-gl booted (`tonight/qemu-wsl.md:45`). Do not claim that UEFI or Rach2's GRUB already fixes it (`plan.md:138`, `plan.md:140`, `plan.md:148`).

* 73-13, 73-14 and 77-2: retain the Debian and Weston pins. Tonight's cage, Alpine Wine, Mesa and Firefox runs are preliminary evidence, not replacements for those acceptance configurations (`plan.md:145`, `plan.md:146`, `plan.md:217`, `tonight/lenovo.md:8`, `tonight/lenovo.md:50`). Reuse the local page and diagnostic scripts after adapting their paths and lifecycle.

* 73-16: add guest-side GL capture and checksum collection. QEMU screendump returned "no surface" for egl-headless GL scanout; DevTools capture worked (`tonight/qemu-wsl.md:92`). A browser screenshot proves browser output, not physical scanout, so retain independent display verification (`plan.md:148`, `plan.md:282`).

* 73-16, 75-13 and 80-7: fix and record the actual rendering size, warmup, repetitions and synchronization mode. Glmark2 chose 5120x2160 while the browser used 1280x800, and browser fill measurements varied by about 50% (`tonight/qemu-wsl.md:85`, `tonight/qemu-wsl.md:106`). These results cannot apply the accepted 10% gate or isolate transport overhead (`plan.md:103`, `plan.md:187`, `plan.md:270`).

* 73-17, 74-5 and 75-12: make GPU-process sandbox status, renderer identity and successful pixel readback explicit Done evidence. Chromium reported an unsandboxed GPU process on both machines (`tonight/lenovo.md:44`, `tonight/qemu-wsl.md:108`). Keep the sandbox requirement; the present runs do not satisfy it (`plan.md:149`, `plan.md:164`, `plan.md:186`).

* 74-1: mark the group change and ordinary-user egl-headless/NVIDIA proof complete. Keep the reusable worker profile and access checks. Use `eglinfo -B -p surfaceless` for the ordinary-user preflight; the failed GBM default-node probe does not justify adding `video`, running QEMU as root, trying a virtual X server or building private QEMU (`plan.md:160`, `tonight/qemu-wsl.md:14`, `tonight/qemu-wsl.md:16`, `tonight/qemu-wsl.md:62`).

* 74-2 and 74-3: use the measured capsets 1, 2 and 4 and negotiated context-init/blob features as the initial reference. Do not require a QEMU `context_init` property, and do not advertise capset 6 on this stock stack (`plan.md:161`, `plan.md:162`, `tonight/qemu-wsl.md:54`). Keep mapping, fence and reset tests; advertised features alone do not complete them.

* 74-4: split offscreen Vulkan qualification from WSI/presentation qualification. Add a host prerequisite for dma-buf export, sync-file import and swapchain before vkcube acceptance. First try the existing Debian VM as a software Venus host: its kernel has CONFIG_UDMABUF and the device exists, but the rendering path is not yet proven (`plan.md:163`, `plan.md:167`, `tonight/qemu-wsl/guest-udmabuf.txt:7`).

* 74-5: start virgl acceptance after 74-2; do not make its GL/browser portion wait for Venus repairs. Keep a later Vulkan/device-loss portion. Use the guest's measured GL 4.2 and GLES 3.1, not the host's GL 4.6, when selecting advertised-API tests (`plan.md:164`, `tonight/qemu-wsl.md:62`).

* 74-6: add a working presenting-Venus host as a prerequisite. Separate Direct3D work from the WebGPU adapter-selection work, so the latter need not wait for Wine tasks 77-1 and 77-3. Record the Ozone platform and chosen adapter. Neither successful SwiftShader execution nor a generic "Hardware accelerated" row counts as Venus acceptance (`plan.md:165`, `tonight/qemu-wsl.md:101`, `tonight/qemu-wsl.md:108`).

* 75-1: replace watchdog discovery with qualification. Retain register dumps, CRC and bounded driver waits. Finding an inactive watchdog does not prove that expiry resets this firmware (`plan.md:175`, `tonight/lenovo.md:21`).

* 75-3, 75-5, 75-7, 75-8 and 75-11: turn tonight's traces into an owned ABI checklist, including mmap offsets, tiling/domain operations, queries, context parameters, timestamp reads and reset statistics. Distinguish optional probes from required behavior; do not implement a success stub merely because a probe occurred (`plan.md:177`, `plan.md:179`, `plan.md:181`, `plan.md:182`, `plan.md:185`, `tonight/lenovo.md:57`, `tonight/lenovo.md:58`).

* 75-5 and 75-11: add explicit dependencies on 73-3 where execution fences and timeline/sync-file operations are exercised. Split headless hasvk bring-up from display presentation, whose acceptance also needs the display tasks. Tonight's vkcube trace includes timeline waits, transfers, dma-buf sync-file import/export and atomic commits (`plan.md:179`, `plan.md:185`, `tonight/lenovo.md:59`).

* 75-6: implement engine reset with full-reset fallback, then measure guilty and innocent contexts, concurrent render/video work and display survival. Retain RC6/RPS acceptance; tonight supplies initial reference values, not reset behavior (`plan.md:180`, `plan.md:190`, `tonight/lenovo.md:18`, `linux/drivers/gpu/drm/i915/gt/intel_reset.c:1473`).

* 75-9 and 75-10: use the captured panel timing, lane count, link rate, clocks, EDID and VBT for initial bring-up. Decode the VBT and collect firmware/Linux register dumps before fixing port assumptions. The compositor's modifier list does not prove every plane can scan out every listed format (`plan.md:183`, `plan.md:184`, `tonight/lenovo.md:17`, `tonight/lenovo.md:29`, `tonight/lenovo.md:63`).

* 75-11 and 75-12: add the small hasvk WebGPU compute/render check, with the exact browser flags and sandbox state recorded. Repeat default and 1.3-report configurations without making the override a global default. This can precede DXVK and native-context work (`plan.md:185`, `plan.md:186`, `tonight/lenovo.md:27`, `tonight/lenovo.md:38`).

* 75-14: split decoder correctness from browser integration. Use ffmpeg/software frame hashes, Firefox's actual decoder log and Chromium's export error as separate cases. Make shared-buffer/export dependencies explicit. VP9 and AV1 remain CPU paths; discovering JPEG and encoding support does not add encoding to the deliverable (`plan.md:188`, `plan.md:190`, `tonight/lenovo.md:30`, `tonight/lenovo.md:31`, `tonight/lenovo.md:45`).

* 77-1, 77-2, 77-3 and 77-5: record DLL format and loader provenance, then repeat on the pinned GE-Proton. Debian's glibc winelib DXVK could not load in Alpine's musl Wine (`tonight/lenovo.md:53`). Add WineD3D/Vulkan as a research comparison, since its small D3D11 readback worked; the 26.6 ms versus 182.3 ms result is not a game benchmark (`tonight/lenovo.md:50`). Keep Crysis acceptance on WineD3D/OpenGL and umu (`plan.md:115`, `plan.md:216`, `plan.md:218`, `plan.md:220`, `plan.md:224`).

* 78-2, 78-5 and 78-6: add a nested-KVM profile with the measured SVM feature set. Preserve software handling where decode assists and other optional features are absent. Keep TCG qualification and the decoder task; real SVM exposure does not remove either (`plan.md:235`, `plan.md:238`, `plan.md:239`, `tonight/qemu-wsl.md:139`).

* 78-3: begin the Lenovo VMX qualification without a firmware sitting. A pending Linux-under-KVM boot pass would qualify the reference route, not Rach2's VMX implementation (`plan.md:236`, `tonight/lenovo.md:20`, `tonight/review-question.md:23`).

* 78-6 and 78-7: move Linux fixture preparation and reference measurements before the dependency on completed 78-1 and assignment session 79-1. Separate boot time, VM-exit microbenchmarks and useful workload throughput. Third-level boot time cannot measure Rach2's big-kernel-lock overhead (`plan.md:239`, `plan.md:240`, `plan.md:305`, `tonight/qemu-wsl.md:145`).

* 79-1 and 79-4: explicitly boot the Lenovo reference with `intel_iommu=on`; tonight's interrupt remapping did not enable DMA translation or produce groups (`plan.md:250`, `tonight/lenovo.md:22`). For emulated assignment, compare KVM and TCG after qualifying the required nesting; do not assume KVM is faster (`plan.md:253`, `tonight/qemu-wsl.md:149`).

* 79-5 and 80-7: separate ordinary GPU-reset recovery from assignment/VM-death recovery. Keep full mode-setting and ownership-transfer work; engine-reset support cannot remove it (`plan.md:254`, `plan.md:270`, `build/tmp/gfxref-lenovo/out/facts/debugfs/i915_capabilities:17`).

* 80-1: start the Linux-to-Linux relay, copying-protocol and readback fixture before Rach2's hypervisor is complete. Keep the Rach2-hosted and assigned-GPU stages behind their real prerequisites. Benchmark the nested route before choosing it for long runs (`plan.md:264`, `tonight/qemu-wsl.md:149`).

* 80-2, 80-4 and 80-5: make Venus admission depend on actual shared-buffer visibility and synchronization, not vulkaninfo. A successful local vtest run would reduce server uncertainty but would not prove QEMU blob mappings, reversed page mapping or revocation (`plan.md:265`, `plan.md:267`, `plan.md:268`, `tonight/lenovo.md:67`).

* 80-6: keep the matched experimental stack. Stock WSL exposes no native-context capset, so it cannot remove that build. Move the Linux forward-direction qualification ahead of 80-5; keep reversed native-context integration dependent on 80-5 (`plan.md:269`, `tonight/qemu-wsl.md:54`).

* 3. Risks and owner decisions.

* Can the firmware-VT-x and ordinary-user WSL-render-access risks be closed, or should they remain prerequisites? Close them: the required states and rendering were measured (`tonight/lenovo.md:20`, `tonight/review-question.md:12`, `tonight/qemu-wsl.md:62`). Keep VMX correctness and each worker's actual access checks open (`plan.md:236`, `plan.md:160`).

* Should 74's software Venus host be the existing Debian VM or a custom WSL kernel with CONFIG_UDMABUF? Recommend qualifying the Debian VM first; it already has the device and avoids a Windows configuration change. Choose a custom WSL kernel only if nested performance or compatibility makes that route unsuitable (`tonight/qemu-wsl/guest-udmabuf.txt:7`, `tonight/qemu-wsl.md:76`, `tonight/qemu-wsl.md:149`). Neither remedy is yet a complete Venus pass.

* Should the testing PC replace TCG with nested KVM everywhere, or retain both? Recommend both, selected per workload. The third-level Linux boot was much slower under KVM; useful rendering throughput and Rach2 SVM behavior remain unmeasured (`tonight/qemu-wsl.md:145`, `plan.md:239`).

* Should 75 require a Chromium H.264 GPU-decode fix, or accept Firefox hardware decoding with Chromium software fallback? Recommend repeating the failure on the pinned Debian stack first, then accepting the split if it persists and playback meets the agreed comparison. This changes the plan's "both browsers" requirement only with the owner's decision (`plan.md:188`, `tonight/lenovo.md:45`). It does not require removing video playback from the user goal (`plan.md:16`).

* Should working Haswell WebGPU become an experimental acceptance case, or remain excluded until the full browser path is qualified? Recommend adding the measured smoke case now and retaining sandboxed product acceptance as the condition for shipping it (`plan.md:310`, `plan.md:186`, `tonight/lenovo.md:38`, `tonight/lenovo.md:44`).

* Should the WebGPU-on-Venus requirement accept SwiftShader, or retain a verified Venus adapter and qualify another configuration? Recommend retaining Venus. Tonight's SwiftShader pass does not exercise the requested transport; a hardware Vulkan host is a candidate, not yet proof of browser adapter selection (`plan.md:165`, `tonight/qemu-wsl.md:110`).

* Should tonight reopen the Crysis renderer, umu route, 10% gate, trial-root or Steam decisions? Recommend keeping all of them. No measurement makes them impossible. Crysis was not run, the DXVK result is pending, and the noisy exploratory measurements cannot replace the matched performance reference (`plan.md:92`, `plan.md:99`, `plan.md:103`, `plan.md:115`, `tonight/fable-checks.md:133`, `tonight/review-question.md:23`, `tonight/qemu-wsl.md:106`).

* Can the watchdog, GPU recovery, assignment and native-context risks now be closed, or only narrowed? Recommend narrowing them. Watchdog discovery and engine-reset capability are proven; expiry, actual hang recovery, assignment and native-context rendering are not (`plan.md:299`, `plan.md:300`, `plan.md:302`, `tonight/lenovo.md:21`, `tonight/lenovo.md:22`, `tonight/qemu-wsl.md:54`).

* Should browser sandbox failure and false acceleration reports be recorded as limitations, or treated as acceptance defects? Recommend acceptance defects until explained and corrected. Both machines reported an unsandboxed Chromium GPU process, and successful WebGPU/video status labels concealed different execution paths (`plan.md:149`, `plan.md:186`, `tonight/lenovo.md:44`, `tonight/lenovo.md:45`, `tonight/qemu-wsl.md:108`).

* 4. Next measurements, in priority order.

* First, collect the pending Lenovo DXVK logs. Run the same probe with `DXVK_LOG_LEVEL=debug`, `WINEDEBUG=+loaddll` and explicit native DLL overrides, on hasvk's default report and `hasvk_report_vk_1_3_version=true`. Confirm DLL loading and the selected device before interpreting silence. Correct pixels would add a pinned-DXVK comparison to 77-2; a named missing feature would bound that version; a loader failure or another timeout leaves GPU compatibility unresolved (`plan.md:216`, `plan.md:217`, `tonight/review-question.md:23`).

* Second, qualify the alternative Venus host on the testing PC's Debian VM. Check `/dev/udmabuf` access, select lavapipe explicitly, run full `vulkaninfo`, then boot a child with `virtio-vga-gl,venus=on,blob=on,hostmem=4G`. Inside it, select only Venus and run vkcube plus offscreen upload/readback and mapped-memory cases. A full pass enables 74-4 and Direct3D trials; failure identifies the remaining prerequisite before considering a WSL kernel change (`plan.md:163`, `plan.md:165`, `tonight/qemu-wsl/guest-udmabuf.txt:7`).

* Third, isolate WebGPU adapter selection in that guest. Use a fresh Chromium profile, explicit Vulkan ICD selection, the measured WebGPU switch, and separate Wayland/X11 runs. Record Dawn diagnostics, adapter identity, compute readback, rendered pixels and sandbox state. If Venus is selected and works, retain the software host; if only SwiftShader works, qualify a hardware Vulkan host. Do not conclude that every CPU Venus configuration is impossible from tonight's selection alone (`plan.md:165`, `tonight/qemu-wsl.md:101`, `tonight/qemu-wsl.md:110`, `tonight/qemu-wsl.md:111`).

* Fourth, complete the Lenovo Venus experiment: `VK_DRIVER_FILES="$HASVK" virgl_test_server --venus`, then `VN_DEBUG=vtest,init VK_DRIVER_FILES="$VENUS" vulkaninfo`, followed by real upload/readback, external-buffer and fence cases. Repeat through QEMU if vtest passes. Enumeration alone narrows risk 5; correct mapped-buffer execution advances 80-5; a reproducible feature or mapping failure makes Haswell Venus conditional while virgl remains the first rung (`plan.md:57`, `plan.md:268`, `plan.md:301`, `tonight/review-question.md:23`).

* Fifth, finish the KVM comparisons on both machines. On the Lenovo, collect the pending VMX serial result. On WSL, boot the same prepared payload with explicit KVM and TCG profiles, first one vCPU, then two; separate firmware time from kernel progress and collect exit counts. A Lenovo pass qualifies the Linux VMX route; failure adds a prerequisite to 78-3 without reviving the disproved firmware-off claim. WSL results determine development profiles, not whether to delete SVM (`plan.md:235`, `plan.md:236`, `plan.md:240`, `tonight/review-question.md:23`, `tonight/qemu-wsl.md:141`).

* Sixth, repeat browser checks on pinned Debian and Weston on the Lenovo. Inspect `chrome://sandbox`, `chrome://gpu`, process diagnostics and Firefox `about:support`; run default settings before overrides. Play identical H.264 clips while collecting actual decoder logs and frame counts. This settles sandbox failures and whether Chromium's i965 export failure applies to the product configuration (`plan.md:145`, `plan.md:149`, `plan.md:188`, `tonight/lenovo.md:44`).

* Seventh, complete the matched reference on the Lenovo: fixed-size repeated GL scenes, verified software controls, power/frequency counters, pinned GE-Proton logs and the agreed Crysis route. Confirm WineD3D, WoW64, fsync and umu separately. This supplies the missing game feasibility result and a usable 10% comparison; tonight's small D3D11 operation cannot supply either (`plan.md:103`, `plan.md:217`, `plan.md:224`, `tonight/lenovo.md:50`).

* Eighth, qualify recovery on the Lenovo in the planned machine session. First prove watchdog expiry and return. Then compare engine-reset and full-reset behavior with concurrent render/video work and recorded display state. Collect the timed Linux return result separately. This settles recovery assumptions without confusing ordinary GT reset with VFIO ownership transfer (`plan.md:175`, `plan.md:180`, `plan.md:303`, `build/tmp/gfxref-lenovo/out/facts/debugfs/i915_capabilities:17`).

* Ninth, perform 79-1's IOMMU-on reference on the Lenovo: boot with `intel_iommu=on`, collect groups and reserved regions, then use the planned assignment fixture to test guest rendering and return to the host driver. The current lack of groups is a boot configuration result, not evidence that VT-d or assignment cannot work (`plan.md:250`, `tonight/lenovo.md:22`).

* Tenth, qualify the remaining fixtures before treating them as acceptance evidence: on WSL and the Mac, boot Rach2's own GRUB profiles, verify GL capture and high-BAR mappings; on the Lenovo, decode VBT/EDID and capture firmware registers; on the matched research stack, request capset 6 and render a triangle. Keep physical HDMI, sound routing, Weston handover and VM-death recovery open until their own tests run (`plan.md:146`, `plan.md:148`, `plan.md:183`, `plan.md:269`, `plan.md:272`, `tonight/lenovo.md:9`, `tonight/lenovo.md:63`).