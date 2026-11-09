Paths without a directory are under `build/flow/coordinator/graphics-research/run2/`. `plan.md` means `build/flow/coordinator/graphics-research/plan.md`; `report.md` means `dev-log/76-graphics/research/run2/report.md`. Task numbers remain draft 5's.

1. Step 8 ext2.

* Not yet known whether Rach2 violated its contract. The nine inode removals match the bitmap removals and fit documented unclean-stop recovery. They do not establish a data-safety fault. The mount mode and original inode owners remain unknown. (`step8/out/e2fsck-after-stop.log:3-28`; `rach2/sys/fs/ext2.Readme.md:68,103-111`; `report.md:135`)
* One forced test: matched Linux/Rach2 ext2 copies, recorded mount modes, synced sentinel files, known open-but-unlinked files with recorded inode numbers, then a forced QEMU stop. Repeat under the paired `wineboot` load; compare recovery and sentinel contents with an orderly-shutdown control. Preserve images before repair. This tests the explanation, not the identity of the old nine files. (`rach2/sys/fs/ext2.Readme.md:34,99,103-113`; `report.md:135,167`)
* Draft 6 may go first with the investigation required before release. Do not adopt "anything else is a fault": the documented recovery states include more than deleted inodes. A demonstrated data-safety fault holds release. (`report.md:167`; `rach2/sys/fs/ext2.Readme.md:68`; `AGENTS.md:24`)

2. Chromium's GPU sandbox.

* Keep 73-17's functional checks with working rendering and verified sandboxes, including the GPU process; keep that requirement in 75-12. Add an explicit Chromium/Mesa qualification-and-fix prerequisite. I agree with Fable's fix-task alternative. Linux's observed failure prevents blaming Rach2; it does not establish impossibility or satisfy the existing Done. (`plan.md:186,223`; `step9/browsers/chromium-default-gpu.txt:164`; `step9/browsers/sandbox-early.log:7`; `AGENTS.md:24`)
* Trace startup, EGL broker permissions and the trapped scheduling calls. Draft 6 may precede that trace if it names the unresolved work and does not promise that a switch fixes it. Firefox's sandbox also remains unverified. (`step2.md:68-70`; `step9.md:155-156`)

3. `LP_NUM_THREADS`.

* Use 0 for the measured dEQP-VK, piglit and WebGL conformance profiles, including their concurrent runs. Use 8 for the measured eight-vCPU single-program software-rendering profile. The first glmark2 pair scored 402/1223; WebGL's shader loop took 436.7/59.2 ms. Match settings between Rach2 and Linux. (`step3-4.md:170-184`; `step5-7/out/p1-lp0/glm-off-1.txt:45`; `step5-7/out/p1-lp8/glm-off-1.txt:45`; `step5-7/out/p1-lp0/p1-lp0-webgl-1.json:1`; `step5-7/out/p1-lp8/p1-lp8-webgl-1.json:1`)
* Reject the universal setting. vkd3d-proton was not measured at zero; retain its measured configuration until qualified. These results do not choose thread counts for hardware renderers or other workloads. (`step3-4.md:201`)

4. Venus's Mesa.

* Both: one coordinated qualification task serving 73-13, 74-4, 74-6 and 77-1, with the proposed Mesa 26.1.6 measurement first. I agree with Fable that it is useful now and need not block draft 6. The candidate pin stays provisional. (`fable-review.md:89`; `plan.md:182,200,202,253`)
* Repeat step 6's extension checks and DXVK, vkd3d-proton and 32-bit WoW64 probes, requiring the intended adapter and correct pixels. Missing extensions, low-address mappings and hasvk's own refusals are separate problems. "About an hour" is Fable's estimate. (`step6.md:104-112`; `step10.md:86-98`; `fable-review.md:89`)

5. Native Windows QEMU.

* WSL remains the supported choice, but "worse on every measure" is false. Windows ANGLE/GL's first saved run took 17.2 ms for 20,000 WebGL draws against WSL's 30.1 ms; its shader loop was slower, 12.6 against 3.8 ms. State the measured advantages and exceptions. (`step5-7/out/p3b-anglegl/p3b-anglegl-webgl-1.json:1`; `step5-7/out/p2-lp0/p2-lp0-webgl-1.json:1`)
* Venus and blobs were disabled by default and never enabled by the launch. Inspect that build's virglrenderer support, then try `venus=on,blob=on,hostmem=1G`, retaining startup errors and guest capsets. This is required before declaring the capabilities absent. Draft 6 may go first with them marked untested. (`step5-7/out/win-gl0.txt:22,30,52`; `step5-7/scripts/win-path.sh:19`)

6. 77-2 and Crysis.

* Preserve the completed session; do not repeat installation or the whole reference run. Keep a smaller 77-2 for missing Linux references: direct/umu comparison on the chosen fullscreen and working silent-audio route, the full first-level criterion, and native-game replays. The flyby and successful sound event do not complete those checks. (`step9.md:28-31`; `step9/crysis-dialog-sound-game.log:115`; `plan.md:129,254,259,261`)
* I narrow my earlier remainder: Rach2 fullscreen/audio integration belongs in 77-4, umu integration in 77-8, gameplay and saves in 77-9, browser work in 73-17/75-12, and Venus qualification in 74-6. HDMI and owner-operated acceptance stay at the final sitting. Fable is right to remove completed work from the queue, but not its missing references. (`plan.md:186,202,223,256,260-261`; `AGENTS.md:88`)

7. Fullscreen.

* Keep Weston with kiosk-shell. Make 77-4 prove uncropped fullscreen, scaling, focus and mouse capture. The tested Weston/Wine arrangement failed and cage worked; that does not prove cage is necessary. (`plan.md:124,256`; `step9.md:44-45`)
* Name cage as the measured fallback launcher arrangement, with return to Weston after exit, still to be qualified as that arrangement. Do not silently replace the owner's desktop choice. (`step9/play-cage.sh:22-25`; `plan.md:41,124`; `AGENTS.md:33`)

8. VT-d's cost.

* Use translation-off Linux for 77-2's native-driver gate, recording allocation policy and cache preparation. For 79's Done, record the assigned-GPU ratio against translation-on native Linux under matched settings: the saved glmark2 comparison is 1185/1199, or 98.8%. (`step9b.md:17,26,42`; `step11/glmark2/compare-gl-step9.txt:34`; `plan.md:293`)
* Reject "15 percent from VT-d alone" and any fixed allowance. Translation, huge pages and stolen-memory use changed together. I agree with Fable on the reference choice, not the attribution. The native gate remains no unexplained loss above 10%; driver-VM overhead remains a research result. (`step9b.md:17,42`; `step11.md:105`; `plan.md:140`)

9. 80-5's Linux blob baseline.

* Record separate workloads and completion boundaries. Serial 64 MiB allocation/map-and-touch/unmap-and-free measured 0.039/58.092/8.873 ms. Holding 32 mappings gave mean map-and-touch 5.047 ms; 32 frees returned in 15.888 ms; the following map took 116.761 ms. These do not support a universal per-operation charge. (`step10/blob-guest-2.txt:9-11`)
* Include reuse, coherence and steady-state bandwidth: CPU write/read/copy 5781/9328/8209 MB/s, GPU copy 7601 MB/s. Record QEMU and host configuration, including direct-host comparisons. Asynchronous unmap and RCU batching make "50 ms per unmap" an unsuitable general model. (`step10/blob-guest-2.txt:14-23`; `step10/blob-host-2.txt:10`; `step9b/blob-host-off.txt:11`; `step10/qemu-src/hw/display/virtio-gpu-virgl.c:180-209`; `step10/qemu-src/util/rcu.c:265-270`)

10. KVM's `Bad address`.

* The huge-page setting is an isolated trigger: with translation off, `never` drew correctly; `always` produced huge pages and stopped QEMU with `Bad address`. The failing PFN and reference-count mechanism remain untraced. The source comment concerns non-compound higher-order allocations, not proof about the observed pages. (`step9b/kvm/kvm-probe.txt:3,29-36`; `step9b/kvm/kvm-always.txt:2-6,13`; `step10/linux-src/virt/kvm/kvm_main.c:2944-2951`)
* I agree with Fable on the trigger, not the mechanism. Put the reproducer and precise fault trace in 78-1. Require correct GPU-backed slot handling, references and invalidation; unusable mappings must fail without stopping a running VM. Keep the working configuration for reference runs. (`plan.md:271`; `fable-review.md:62`; `step9b/kvm/kvm-probe.txt:36`)

11. Rows 1 to 8 and their tasks.

* Row 1: agree on WSL/KVM for reference guests. Reject the session-0 exclusion of Windows graphics. 73-16 must distinguish the Windows builds and profiles. (`fable-review.md:17`; `step5-7/out/p3a-d3d11/info.txt:22`; `report.md:152`)
* Row 2: agree on early Direct3D qualification on guest lavapipe. The saved 32/64-bit Direct3D 12 probes pass. Sandbox and Venus qualification remain; discovering the GE-Proton version does not finish package integration. (`step2/out/proton/d3d12-32-vkd3dp.out:6`; `step2/out/proton/d3d12-64-vkd3dp.out:6`; `plan.md:253`)
* Row 3: agree on measured corpus sizes, with exact case lists, exclusions and named Linux failures. Do not turn the discovery harness's silence timeout into a release verdict or count interrupted units as complete conformance. (`step3-4.md:191-195`; `step3-4/scripts/guest/deqp-run.py:61-65`; `AGENTS.md:71`)
* Row 4: partly settled. The 32-CPU admission limit is supported for the measured CPU-bound mixture; charging light guests two CPUs remains an estimate. 73-16 uses point 3's profile-specific thread counts. (`step3-4.md:172-185,201-204`)
* Row 5: retain 74-1's profile and account-access work. Retain 74-2/74-5 for acceleration, with same-QEMU Linux comparisons. Pipeline where semantics permit; preserve required fence waits. The Windows capability question remains open. (`plan.md:197-201`; `step5-7.md:114-117`)
* Row 6: agree on the fourth host option, not completion of decision A's application requirements. 73-16 keeps separate RTX-virgl and CPU-GL Venus profiles and version-specific module validation. 73-3 starts with udmabuf and sync-file export; reverse mappings remain unproven. (`step6.md:104-112,159-164`; `step6/out/load.txt:14`; `step8.md:150`; `plan.md:305`)
* Row 7: agree on TCG for the measured seven x86-64 suites. 78-6 takes the PCI improvement and investigates SB16/bridge-scan costs, with hardware qualification remaining. 78-7 counts exit kinds and separates nested-PC results from Lenovo results. Investigate the reported Windows admission fault; its workaround is not a repair. (`step5-7.md:127-128,169,173-186,205-207`; `plan.md:277`)
* Row 8: agree that sizing remains partial. 73-3/73-9 gain the measured export/discovery work and 77-4's input dependency. Reject "77-3's 64-bit half is done": Debian Wine 10 was tested, while that task requires pinned GE-Proton on virgl and its memory behavior. Retain the discovered gaps and unmeasured DRM/input/i386 work. (`step8.md:150-171`; `plan.md:255`)
* Evidence limits: Fable's step 1 exit-cost figures and step 6 Venus frame rates lack retained raw output. Use step 7's saved exit measurements; rerun Venus timing before using it in a gate. The Windows admission error remains the step agent's account. (`report.md:153-157`)

12. Next measurements, in priority order.

* Testing PC: point 1's matched ext2 crash/recovery test. Draft 6 can go without it; resolve before release. (`rach2/sys/fs/ext2.Readme.md:68,103-113`; `AGENTS.md:24`)
* WSL, then Lenovo: repeat steps 6/10 with candidate Mesa, both Direct3D bitnesses, adapter/pixel checks and Chromium's Venus selection. Draft 6 can go without it; the replacement pin remains unqualified. (`step6.md:104-112`; `step10/web-a.txt:15,21`)
* Testing PC and Lenovo: trace Chromium startup, broker opens and scheduling traps; collect Firefox sandbox diagnostics. Draft 6 can go without it; browser acceptance cannot. (`step2.md:69-70`; `step9.md:155-156`)
* Windows testing PC: inspect virglrenderer support and explicitly enable Venus/blob/hostmem in step 5's launch. Draft 6 can go without it; an absence claim cannot. (`step5-7/scripts/win-path.sh:19`; `step5-7/out/win-gl0.txt:22,30,52`)
* Lenovo: complete 77-2's missing game references, direct/umu comparison and native replays on the chosen launcher and silent sink. Draft 6 can go without them; owner-operated gameplay stays at the final sitting. (`plan.md:254,259-261`; `AGENTS.md:88`)
* Lenovo: repeat step 10's failing WineD3D probe with PFN/reference tracing under `never`, `deny` and `always`. Draft 6 can go without it; the mechanism remains unknown. (`step9b/kvm/kvm-always.txt:13`; `step10/linux-src/virt/kvm/kvm_main.c:2950-2951`)
* Testing PC and Lenovo: retain Venus timing output; repeat only affected references with fixed corpus pins, caches, renderer identity and concurrent work controlled. Draft 6 can go without these; gate references cannot be frozen first. (`report.md:157`; `step9/webgl/chromium-summary.txt:3`; `step9b.md:26`)
* Lenovo: 75-1's controlled watchdog expiry and automatic return. Draft 6 can go without it; an experiment relying on that recovery path must wait. (`plan.md:212`; `step11/wdog-arm.txt:18-22`; `step11.md:98`)