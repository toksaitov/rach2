# Step 9: the Lenovo's graphics and games reference under Debian

Row 9 of `run2-plan.md`, task 77-2: Debian on the Lenovo, set up by the agent before you (`run2/lenovo-setup.md`: the address, the users, how to leave Linux and restore p1). It answers risk 1 and decisions B and C, and gives the reference number for every gate of the plan.

* Debian runs with the IOMMU's DMA translation on, its default (i915 says "DMAR active, disabling use of stolen memory"). Take the reference this way and say so beside each number; a later boot with `intel_iommu=off` retakes a subset to measure what translation costs.
* Crysis's fixed route under GE-Proton, outside Steam, WoW64 mode, with WineD3D on GL (`PROTON_USE_WINED3D=1`) and with WineD3D on Vulkan: the game is in Debian's root at `/srv/crysis/game` (checked against its manifest), used as the lower layer of an overlay, since the game writes into its own tree; the 64-bit DX9 route, `Bin64/Crysis.exe -dx9 -DEVMODE +map island +exec benchmark_gpu`, its log `Game/Levels/island/benchmark_gpu.log`. Its sound off. Record frame times, the renderer and every error.
* Both browsers: the first run's probe page (WebGL 1 and 2, WebGPU with each Chromium switch alone and both, a WebGPU picture), Chromium's and Firefox's video decoders on a longer H.264 clip and a VP9 one (decision B: GPU decoding in Chromium on Debian, and if it fails, where), and the WebGL conformance suite in each.
* glmark2 on DRM and under Weston, piglit's GL suite, dEQP-GLES and dEQP-VK subsets, IGT's KMS tests built from the pinned source, each with passes, failures and time, on Crocus and hasvk.
* The screen: these take the panel; keep its backlight low. Sound stays off: no sound driver loads.
* Leave Debian running for step 10, with nothing of yours running. Close with the reference numbers, in one table, and what decisions B and C and risk 1 should say.
