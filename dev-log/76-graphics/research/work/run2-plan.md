# The second discovery run: design

Written 2026-11-06 by the chat, for the owner's question of what run will steer the graphics plan. Approved by the owner on 2026-11-06; it starts when the Wi-Fi work is finished.

The owner's decisions it rests on: no WINQ-EMU, no gfxstream; the reference lavapipe is the agents' target on the testing PC, in WSL or native Windows by velocity (WSL expected); graphics runs under KVM or WHPX, never TCG; Crysis work only on the Lenovo at the very end; decisions rest on a comparison with Linux in the same environment.

Rule: a Debian 13 guest with the plan's Mesa 25.0.7 runs everything first. Each result is saved as the reference Rach2 must later match. The Mac has only TCG, so it runs no graphics suites.

| # | Measure | Where | Steers |
|---|---|---|---|
| 1 | The same lavapipe work and Rach2 boot, KVM against WHPX | WSL; `D:/rach2/build/qemu` | Agents' environment; 73-16's profiles. WSL unless Windows is about twice as fast |
| 2 | What runs on guest lavapipe: Vulkan, GL, WebGL, Weston with both browsers (sandbox on), Wine's Direct3D 9 to 12 with the pinned GE-Proton's DXVK and vkd3d-proton (never tried), WebGPU on SwiftShader | Winner of 1 | Whether 74-6 moves onto guest lavapipe in 73; 73-13's Mesa pin; 77-1's pin. DXVK 3.x may ask Vulkan 1.4 (Codex) |
| 3 | Wall time of the WebGL suite, piglit, dEQP-VK, Wine's Direct3D tests and vkd3d-proton's suite at 2, 4, 8 threads | Same | Which suites stay in each Done, and the subset sizes |
| 4 | 2, then 5 guests beside a suite load, threads capped | Same | Admission limit, LP_NUM_THREADS |
| 5 | Same light programs: guest llvmpipe, virgl on the RTX in WSL, stock QEMU 11.1.0 over ANGLE (headless, session 0) | WSL; Windows | Whether 74-1, 74-2, 74-5 stay a must; closes the owner's native QEMU question |
| 6 | Venus on host lavapipe with the udmabuf module, KVM at level 2, then a picture | WSL | Decision A; agents' way to test Rach2's virtio-gpu 3D; deliverable 2's transport |
| 7 | Rach2's suites under KVM and WHPX against TCG, exits counted, one PCI scan at boot | WSL; Windows | Whether kernel work may use the fast paths; the owner's TCG default |
| 8 | Today's Rach2 on the GNU root with Debian's lavapipe, Weston headless and Wine, `rach2.trace=enosys` | Rach2 in QEMU | The real size of 73, 76 and 77 in place of line estimates |
| 9 | 77-2: Crysis's fixed route under GE-Proton on WineD3D's GL and Vulkan renderers, browsers, decoders, glmark2, piglit, dEQP-GLES, IGT, WebGL | Lenovo, Debian | Risk 1; decisions B, C; the reference numbers for every gate |
| 10 | Venus through QEMU on hasvk with a Linux guest, Chromium's WebGPU on it | Lenovo, Debian | Risk 5; the one real-GPU check of the virtual GPU left |
| 11 | 79-1: the HD 4400 assigned to a VM through VT-d, chipset watchdog armed | Lenovo, Debian, last | Risk 3, risk 6; go or no-go for deliverable 2 before any hypervisor code |

Left out: WINQ-EMU, gfxstream, GPU assignment on the testing PC, a forced GPU hang, Crysis on the testing PC, hypervisor tests at level 3, Rach2's own graphics drivers.

Needs: the udmabuf module in WSL with no WSL job running; Debian on the Lenovo's trial partition after the Wi-Fi run; step 11 takes the screen, with notice, and a hang the watchdog misses needs the power button.

Out: a report with Linux's result beside each, then draft 6 of the plan, reviewed by Codex and Fable.
