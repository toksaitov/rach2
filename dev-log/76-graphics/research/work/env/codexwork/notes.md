# Research notes

Task: read ../brief.md and answer its seven questions in report.md. Only notes.md and report.md may be written, both in this directory. No SSH, no VM, no subagents, no repository edits. Use current web sources, label measured/documented/inferred claims, and end with report path. Reread brief and this file after compaction.

Read root Readme.md and AGENTS.md, tonight/report.md, and part of combined tonight reports. Need targeted reread qemu-wsl.md and lenovo.md because first combined tool output was truncated. Local source trees: build/research/src/{linux,mesa,libdrm,proton,...}; no QEMU or virglrenderer there.

Initial finding: Microsoft Learn troubleshooting explicitly excludes client Windows 10/11 Pro and desktop hardware from supported DDA/GPU-P deployment; DDA requires Server 2016+, supported GPU-P Server 2025+. This is support evidence, not observed rejection on build 26300.9818. Need distinguish enumerated GPU partitions from usable Linux Vulkan and from SKU enforcement. URL: https://learn.microsoft.com/en-us/troubleshoot/windows-server/virtualization/troubleshoot-hyper-v-gpu-assignment-partitioning-passthrough-issues

Likely ranking to test: WSL KVM + virgl/d3d12 for hardware GL; WSL KVM + Venus/lavapipe after udmabuf for Vulkan transport; native Windows WHPX + virgl/ANGLE must be measured and gfxstream investigated for actual Vulkan. Avoid Debian-inside-WSL layer. Native guest lavapipe useful control but no virtio transport coverage.

No remote actions or experiment runs performed. Existing measurements only, with new read-only local/source/web research.

Major current discovery: cmspam/winq-emu alpha10 provides Windows Venus port, QEMU v11.0.0 + virglrenderer 1.3.0. Read raw READMEs in winq-emu, winq-emu-qemu, winq-emu-virglrenderer (all branch alpha10), release alpha10 (launcher tag commit 70d5175), installer/launch-vm.bat. Native command uses WHPX cpu host (fork only), virtio-vga-gl,blob=on,hostmem=4G,venus=on, SDL gl=on, BIOS. README claims Zink Wayland, native GPU Vulkan; not measured here, no conformance evidence. Code read: src/venus/vkr_physical_device.c remaps opaque fd to Win32, synthesizes dma-buf/modifiers with LINEAR only, translates sync fd properties. README admits deferred Vulkan object destruction until device teardown, in-process threads replacing renderer processes, and broken Chromium zero-copy video. Open issue /issues/2 reports Ubuntu 26.04 RTX3070Ti Vulkan freeze, /issues/1 q35 boot blank, /issues/3 Hyper-V boot problems. Therefore high-value experiment, not proven default. Avoid installer/global writes/diagnostic flag writing AppData; private build/runtime under build. Need integrate Rach2 WHPX XSAVE fix (tools/setup/qemu-whpx, dev-log/62-etude/qemu-whpx-report.md); installed 11.1.0 vendor code identity uncertain. No binary patch from other project.

Current DXVK upstream Driver-support wiki now says DXVK 3.x Vulkan1.4; brief's 1.3 statement applies pinned 2.x. vkd3d-proton README current master requires 1.3, million update-after-bind descriptors for all except uniform buffers, robustness2, push_descriptor, samplerMirrorClampToEdge/shaderDrawParameters. No vkd3d-proton measured locally. Do not equate version with success. https://github.com/doitsujin/dxvk/wiki/Driver-support ; https://raw.githubusercontent.com/HansKristian-Work/vkd3d-proton/master/README.md

Mesa local VERSION=25.0.7. dzn_device.c API1.2, no robustness2, Linux external_memory_fd/external_semaphore_fd exist but opaque handles (not dma-buf/sync_file). Current raw GitLab source inaccessible; cannot claim newer 26.x same merely from old tree. Need private current dzn probe as conditional experiment.

Hyper-V Linux consumer GPU-PV actually has contemporary primary projects: Ripthulhu/easy-gpu-pv-linux main README tests Windows11 26200 RTX3080Ti driver32.0.16.1088; dxgkrnl+WSL libs+UMD, OpenGL4.6 Vulkan1.2 via Dozen; not NVIDIA native Linux Vulkan. Speedstu/mirage-gpupv main tests AMD RX9060XT Ubuntu24.04 Mesa25.2.8, GL GLES3.1 readback, no validation on Raphael iGPU. These keep Windows GPU. Official supported Server GPU-P list excludes consumer GPUs; distinguish experimental GPU-PV.

DDA refusal evidence: https://learn.microsoft.com/en-us/answers/questions/4112018/is-it-possible-to-use-direct-device-assignment-on is firsthand Windows11 report: dismount/add accepted, VM start fails virtual PCIe port 0xC035001e 'A hypervisor feature is not available to the user'. Not proof exact machine/build. MS official troubleshooting support docs + deploy docs requires Server2016+. IovSupport is platform capability not license. No destructive test proposed before resolving SKU.

Source docs: https://docs.mesa3d.org/drivers/venus.html now lists NVIDIA proprietary570.86+ (Linux), RADV etc, Linux host requirement Vulkan1.1+external_memory_fd plus mapped-memory caveat. QEMU https://www.qemu.org/docs/master/system/devices/virtio/virtio-gpu.html now documents drm_native_context property (installed10.2 does not), rutabaga gfxstream-vulkan. Hence do not say all stock QEMU has no native context, only measured installed version. crosvm Windows main docs build cargo --features all-msvc64,whpx; WHPX upstream untested per crosvm.dev/book/hypervisors.html. gfxstream github/google/gfxstream main README supports Windows backend CMake/ClangCL; existence of gfxstream ICD in WSL is guest frontend, not RTX host driver.

Read complete required tonight reports through targeted reads, plan relevant milestones73-80. Plan common work approx11k lines desktop73 plus1.5k virtio graphics74; environment estimates incremental. Tonight measured lvp guest1.4, Venus1.2 missing udmabuf, after Debian udmabuf Venus1.3.269 DXVK2.6 works offscreen; no visible presentation and llvmpipe display SIGSEGV. Vulkan swapchain availability != screen proof. Draw timing cannot alone prove d3d12 causation. CPU-device Dawn fallback can perhaps be selected explicitly; no fundamental WebGPU-on-CPU prohibition.

Need finish report: comparison table all environments, deep sharing/assignment/software/port costs, ranked2-3, exact priority experiment recipes native installed and fork, WSL udmabuf module vs custom kernel vs sibling Hyper-V Linux software host (eliminates L3 without WSL kernel change), evidence links and limitations. No tests or remote tools. Only notes/report writes.

CHECKPOINT BEFORE RESET: report.md now has full draft (~large, 3 patches): overview,18-row comparison, build/change/control table, HyperV/DDA/2GPUs, sharing/lavapipe, alternative ports/hardware/rental, recommendation top3, seven priority experiment sections with PowerShell/QEMU/guest commands and limitations. Must reread brief+notes then finish source citations and audit correctness. No source-reference definitions yet! Text currently has labels like [S/I; M1, W1-W4] which must convert to `(S/I; [M1], [W1],...)` and add valid markdown link reference definitions. Do not leave unresolved. Only write notes.md/report.md. User final must end with absolute path report.md. No tests, no SSH, no VM, no subagents. Window01a1acba-a01e-76a2-b675-e6c0013ec9be; user asks read ../brief.md whole task, maintain notes+report and write nothing else; history ID query failed.

SOURCE MAP to define (all read in prior tools except L7 need browse exact docs):
M1 ../brief.md absolute /Users/toksaitov/Desktop/rach2/build/flow/coordinator/graphics-research/env/brief.md
M2 /Users/toksaitov/Desktop/rach2/build/flow/coordinator/graphics-research/tonight/qemu-wsl.md (raw outputs linked within)
M3 /Users/toksaitov/Desktop/rach2/dev-log/62-etude/qemu-whpx-report.md (+ tools/setup/qemu-whpx/Readme.md)
P1 /Users/toksaitov/Desktop/rach2/build/flow/coordinator/graphics-research/plan.md
T1 /Users/toksaitov/Desktop/rach2/tools/etude/Readme.md (resource admission and Win JobObject)
Q1 https://www.qemu.org/docs/master/system/devices/virtio/virtio-gpu.html (current master docs; installed10.2 lacks native context but master has it)
Q2 https://www.qemu.org/docs/master/system/invocation.html
W1 https://raw.githubusercontent.com/cmspam/winq-emu-qemu/alpha10/README.md
W2 https://raw.githubusercontent.com/cmspam/winq-emu-virglrenderer/alpha10/README.md
W3 https://raw.githubusercontent.com/cmspam/winq-emu-virglrenderer/alpha10/src/venus/vkr_physical_device.c (read actual WIN32 fd remapping & linear DRM modifier code)
W4 https://github.com/cmspam/winq-emu/issues/2 (Vulkan freeze Ubuntu26.04 RTX3070Ti)
W5 https://github.com/Tsuki-Bakery/qemu-virgl-whpx (ANGLE Windows virgl example; not proof installed build)
W6 https://chromium.googlesource.com/angle/angle/+/HEAD/src/libANGLE/Display.cpp (ANGLE_DEFAULT_PLATFORM)
W7 https://github.com/cmspam/winq-emu/issues/1 (q35 boot blank; owner can't reproduce, reporter patched)
W8 https://raw.githubusercontent.com/cmspam/winq-emu/alpha10/installer/launch-vm.bat
extra Windows sources https://raw.githubusercontent.com/cmspam/winq-emu/alpha10/README.md (BIOS advised, zero-copy video broken); release https://github.com/cmspam/winq-emu/releases/tag/alpha10
G1 https://github.com/google/gfxstream README (Windows CMake ClangCL, API transport)
G2 https://crosvm.dev/book/hypervisors.html (WHPX not upstream tested, HAXM Intel)
G3 https://raw.githubusercontent.com/google/crosvm/main/docs/book/src/building_crosvm/windows.md (cargo featuresall-msvc64,whpx)
G4 https://raw.githubusercontent.com/google/gfxstream/main/host/vulkan/vk_decoder_global_state.cpp (lowercase file! read Win32 DuplicateHandle implementation). Android current docs if needed https://developer.android.com/studio/releases/emulator
D1 https://docs.mesa3d.org/drivers/d3d12.html adapter name selector
D2 /Users/toksaitov/Desktop/rach2/build/research/src/mesa/src/microsoft/vulkan/dzn_device.c local VERSION25.0.7. Need verify build option microsoft-experimental in local meson_options.
V1 https://docs.mesa3d.org/drivers/venus.html (current supports Linux NVIDIA570.86+, memory warning)
L1 local Mesa25.0.7 src/virtio/vulkan/vn_physical_device.c (api clamp lines402+ and WSI955), and gallium/frontends/lavapipe/lvp_device.c plus llvmpipe/lp_screen.c. Could add multiple local reference labels for precise claims.
L2 https://docs.mesa3d.org/drivers/llvmpipe.html
L3 https://raw.githubusercontent.com/torvalds/linux/v6.18/drivers/dma-buf/Kconfig
L4 https://docs.mesa3d.org/envvars.html#lp-num-threads
L5 https://raw.githubusercontent.com/torvalds/linux/v6.18/drivers/dma-buf/udmabuf.c
L6 localMesa src/virtio/vulkan/vn_renderer_vtest.c recvmsgSCM_RIGHTS lines120+
L7 NEED verify docs.kernel.org/kbuild/modules.html for modules_prepare no Module.symvers
H1 MS troubleshooting URL already above; H2 https://learn.microsoft.com/en-us/windows-server/virtualization/hyper-v/nested-virtualization
H3 Ripthulhu README above; H4 Speedstu README above
H5 https://docs.nvidia.com/vgpu/latest/grid-vgpu-user-guide/using-gpu-pass-through.html (but GPU-P matrix use https://docs.nvidia.com/knowledge-base/latest/overview.html also read)
H6 https://learn.microsoft.com/en-us/windows-server/virtualization/hyper-v/deploy/deploying-graphics-devices-using-dda
H7 https://raw.githubusercontent.com/torvalds/linux/v6.18/drivers/gpu/drm/hyperv/hyperv_drm_drv.c (modeset/GEM/atomic no render)
H8 https://learn.microsoft.com/en-us/answers/questions/4112018/is-it-possible-to-use-direct-device-assignment-on (firsthand refusal, not confirmed machine)
H9 https://learn.microsoft.com/en-us/windows-server/virtualization/hyper-v/gpu-partitioning
H10 https://github.com/MicrosoftDocs/Virtualization-Documentation/blob/main/hyperv-tools/DiscreteDeviceAssignment/SurveyDDA.ps1
H11 https://docs.nvidia.com/deploy/mps/latest/index.html
H12 https://learn.microsoft.com/en-us/windows/wsl/wsl-config
H13 https://learn.microsoft.com/en-us/windows/wsl/filesystems
O1 https://www.vmware.com/products/desktop-hypervisor/workstation-and-fusion ; O2 https://docs.mesa3d.org/drivers/svga3d.html ; O3 https://docs.oracle.com/en/virtualization/virtualbox/7.2/user/guestadditions.html ; O4 https://docs.oracle.com/en/virtualization/virtualbox/7.2/user/AdvancedTopics.html
B1 AMD7950X3D product URL alreadyabove; B2 https://www.hetzner.com/dedicated-rootserver/matrix-gpu/ (now GEX45/GEX63etc, pricing not rendered); B3 https://www.hetzner.com/dedicated-rootserver/gex44/configurator/ (unavailable)
A1 DXVKwikiabove A2 vkd3dREADMEabove A3 https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md A4 https://dawn.googlesource.com/dawn.git/+/d758dd0ff51519e8c5acb047a6bab0adacd02ed2/docs/dawn/features/adapter_options.md (search retrieved fallback/UUID docs).


FINAL CHECKPOINT: report.md is complete. It answers all seven brief questions, compares 18 environments plus incremental work/control, ranks WSL virgl now, native Windows WHPX/Windows Venus next, and host lavapipe/Venus as correctness lane. Native Windows alpha10 remains unmeasured. Detailed ordered recipes include native installed baseline, private Venus port, exact-source udmabuf kernel/module feasibility, peer Hyper-V software host, application and concurrency checks, and lower-priority alternatives. No remote command or experiment ran. Only notes.md and report.md exist in output directory. No repository edit, subagent, SSH, VM, or machine change.

Final source audit corrected vkd3d-proton test binary to d3d12.exe (tests/meson.build), added explicit native DLL selection and disposable WINEPREFIX, and corrected Dawn fallback guidance: current BackendVk.cpp forceFallbackAdapter recognizes SwiftShader specifically. Chromium lkgr CanUseAdapter requires external image support for other adapters. Native Dawn explicit Vulkan UUID selection with forceFallbackAdapter=false is diagnostic, not browser acceptance. Added explicit research-bed/nested-SVM boundary: WHPX graphics success does not qualify Rach2 SVM; WSL still puts Rach2's inner driver VM at level3.

All report source markers now use descriptive Markdown links with reference definitions, 61 sources. Local source paths exist; no unresolved references, all ASCII, seven balanced code blocks. Linux module docs verified modules_prepare does not build Module.symvers. Source catalog names mutable main/HEAD versions and separate pins; latest Dozen properties remain unverified. Report approx73KB; final answer should be concise and end with absolute report.md link/path. Current window01a1acd4-b84a-76b3-991d-8aca7933ec24.
