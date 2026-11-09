# Steps 5 and 7: GPU paths for light programs, and Rach2's suites under KVM and WHPX

Rows 5 and 7 of `run2-plan.md`.

* Step 5: the same light programs (glmark2 offscreen, the first run's WebGL probe in Chromium, a vkcube offscreen run) in a Debian 13 guest three ways: on the guest's llvmpipe; on virgl over the RTX 4080 SUPER in WSL (`GALLIUM_DRIVER=d3d12 MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA` in QEMU's environment, as the first run measured); and on the stock QEMU 11.1.0 on Windows with its GL display over ANGLE, headless from session 0 (say what session 0 allows; change nothing on Windows to get a desktop session). It decides whether tasks 74-1, 74-2 and 74-5 stay a must, and answers the owner's question on native QEMU.
* Step 7: Rach2's own suites under KVM in WSL and WHPX on Windows against TCG, with exits counted where the engine allows. Use the release copy's build, copied into your own directory; run the suites with `tests/run` from your own copy of the tree at master's head, never from the release copy. Pick the suites that boot the system most (name them). Then one PCI scan at boot: a local patch in your copy that scans the PCI buses once and serves every driver from that list, measured against the unpatched kernel under each engine. The patch stays in your directory as a diff; nothing is committed. It decides whether kernel work may use the fast paths, and the owner's TCG default.
* No other heavy work runs on the testing PC during these timings.
* From step 1 (`run2/step1.md`): Windows QEMU started over SSH runs in session 0 with no desktop, the test engine's build has no GL device, and WHPX exits cannot be counted from Windows. So step 5 first shows whether the stock QEMU's `egl-headless` (or `dbus`) makes a GL context in session 0; if it cannot, say so and measure only what can run. Step 7 counts WHPX exits by QEMU's trace of device accesses, as step 1 did, and KVM's by its own statistics.

## Notes from steps 3 and 4 (the chat, 05:35)

* The testing PC is yours alone until you finish; WSL was idle at 05:34 (no QEMU, load falling, 903 GiB free on its root). Step 6 follows you there.
* Step 3's timed guest, `/home/testing/rach2/build/cache/run2/step3-4/deb34.qcow2` (Debian 13 with Chromium, the test suites and their runners built; `run2/step3-4.md` says what is in it), and `step2-copy.qcow2` beside it may be the base of your guests: use an overlay of your own and never write the base.
* In any guest on llvmpipe or lavapipe: `LP_NUM_THREADS=0`, Xvfb with `-noreset`, and Chromium's WebGL needs `--ignore-gpu-blocklist`; start each timed run with Mesa's shader cache empty, and say so.
* A run's numbers move about 3% from run to run (step 4's repeat of one guest alone); give a difference smaller than that as no difference, or repeat the run.
