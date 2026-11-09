This review covers rows 9 to 11 only. `plan.md` means draft 5 in `build/flow/coordinator/graphics-research/`; other paths below are relative to its `run2/` directory. Task numbers remain draft 5's numbers.

1. Errors.

* `step9.md:255`, "sandboxes on can only mean ... an unsandboxed GPU process", is unsupported. The default reports `Sandboxed: false`; early startup fails to load EGL (`step9/browsers/chromium-default-gpu.txt:164`, `sandbox-early.log:7`). These establish two failing configurations, not impossibility. Keep the sandbox requirement and its unresolved fault.

* `step9.md:253`, "Chromium exports a VA surface before decoding into it", goes beyond the trace. The log establishes `invalid VASurfaceID` at export and subsequent software decoding (`step9/browsers/chromium-h264-default.log:27`, `chromium-h264-default-media.json:152`). It does not distinguish a missing surface from missing backing storage or identify its creation call. Neither proposed fix is established.

* `step9.md:253`, "its drops are not the GPU decoder's", is too strong. Firefox drops frames with both decoders, which supports investigating a shared presentation problem; it does not exclude a decoder contribution. The measured H.264 counts remain valid (`step9/browsers/firefox-h264-default.json:1`, `step9b/browsers/firefox-h264-default.json:1`).

* `step9.md:147`, "MPEG-2 and H.264 ... and nothing else", contradicts VC-1 and JPEG decoding in `lenovo-setup/facts/vainfo.txt:27` and `:31`. Correct the capability list. The H.264/VP9 comparison is unchanged.

* `step9.md:26` calls vkd3d-proton `1.1-5687-g8293130eb`; its runtime identifies `v3.1-1-g8293130eb` (`step10/proton-g.txt:6`). Correct the pin record. The refusal remains valid.

* `step9.md:169` attributes 7,360 Firefox failures to RGB10_A2 uploads. Summing the saved per-page results gives 6,991 across 26 pages. The 7,360 figure includes all failures under `conformance2/textures`, including 228 feedback and 139 unpack failures (`step9/webgl/firefox-summary.txt:6`, `:17`, `:29`). The total of 8,013 is correct.

* `step9.md:218`, "the builds paused or finished", contradicts `step9.md:160`, which says builds ran beside WebGL conformance. Its runner does not pause them (`step9/webgl.sh:11`). Retain the recorded results, but do not call their timing an idle-machine reference.

* `step9.md:255`, "the two together give the fastest compute", is not established by the one-shot checks. Both switches gave 16.5 ms and later 25.3 ms, against 24.6 ms with the blocklist switch alone (`step9/browsers/chromium-both.json:1`, `chromium-blocklist.json:1`, `step10/host-web.txt:2`). Adapter selection is settled; the speed recommendation is not.

* `step9b.md:44`, "they are shader compiles", exceeds the evidence. The cache comparison establishes a substantial cache-dependent first-use cost; it does not identify every hitch. The report itself records different page-cache preparation (`step9b.md:26`). Keep the frame counts and the cache requirement; qualify the cause.

* `step10.md:52`, "every unmap ... about 50 ms", is too broad. Thirty-two unmaps/frees together took 15.888 ms, followed by a 116.761 ms map (`step10/blob-guest-2.txt:10`). The remaining maps' approximately 3.5 ms is an average, not a fixed cost: the recorded maximum, 52.243 ms, exceeds even the first map. Distinguish serial release/reallocation from batching.

* `step10.md:76` identifies huge-page tail reference counts as the exact KVM cause without tracing the failing page. The saved source explicitly discusses non-compound higher-order allocations (`step10/linux-src/virt/kvm/kvm_main.c:2944`). The `never`/`always` comparison isolates the huge-page trigger, but not the complete reference-count mechanism (`step9b/kvm/kvm-probe.txt:36`, `kvm-always.txt:5`).

* `step10.md:127`, "85 to 95 percent ... fill and ALU rate", overstates ALU performance. One guest sample takes 22.2 ms against the host's 16.1 ms, about 73 percent of its rate (`step10/web/webgl-guest.txt:5`, `step10/host-web.txt:3`). The fill-rate range stands.

* `step11.md:30`, "the 128 KiB ... are all 0xff", is unsupported. Only the first 32 bytes were displayed, from a 4 KiB read (`step11/facts/facts.txt:193`, `step11/scripts/facts.sh:35`). Say that no usable ROM was returned. Assignment without a ROM remains proven.

* `step11.md:105`, "about 15 percent from VT-d alone", is not established. Translation, huge-page allocation and stolen-memory use changed together (`step9b.md:17`, `:42`). The assigned/native ratios are measurements of these configurations, not a general prediction for the future driver VM.

2. Decisions settled by rows 9 to 11.

* Row 9 settles Linux's basic Crysis speed risk on WineD3D GL: the two translation-off runs average 46.08 and 45.64 FPS over loops 2 to 4 at Low 1366x768 (`step9b/crysis/compare.txt:3`, `:7`). It does not complete the first-level criterion, the umu route, sound acceptance or native-game references required by `plan.md:129`, `:254` and `:261`.

* Decision B's first option has supporting evidence: Chromium software H.264 drops 0 of 1,738 frames; Firefox hardware H.264 drops 62 of 1,727 with translation on and 50 of 1,724 off (`step9/browsers/chromium-h264-default.json:1`, `firefox-h264-default.json:1`, `step9b/browsers/firefox-h264-default.json:1`). This supports the proposed tradeoff; it does not constitute the owner's acceptance. Option 2 still needs a traced fix.

* Decision C's technical question is settled for the tested native Chromium: no switch gives no adapter, `--enable-unsafe-webgpu` gives SwiftShader, and `--ignore-gpu-blocklist` gives Intel gen-7 with correct compute and pixels. Its picture produced 480 frames in 8.0098 seconds (`step9/browsers/chromium-default.json:1`, `chromium-unsafe.json:1`, `chromium-blocklist.json:1`, `chromium-pic-blocklist.json:1`). Test-only remains a supported recommendation, separate from the unresolved sandbox.

* Row 10 clears risk 5's forward Linux mapping check. Venus buffers have zero mismatches and near-native steady bandwidth (`step10/blob-guest-2.txt:14`, `:17`, `:20`). Presentation also works: SDL's 300/900-frame runs imply 56.4 FPS; GTK gives about 50 FPS (`step10/cube-sdl-timing.txt:11`, `:15`, `cube-gtk-timing.txt:2`, `:5`). This adds a measured Lenovo GPU-host option to decision A; it does not select the testing PC's host arrangement.

* Row 10 does not establish WebGPU on Venus. Chromium returns null or SwiftShader, and ANGLE Vulkan fails during cross-renderer resource import (`step10/web-a.txt:15`, `:21`, `step10/web/vulkan-fatal-info.txt:7`). The recommendation to keep SwiftShader as the working guest reference is supported. It cannot complete the promised Venus WebGPU work.

* Row 11 supports proceeding with device assignment. The guest scores 1185/1183, and a killed QEMU releases the GPU through AF_FLR; the host then renders kmscube at 60.01 FPS (`step11/glmark2/wayland-gl.log:45`, `wayland-es2.log:45`, `step11/killtest.txt:11`, `step11/lenovo-host/kmscube-back-c.log:20`). Risk 3 is cleared for this Linux configuration. Risk 6 remains open because arming and feeding the watchdog did not test expiry (`step11/wdog-arm.txt:18`).

* Rows 1 to 8, including the native-Windows-QEMU question and TCG default, are outside this assigned part (`codex-qb.md:3`).

3. Changes to draft 5.

* `plan.md:7`, "WebGPU ... on Venus ... the Lenovo under Linux", and `:112`, "Chromium on it is measured in 77-2", need: "Native hasvk WebGPU works with the blocklist override; WebGPU through Venus remains blocked on the tested Linux stack." Preserve that work in 74-6; the null/SwiftShader results do not satisfy it (`step10/web-a.txt:15`, `:21`).

* `plan.md:22`, "vkd3d-proton was not run anywhere", `:253`, "never run on the Lenovo", and `:350`, "not run", are obsolete. Replace them with the tested GE-Proton11-12 versions and observed refusals: DXVK's Vulkan/feature checks and vkd3d-proton's maintenance5/6 requirement (`step10/probes-detail.txt:28`, `step10/proton-g.txt:6`, `:11`). Update 77-1 and the current explanation at `plan.md:152`; retain older observations only as dated history.

* `plan.md:26`, "Still to qualify ... pinned Debian, Crysis, a blob ... GPU assignment", should name the completed checks and the remaining acceptance work. Native context remains untested; Crysis's complete acceptance remains open. Do not leave the completed Linux assignment and blob checks on the critical path (`step9b/crysis/compare.txt:3`, `step10/blob-guest-2.txt:24`, `step11/killtest.txt:11`).

* `plan.md:29`, `:116`, `:222` and `:346` retain uncertainty over the WebGPU switches or picture. Replace it with the measured switch matrix and captured native picture. In 75-11, use `--ignore-gpu-blocklist` for the established GPU case; keep unsafe-WebGPU testing separate (`step9/browsers/chromium-blocklist.json:1`, `chromium-pic-blocklist.json:1`).

* `plan.md:114`, "Check it again on the pinned Debian", and 75-14's short Alpine drop reference at `:225` are obsolete. Record the longer Debian H.264/VP9 references, including the IOMMU configuration. Keep decoder identification and frame-hash checks; browser playback alone did not complete those checks (`step9/browsers/firefox-h264-default.json:1`, `step9b/browsers/firefox-h264-default.json:1`).

* `plan.md:33`, "DMA remapping is off unless asked for", and `:287`, "there are no groups", describe Alpine, not this Debian. Replace them with: "This Debian boot translates DMA by default; the GPU is alone in group 0; `intel_iommu=off` removes the groups." Preserve identity-based discovery in 73-1: Debian's i915 is card0, unlike the earlier Alpine instance (`lenovo-setup.md:88`, `:93`, `step11/facts/facts.txt:73`).

* `plan.md:34` and `:340` require Debian to repeat the alarm return. Mark that reference complete: Rach2 returned after the Linux power-off, and the restored boot partition matched the saved image (`step11/rach2-wait.txt`, `step11/p1-restore.out:2`, `step11/p1-readback-mac.txt:1`).

* `plan.md:71`, `:287` and `:291` make legacy IGD assignment central. Replace it with the proven q35, SeaBIOS, direct-kernel-boot configuration with `x-igd-opregion=on`. Remove mandatory VBIOS extraction. Guest GRUB and OVMF remain unqualified alternatives, not prerequisites (`step11.md:44`, `:102`).

* `plan.md:80` and risk 5 at `:338`, "what stays to be proven is ... mapped memory in a VM", are obsolete for the forward Linux path. Replace them with the successful blob result and its allocation/release costs. The reversed mapping, revocation and cache-mode proof in 80-5 remain necessary (`step10/blob-guest-2.txt:5`, `:10`, `:17`).

* `plan.md:94` and 80-4 at `:304`, "vfio-pci ... no-IOMMU mode", cannot use this Debian kernel: `CONFIG_VFIO_NOIOMMU` is unset (`step11/facts/facts.txt:322`). Choose the small module already contemplated, or qualify an emulated IOMMU. If choosing the module, put its interrupt support in 80-4; avoid making 80-4 depend on all of 80-5, which already waits on 80-4.

* 73-13's "Mesa 25.0.7" at `plan.md:182` is not a qualified pin for all promised paths. The tested Venus lacks features needed by the selected DXVK and by WoW64 Vulkan mappings (`step10/probes-detail.txt:31`, `:55`). Add one coordinated stack-qualification task covering 73-13, 74-4, 74-6 and 77-1. The run does not prove that upgrading Mesa alone, or choosing 26.1.6, fixes everything.

* 73-2, 73-3, 75-3 and 78-1 need explicit coverage of GPU-backed mappings in KVM slots, including compound pages, references, revocation and failure before an unusable mapping reaches a running guest. The stopped VM supplies a concrete regression case; the precise Linux failure mechanism remains to be traced (`step9b/kvm/kvm-always.txt:5`, `:14`).

* 73-14's "Weston with kiosk-shell" and 77-4's "Fullscreen" need a real fullscreen Xwayland acceptance case. The Crysis reference used cage after Weston's placement failed. Fix the chosen desktop path or name an explicit game launcher arrangement; do not silently substitute cage for the owner's desktop choice (`step9.md:44`, `:45`, `:250`).

* 73-16 and 75-13 must record the renderer identity, IOMMU state, cache preparation, exact case list and concurrent work. Use translation-off Linux for today's native Rach2 comparison and translation-on Linux beside assignment. Compare failures by case, not just pass totals (`step9b.md:26`, `step9/webgl/firefox-summary.txt:3`, `step11/glmark2/compare-gl-step9.txt:1`).

* 73-17's "first thing to try" at `plan.md:186` has now failed. Replace that investigation step with tracing sandbox startup and the denied EGL load. Keep "sandboxes on" in 73-17 and 75-12; neither browser's complete sandbox acceptance has passed (`step9/browsers/sandbox-early.log:7`, `step9.md:156`).

* 75-1 still requires controlled watchdog expiry. Arming, renewal and disarming are useful completed preparation, not its Done (`plan.md:212`, `step11/wdog-arm.txt:18`, `step11.md:91`).

* 75-6 and 75-13 gain reported repeatable hang triggers: multisample `read-front` and `arb_copy_image-formats --samples=2`. Preserve their exact commands and recovery evidence, then use them for the driver comparison. Do not substitute a clean-suite requirement for Linux's recorded failures (`step9.md:199`, `plan.md:217`, `:224`).

* 75-9 and 75-10 must preserve Haswell's scanout-format restrictions. The direct glmark2 reference needs `--visual-config a=0`; a successful GL render does not make ARGB8888 a supported primary-plane format (`step9.md:101`, `step9/glmark2/drm-gl-xrgb.log`).

* 77-1's release discovery is done, but package integration and dependency qualification remain. 77-2 becomes a remainder task: native games, the accepted umu route, complete first-level gameplay, sound and outstanding browser checks. Remove its obsolete wait on release 71 and its repeated Linux installation work (`plan.md:253`, `:254`; `step9.md:26`, `step11/rach2-end.txt`).

* 77-3 and 74-6 need both 32-bit and 64-bit Vulkan probes. Kernel support for addresses below 4 GiB does not by itself solve the observed Wine/Venus allocation failure (`plan.md:235`, `step10/probes-detail.txt:47`). Keep this distinct from the CPU's compat ABI.

* 77-5's WineD3D Vulkan comparison remains useful for small programs, but Crysis has a WineD3D crash as its reference. Replace any implication that the earlier small-probe speedup predicts a working Vulkan game route (`plan.md:257`, `step9/crysis-vk-1366-low-ffp/Game.log:86`).

* 73-14, 77-4 and 77-9 need a functioning, possibly silent, audio output during gameplay. `s_SoundEnable=0` changed the game's dialog timing and HUD state; use an inaudible sink instead. The existing save that records a hidden HUD must not become the clean acceptance fixture (`step9/crysis-dialog-game.log:121`, `:133`, `crysis-dialog-sound-game.log:115`, `:129`).

* Keep 73-15, 77-6 and 77-9's update-persistence work. Crysis was absent after the root update; the cache instructions explicitly say `/opt` is rewritten (`lenovo-setup/p3-ro-check.txt:26`, `build/cache/crysis/README:15`). This is evidence for durable storage, not completion of it.

* 79-3 at `plan.md:289` can drop required legacy-IGD bridge emulation and VBIOS acquisition for this configuration. Keep normal region reporting, the OpRegion, MSI, BAR mappings, reset and ownership. Implement graphics RMRR handling from Linux's reserved-region semantics; do not generalize the graphics region's relaxability to every reserved region (`step11/facts/facts.txt:74`, `step11/qemu-src/hw/vfio/igd.c:573`).

* 79-5 at `plan.md:291`, "or the reboot ... necessary", should require the measured reset and driver reinitialization path. Its Done must include clean shutdown and VMM death under load, followed by working display and rendering. Keep both translation-on and translation-off ratios as research measurements, not a fixed overhead allowance (`step11/killtest.txt:7`, `:11`, `step11/hostgl-back-c.txt:3`).

* 80-4 needs exportable resources wherever GL and Venus share buffers; a working Venus-to-GL compositor does not prove the reverse direction. 80-5 must compare serial allocation/release and mapping reuse separately, and name the host configuration: its 64 MiB allocation plus first touch changes from about 13 ms to 31 ms (`step10/web/vulkan-fatal-info.txt:7`, `step10/blob-host-2.txt:10`, `step9b/blob-host-off.txt:11`).

* In the order at `plan.md:313` and `:314`, remove the completed Linux reference installations and assignment experiment. Their results now inform VMX, VFIO and transport design immediately. Keep watchdog qualification before relying on it. Do not mark 78-7's exit-cost measurements or 80-6's native-context check complete: neither was supplied by these rows.

* Risks: clear risk 1's basic Linux speed concern, risk 3's tested assignment obstacle and risk 5's forward mapping obstacle. Extend risk 7's return method to Debian. Keep risks 4, 6, 8 and 10 open. Add explicit stack-compatibility, low-address Vulkan mapping and GL-to-Venus sharing risks; the raw failures above justify them (`plan.md:334`, `:336`, `:338`, `:340`; `step10/probes-detail.txt:55`, `step10/web/vulkan-fatal-info.txt:7`).

4. Faults encountered.

* IGT loading physical sound and vgem is a run-method fault. The recorded `kms_prime@basic-crc-hybrid` entry contains both module loads (`step9/igt/results.json.gz`). A modprobe blacklist was insufficient. Prevent explicit physical-sound loading in this fixture, and verify the chosen render device after IGT. The discarded llvmpipe reruns are invalid; the later Crocus reruns remain usable (`step9.md:214`).

* KVM's `Bad address` is a Linux reference-stack fault, not evidence of a Rach2 defect. The huge-page trigger and working configurations are established. Use the tested workaround for reference runs while tracing the exact failure; retain the case in 78-1's proof (`step9b/kvm/kvm-probe.txt:36`, `kvm-always.txt:5`). It does not invalidate successful Venus buffer checks or VFIO assignment.

* IGT's 120-second and two-hour limits are run-method faults against the count-based rule (`step9/igt/metadata.txt:14`). No timeout was reported, so completed results stand. WebGL's server-line monitor also confuses a long page with stalled work and can be reset by an unrelated request (`step9/webgl.sh:26`, `step9.md:161`). Repair the observation method before reuse.

* Missing `spirv-as`, the wrong default EGL platform and vgem's renderer selection were fixture faults. Retain the corrected per-case results and their prerequisites, rather than presenting the adjusted piglit total as one clean rerun (`step9.md:197`, `step9/piglit/rerun/run.log`, `step9/piglit/rerun-egl/run.log`).

* Disabling Crysis sound was a method fault for gameplay acceptance. The dialog traces and silent-device comparison support the HUD diagnosis; they do not require a speculative graphics-driver fix (`step9/crysis-dialog-game.log:130`, `crysis-dialog-sound-game.log:119`). The silent timing benchmarks remain measurements of that configuration.

* Chromium's sandbox failure, video fallback, WineD3D's Vulkan crash and the GL-to-Venus import failure occurred on Linux. They establish reference-stack problems, not Rach2 kernel causes. Work required by the promised desktop belongs in 73/74/75/77; matching the failure must not silently waive a required working feature (`step9/browsers/sandbox-early.log:7`, `step9/crysis-vk-1366-low-ffp/Game.log:86`, `step10/web/vulkan-fatal-info.txt:7`).

* The initial refusal to write p5 is no Rach2 fault: the documented ordinary-kernel policy returns EPERM for partition writes (`man/man7/disk.7:116`). The missing game was a stale preparation assumption under the documented `/opt` update behavior, not demonstrated filesystem corruption (`lenovo-setup/p3-ro-check.txt:26`, `build/cache/crysis/README:15`).

* The boot partition restoration is supported by two matching hashes (`step11/p1-restore.out:2`, `step11/p1-readback-mac.txt:1`). Watchdog expiry remains untested. The ext2, Windows admission, stale testing-PC monitor and WSL module incidents belong to the other assigned review part (`codex-qb.md:3`).

5. Measurements next, in priority order.

* Qualify the product graphics stack on the Lenovo and the selected testing-PC host: repeat step 10's Direct3D probes in both bitnesses, then its browser probe, using a candidate Mesa/virglrenderer combination. Require actual selected adapters and pixel readback. Do this before presenting a replacement pin as working; the existing failures prevent that claim (`step10/probes-detail.txt:31`, `:55`, `step10/web-a.txt:15`).

* On the Lenovo, trace Chromium's sandbox startup and denied EGL load; record Firefox's sandbox diagnostics too. Then repeat the required accelerated browser cases. The plan can go to the owner without the measurement only with this retained as unresolved work, not with the proposed unsandboxed acceptance (`step9/browsers/sandbox-early.log:7`, `plan.md:186`).

* On the Lenovo, complete 77-2's remainder: umu, the chosen fullscreen route, a silent working audio sink, full first-level gameplay and native-game replays. The plan can go to the owner beforehand, but must distinguish the proven Crysis speed margin from acceptance still owed (`plan.md:254`, `:261`, `step9/crysis-dialog-sound-game.log:115`).

* Repeat only affected reference cases with the corrected fixture: WebGL timeout cases without concurrent builds, renderer-sensitive checks after IGT, and clean performance repetitions with matched cache preparation. A changed Mesa pin requires new matching references. This need not delay plan review, but it must precede freezing the comparison results (`step9/webgl/chromium-summary.txt:3`, `step9.md:160`, `:214`).

* On the Lenovo, perform 75-1's controlled watchdog expiry and automatic return before relying on it during a forced host hang. Plan review need not wait; hardware experiments that depend on this protection must (`plan.md:212`, `step11/wdog-arm.txt:18`).

* On Linux on the Lenovo, repeat the failing WineD3D probe with tracing at the PFN lookup/reference path, recording the failing mapping, page type and reference count. Compare `never`, `deny` and the demonstrated `always` trigger. Plan review need not wait because a working reference configuration exists; the exact-cause claim must wait (`step10/linux-src/virt/kvm/kvm_main.c:2886`, `step9b/kvm/kvm-always.txt:14`).

* Keep reversed-buffer revocation, death with live mappings and the matched Haswell native-context experiment in 80-5/80-6. Forward Venus and successful assignment do not answer them. These are explicit research tasks and need not precede approval of that research (`plan.md:305`, `:306`).