# Linux's i386 Direct3D runs on the testing PC

The Debian reference guest's runs of the i386 Direct3D probes, from which [`d3d/ref/linux32.json`](../../d3d/ref/linux32.json) and [`d3d/ref/venus32.json`](../../d3d/ref/venus32.json) were made, the references that the i386 rows of the `d3d` and `d3dvenus` suites judge Rach2 by ([i386 Direct3D](../../Readme.md#i386-direct3d)). Each run is `d3d/reference.sh`'s, in the Debian guest under KVM with 8 CPUs and 16 GiB, with GE-Proton11-12 and `run.py -R`, after `d3d/i386.sh` gave the guest the root's i386 layer: Mesa 25.0.7 for i386 beside amd64's, and Mesa 26.1.6's i386 Venus driver beside its amd64 one. Every run was taken on 2026-11-10 with the image `debian-13-generic-amd64-20261103-2651.qcow2` and the snapshot 20261108T000000Z, as each run's `record` says.

## Results

Each archive is one run's output directory whole: a numbered directory for each row, with `run.py`'s `environment.json`, `results.json` and logs, the row's exit status in `N.status`, and `record`, the guest's record and the host's.

* `lavapipe.tar.gz` and `lavapipe2.tar.gz`, `ref-lavapipe` and `ref-lavapipe2`: the three probes on lavapipe in the `virtio-vga` profile, `run.py -a lavapipe`, through DXVK and vkd3d-proton, under WoW64 and in Proton's default mode.
* `venus.tar.gz` and `venus2.tar.gz`, `ref-venus` and `ref-venus2`: the three probes on Venus in the `venus` profile, over the host's lavapipe with udmabuf's `size_limit_mb` at 16384, through DXVK and vkd3d-proton and through WineD3D on Vulkan with Wine's vkd3d, each under WoW64 and in the default mode.

All 18 rows drew in both runs: each probe exited 0 and read back the interior pixel at 255,128,0,255 and the clear pixel at 0,0,128,255. While each probe held, `run.py` read its process: under WoW64 a 64-bit executable whose map has `x86_64-unix/ntdll.so`, in the default mode a 32-bit one whose map has `i386-unix/ntdll.so`. The adapters were the 64-bit probes' own: DXVK's Direct3D 9 names `AMD Radeon RX 6700 XT` with the driver `aticfx32.dll` in both modes on both devices, and every other row names the device, `llvmpipe (LLVM 19.1.7, 256 bits)` on lavapipe and `Virtio-GPU Venus (llvmpipe (LLVM 21.1.8, 256 bits))` on Venus, WineD3D's Direct3D 9 with the driver `wined3d.dll`. An i386 program's Direct3D 11 and 12 adapter has 3,072 MiB of dedicated and of shared memory through DXVK and vkd3d-proton and 4,095 MiB through WineD3D, in both modes; the probes print it and no check reads it.

A first try of `ref-venus2` drew all 12 rows but did not keep them: after the i386 packages were installed, apt 3.0.3 printed `W: Tried to start delayed item ... trixie/main i386 libllvm19 i386 1:19.1.7-3+b1, but failed` at least 453,439 times, until the guest's output passed the 64 MiB that `etude admit` lets a run's standard error have and the run was refused ([the upstream index](../../../../dev-log/upstream.md)). `i386.sh` now keeps apt's output in the guest's own log; the run taken again had 684 lines of it and no warning.

The references were folded from the four runs, each archive unpacked in one directory:

    python3 tests/graphics/d3d/mkref.py \
        "Debian 13 on lavapipe in the virtio-vga profile on the testing PC, Mesa 25.0.7 for both architectures with the root's i386 layer, GE-Proton11-12, 8 CPUs, 16 GiB; the i386 probes under WoW64 and in the default mode, twice" \
        tests/graphics/d3d/ref/linux32.json ref-lavapipe ref-lavapipe2
    python3 tests/graphics/d3d/mkref.py \
        "Debian 13 on Venus over the testing PC's lavapipe, Mesa 26.1.6's Venus driver for both architectures with the root's i386 layer, GE-Proton11-12, 8 CPUs, 16 GiB, udmabuf size_limit_mb 16384; the i386 probes on both paths under WoW64 and in the default mode, twice" \
        tests/graphics/d3d/ref/venus32.json ref-venus ref-venus2

The runs that took them, from the top of the tree, `ref-lavapipe2` as `ref-lavapipe` and `ref-venus2` as `ref-venus`:

    sh tests/graphics/d3d/reference.sh -a lavapipe -p virtio-vga ref-lavapipe \
        probe/9-wow64 probe/11-wow64 probe/12-wow64 \
        probe/9-i386 probe/11-i386 probe/12-i386
    sh tests/graphics/d3d/reference.sh ref-venus \
        probe/proton/9-wow64 probe/proton/11-wow64 probe/proton/12-wow64 \
        probe/proton/9-i386 probe/proton/11-i386 probe/proton/12-i386 \
        probe/wined3dvk/9-wow64 probe/wined3dvk/11-wow64 probe/wined3dvk/12-wow64 \
        probe/wined3dvk/9-i386 probe/wined3dvk/11-i386 probe/wined3dvk/12-i386

## Updating

The runs are taken again, and the two references folded again from them, when a probe, `run.py`'s probe rows, GE-Proton, the root's i386 layer or Mesa for either architecture changes.
