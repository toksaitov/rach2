# Faults in other projects' code

This file indexes every fault Rach2 has found in another project's code, whether or not Rach2 works around it, so that someone can research it further, report it upstream or send a patch. Each fault is recorded in the milestone where it was found or first recorded, in `dev-log/NN-name/upstream.md`, and has a row in the index below. A documented limit, a feature the project lacks by design and a mistake of Rach2's are not faults here.

## Format

A milestone's `upstream.md` opens with a title, `# Upstream faults of milestone NN`, and a line that points here. One `##` section follows for each fault, headed by the fault as a claim. Its body is eight bullets, always these and in this order, each opening with its label:

* Project: the project and its exact version, with the source's name and SHA-256 where Rach2 keeps it.
* Fails: what goes wrong, the claim first, then the code that does it, by file and line in the project's source.
* Found: the date, the machine and the milestone, and what Rach2 was doing.
* Reproduction: the steps that show the fault, or what a reproduction still needs.
* Evidence: the records, commits, patches and logs, by path from the top of the repository.
* Rach2: what Rach2 did about it, a workaround, a local patch or nothing, with its commit.
* Status: unreported; reported, with the link; patch sent, with the link; or fixed upstream in a named version, with the upstream commit when it is known. A draft is marked so.
* Next: what must be done before a report or a patch: the checks against current upstream, the missing facts, the route.

The bullets state only what the records show. A fact the records do not hold, such as an upstream commit or a version, is written as missing, never guessed. A long record may follow the eight bullets with `###` sections or keep its detail in a file of its own in the milestone's directory, which the entry links. When an entry's status changes, its row below changes in the same commit.

## Index

| Fault | Project | Milestone | Status | Entry |
|---|---|---|---|---|
| QEMU replaces TCO status bits on a write that should clear them | QEMU 10.0.0 and 11.1.3 | 78 | unreported | [78-intel/upstream.md](78-intel/upstream.md#qemu-replaces-tco-status-bits-on-a-write-that-should-clear-them) |
| BusyBox uses a freed set-id configuration | BusyBox 1.36.1 | 19 | unreported | [19-busybox-first/upstream.md](19-busybox-first/upstream.md#busybox-uses-a-freed-set-id-configuration) |
| termbox2 misspells its Windows unsupported message | termbox2 2.7.0-dev | 27 | unreported | [27-prose/upstream.md](27-prose/upstream.md#termbox2-misspells-its-windows-unsupported-message) |
| BusyBox time reports four times the peak memory | BusyBox 1.36.1 | 54 | unreported | [54-gnu/upstream.md](54-gnu/upstream.md#busybox-time-reports-four-times-the-peak-memory) |
| TinyCC leaves a static executable's PLT slots unfilled | TinyCC 0.9.27 | 57 | unreported | [57-arch/upstream.md](57-arch/upstream.md#tinycc-leaves-a-static-executables-plt-slots-unfilled) |
| TinyCC fills a static executable's GOT after dropping its relocations | TinyCC 0.9.27 | 57 | unreported | [57-arch/upstream.md](57-arch/upstream.md#tinycc-fills-a-static-executables-got-after-dropping-its-relocations) |
| TinyCC's i386 assembler writes 32-bit code under code16 | TinyCC 0.9.27 | 57 | unreported | [57-arch/upstream.md](57-arch/upstream.md#tinyccs-i386-assembler-writes-32-bit-code-under-code16) |
| TinyCC's assembler shortens a forward label difference | TinyCC 0.9.27 | 57 | unreported | [57-arch/upstream.md](57-arch/upstream.md#tinyccs-assembler-shortens-a-forward-label-difference) |
| doomgeneric keeps a pointer to a demo name on the stack | doomgeneric fc601639494e | 57 | unreported | [57-arch/upstream.md](57-arch/upstream.md#doomgeneric-keeps-a-pointer-to-a-demo-name-on-the-stack) |
| QEMU's WHPX returns a stale XSAVE area size | QEMU 11.1.0 to 11.1.2 | 62 | unreported | [62-etude/upstream.md](62-etude/upstream.md#qemus-whpx-returns-a-stale-xsave-area-size) |
| libslirp passes a Winsock socket to GLib as a descriptor | libslirp 4.9.3 | 62 | unreported | [62-etude/upstream.md](62-etude/upstream.md#libslirp-passes-a-winsock-socket-to-glib-as-a-descriptor) |
| mtools parses an offset with strtol | mtools 4.0.49 | 62 | unreported | [62-etude/upstream.md](62-etude/upstream.md#mtools-parses-an-offset-with-strtol) |
| mtools takes a block device's length only when its geometry fails | mtools 4.0.49 | 62 | unreported | [62-etude/upstream.md](62-etude/upstream.md#mtools-takes-a-block-devices-length-only-when-its-geometry-fails) |
| mtools leaves the Linux musl target without block sizing | mtools 4.0.49 | 62 | unreported | [62-etude/upstream.md](62-etude/upstream.md#mtools-leaves-the-linux-musl-target-without-block-sizing) |
| TinyCC leaves a register variable unbound without a percent sign | TinyCC 0.9.27 | 62 | unreported | [62-etude/upstream.md](62-etude/upstream.md#tinycc-leaves-a-register-variable-unbound-without-a-percent-sign) |
| BusyBox drops the line being typed when a background job ends | BusyBox 1.36.1 | 62 | fixed upstream in 1.37.0 | [62-etude/upstream.md](62-etude/upstream.md#busybox-drops-the-line-being-typed-when-a-background-job-ends) |
| Debian's chromium-headless-shell launcher does not exec | Debian's chromium-headless-shell | 66 | unreported | [66-terminal/upstream.md](66-terminal/upstream.md#debians-chromium-headless-shell-launcher-does-not-exec) |
| libtsm sends Shift F1 as F11 and swaps the keypad's divide and multiply | libtsm 4.8.0 | 66 | unreported | [66-terminal/upstream.md](66-terminal/upstream.md#libtsm-sends-shift-f1-as-f11-and-swaps-the-keypads-divide-and-multiply) |
| libtsm takes CSI with a greater-than marker for the plain sequence | libtsm 4.8.0 | 66 | unreported | [66-terminal/upstream.md](66-terminal/upstream.md#libtsm-takes-csi-with-a-greater-than-marker-for-the-plain-sequence) |
| libtsm's CSI collector drops the less-than and equals markers | libtsm 4.8.0 | 66 | unreported | [66-terminal/upstream.md](66-terminal/upstream.md#libtsms-csi-collector-drops-the-less-than-and-equals-markers) |
| libtsm's combined symbols skip an index and free an interior pointer | libtsm 4.8.0 | 66 | unreported | [66-terminal/upstream.md](66-terminal/upstream.md#libtsms-combined-symbols-skip-an-index-and-free-an-interior-pointer) |
| libtsm clips a wide character in the last column and drops combining marks | libtsm 4.8.0 | 66 | unreported | [66-terminal/upstream.md](66-terminal/upstream.md#libtsm-clips-a-wide-character-in-the-last-column-and-drops-combining-marks) |
| libtsm's DECSTBM leaves the cursor in place | libtsm 4.8.0 | 66 | unreported | [66-terminal/upstream.md](66-terminal/upstream.md#libtsms-decstbm-leaves-the-cursor-in-place) |
| libtsm's REP without a parameter fills the rest of the line | libtsm 4.8.0 | 66 | unreported | [66-terminal/upstream.md](66-terminal/upstream.md#libtsms-rep-without-a-parameter-fills-the-rest-of-the-line) |
| libtsm's REP does not wrap | libtsm 4.8.0 | 66 | unreported | [66-terminal/upstream.md](66-terminal/upstream.md#libtsms-rep-does-not-wrap) |
| libtsm scrolls the region on a line feed below it | libtsm 4.8.0 | 66 | unreported | [66-terminal/upstream.md](66-terminal/upstream.md#libtsm-scrolls-the-region-on-a-line-feed-below-it) |
| libtsm's RIS leaves the alternate screen's text | libtsm 4.8.0 | 66 | unreported | [66-terminal/upstream.md](66-terminal/upstream.md#libtsms-ris-leaves-the-alternate-screens-text) |
| libtsm's mode 1049 keeps the alternate screen's attributes | libtsm 4.8.0 | 67 | unreported | [67-agents/upstream.md](67-agents/upstream.md#libtsms-mode-1049-keeps-the-alternate-screens-attributes) |
| cmus's configure tests the build host's OSS header | cmus 2.12.0 | 68 | unreported | [68-sound/upstream.md](68-sound/upstream.md#cmuss-configure-tests-the-build-hosts-oss-header) |
| cmus takes its version from an enclosing git clone | cmus 2.12.0 | 68 | unreported | [68-sound/upstream.md](68-sound/upstream.md#cmus-takes-its-version-from-an-enclosing-git-clone) |
| glibc formats a rounded alternate g conversion without its zeros | glibc 2.43 | 68 | unreported | [68-sound/upstream.md](68-sound/upstream.md#glibc-formats-a-rounded-alternate-g-conversion-without-its-zeros) |
| iw dev link reads the DTIM period and beacon interval unchecked | iw 6.17 | 69 | unreported | [69-wifi/upstream.md](69-wifi/upstream.md#iw-dev-link-reads-the-dtim-period-and-beacon-interval-unchecked) |
| BusyBox ping stops at an interval of 0 | BusyBox 1.36.1 | 73 | unreported | [73-wifi-steady/upstream.md](73-wifi-steady/upstream.md#busybox-ping-stops-at-an-interval-of-0) |
| wpa_supplicant arms a pairwise rekey only after a completed one | wpa_supplicant, Alpine's | 74 | unreported | [74-wifi-soak/upstream.md](74-wifi-soak/upstream.md#wpa_supplicant-arms-a-pairwise-rekey-only-after-a-completed-one) |
| The access point sends its queue under a new key with the old key's numbers | TP-Link access point | 74 | unreported | [74-wifi-soak/upstream.md](74-wifi-soak/upstream.md#the-access-point-sends-its-queue-under-a-new-key-with-the-old-keys-numbers) |
| The access point forgets a station without a deauthentication | TP-Link access point | 74 | unreported | [74-wifi-soak/upstream.md](74-wifi-soak/upstream.md#the-access-point-forgets-a-station-without-a-deauthentication) |
| Linux 6.18.58 goes silent at key renewals on the Lenovo | Linux 6.18.58, Alpine's | 74 | unreported, cause unproved | [74-wifi-soak/upstream.md](74-wifi-soak/upstream.md#linux-61858-goes-silent-at-key-renewals-on-the-lenovo) |
| KVM stops a guest on a huge page of a PFNMAP GPU mapping | Linux 6.12.115, KVM | 76 | unreported | [76-graphics/upstream.md](76-graphics/upstream.md#kvm-stops-a-guest-on-a-huge-page-of-a-pfnmap-gpu-mapping) |
| Chromium's VA-API decoder exports a surface that i965 has no buffer for | Chromium 156 and i965 2.4.1 | 76 | unreported | [76-graphics/upstream.md](76-graphics/upstream.md#chromiums-va-api-decoder-exports-a-surface-that-i965-has-no-buffer-for) |
| WineD3D's Vulkan renderer reads a NULL SPIR-V program in Crysis | Wine 11.0 in GE-Proton11-12 | 76 | unreported | [76-graphics/upstream.md](76-graphics/upstream.md#wined3ds-vulkan-renderer-reads-a-null-spir-v-program-in-crysis) |
| Weston's Xwayland window manager frames an undecorated fullscreen X window | Weston 14.0.2 | 76 | unreported | [76-graphics/upstream.md](76-graphics/upstream.md#westons-xwayland-window-manager-frames-an-undecorated-fullscreen-x-window) |
| QEMU on Windows dies with ANGLE's Vulkan backend | QEMU 11.1.0 for Windows and ANGLE | 76 | unreported | [76-graphics/upstream.md](76-graphics/upstream.md#qemu-on-windows-dies-with-angles-vulkan-backend) |
| Chromium returns no WebGPU adapter for Venus | Chromium 156 | 76 | unreported | [76-graphics/upstream.md](76-graphics/upstream.md#chromium-returns-no-webgpu-adapter-for-venus) |
| Crysis hides its HUD for good when it has no sound output | Crysis | 76 | unreported | [76-graphics/upstream.md](76-graphics/upstream.md#crysis-hides-its-hud-for-good-when-it-has-no-sound-output) |
| Crocus hangs the GPU in piglit's multisample read-front tests | Mesa 25.0.7, Crocus | 76 | unreported | [76-graphics/upstream.md](76-graphics/upstream.md#crocus-hangs-the-gpu-in-piglits-multisample-read-front-tests) |
| i915 underruns the FIFO on pipes B and C through the panel fitter | Linux 6.12.115, i915 | 76 | unreported | [76-graphics/upstream.md](76-graphics/upstream.md#i915-underruns-the-fifo-on-pipes-b-and-c-through-the-panel-fitter) |
| lavapipe segfaults in vkd3d-proton's tests | Mesa 25.0.7, lavapipe | 76 | unreported | [76-graphics/upstream.md](76-graphics/upstream.md#lavapipe-segfaults-in-vkd3d-protons-tests) |
| dEQP-VK timeline semaphore cases hang on lavapipe | Mesa 25.0.7, lavapipe | 76 | unreported | [76-graphics/upstream.md](76-graphics/upstream.md#deqp-vk-timeline-semaphore-cases-hang-on-lavapipe) |
| Chromium's GPU sandbox traps Mesa's queue scheduling calls | Chromium 156 and Mesa 25.0.7 | 76 | unreported | [76-graphics/upstream.md](76-graphics/upstream.md#chromiums-gpu-sandbox-traps-mesas-queue-scheduling-calls) |
| Weston requests cursor names absent from Adwaita | Weston 14.0.2 and Adwaita 48.1 | 76 | unreported, draft | [76-display/upstream.md](76-display/upstream.md#weston-requests-cursor-names-absent-from-adwaita) |
| llvmpipe's JIT calls address 0 for a libm function the host program does not link | Mesa 26.0.8, llvmpipe | 77 | unreported | [77-vgpu/upstream.md](77-vgpu/upstream.md#llvmpipes-jit-calls-address-0-for-a-libm-function-the-host-program-does-not-link) |
| QEMU's intel-iommu leaves CFIS clear when CFI is set | QEMU 10.2.1 | 82 | unreported | [82-assign/upstream.md](82-assign/upstream.md#qemus-intel-iommu-leaves-cfis-clear-when-cfi-is-set) |
| Mesa's vtest client counts a put's data in words but sends it unpadded | Mesa 26.0.8 | 83 | unreported | [83-testbed/upstream.md](83-testbed/upstream.md#mesas-vtest-client-counts-a-puts-data-in-words-but-sends-it-unpadded) |
| Debian's virgl-server does not depend on an EGL loader | Debian's virglrenderer 1.1.0-2 | 83 | unreported | [83-testbed/upstream.md](83-testbed/upstream.md#debians-virgl-server-does-not-depend-on-an-egl-loader) |
| QEMU aborts in libepoxy on exit after virgl fails to initialize | QEMU 11.1.2 | 83 | unreported | [83-testbed/upstream.md](83-testbed/upstream.md#qemu-aborts-in-libepoxy-on-exit-after-virgl-fails-to-initialize) |
