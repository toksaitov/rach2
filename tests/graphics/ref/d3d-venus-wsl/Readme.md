# Linux's Direct3D runs on Venus on the testing PC

The Debian reference guest's Direct3D rows on Venus, from which [`d3d/ref/venus.json`](../../d3d/ref/venus.json) was made, the reference that the `d3dvenus` suite judges Rach2 by ([Direct3D on Venus](../../Readme.md#direct3d-on-venus)). Each run is `d3d/reference.sh`'s, in the Debian guest on the `venus` profile under KVM with 8 CPUs and 16 GiB, over the host's lavapipe with udmabuf's `size_limit_mb` at 16384, Mesa 26.1.6's Venus driver, GE-Proton11-12 and `run.py -a venus -R`. Every run was taken on 2026-11-10 with the image `debian-13-generic-amd64-20261103-2651.qcow2` and the snapshot 20261108T000000Z, as each run's `record` says.

## Results

Each archive is one run's output directory whole: a numbered directory for each row, with `run.py`'s `environment.json`, `results.json` and logs, the row's exit status in `N.status`, and `record`, the guest's record and the host's.

* `proton.tar.gz` and `proton2.tar.gz`, `ref-proton` and `ref-proton2`: the six probes, Direct3D 9, 11 and 12 through Proton's DXVK and vkd3d-proton and through WineD3D on Vulkan with Wine's vkd3d, and Wine's 13 units through Proton. Every probe drew and read both pixels back right in both runs. The two runs agree in every unit's summary and exit but for the units that crash before their summary, whose exits vary. `d3d9:device` stalled at the same line in both, its process alive with no CPU time, and was killed by hand.
* `wined3dvk.tar.gz` and `wined3dvk1.tar.gz`, `ref-wined3dvk` and `ref-wined3dvk1`: Wine's 13 units through WineD3D on Vulkan. The two agree unit for unit. `wined3dvk1-lost.tar.gz` is the first run's `d3d10:effect`, set apart from it: Proton's prefix creation hung there, with explorer.exe logging "no driver could be loaded", and the row has no `results.json`; the unit ran in the second run.
* `vkd3d.tar.gz`, `ref-vkd3d`: vkd3d-proton's 578 tests, the eight parts of `run.py`'s list. None crashed. `test_multithread_command_queue_exec` stalled after its 26th failure at line 4176 and was killed by hand.

The reference was folded from the five runs, each archive unpacked in one directory and `wined3dvk1-lost.tar.gz` left out:

    python3 tests/graphics/d3d/mkref.py \
        -s proton/d3d9:device -s test_multithread_command_queue_exec \
        "Debian 13 on Venus over the testing PC's lavapipe, Mesa 26.1.6's Venus driver, GE-Proton11-12, 8 CPUs, 16 GiB, udmabuf size_limit_mb 16384; probes and Proton units twice, WineD3D on Vulkan twice, vkd3d-proton once" \
        tests/graphics/d3d/ref/venus.json \
        ref-proton ref-proton2 ref-wined3dvk ref-wined3dvk1 ref-vkd3d

The fold gives Proton's `d3d9:visual` the stored runs' end, exit 245 before its summary after 384 test lines, where DXVK timed out in `stencil_cull_test`. That end follows the host's shader cache and load, and `venus.json` replaces it by hand with a stop at the unit's `visual.c:2412` record, the last before `z_range_test`, where the Debian guest's later runs timed out ([Direct3D on Venus](../../Readme.md#direct3d-on-venus)).

The runs that took them, from the top of the tree, `ref-proton2` as `ref-proton` and `ref-wined3dvk1` as `ref-wined3dvk`:

    sh tests/graphics/d3d/reference.sh ref-proton \
        probe/proton/9 probe/proton/11 probe/proton/12 \
        probe/wined3dvk/9 probe/wined3dvk/11 probe/wined3dvk/12 \
        wine/proton/d3d9:d3d9ex wine/proton/d3d9:device \
        wine/proton/d3d9:stateblock wine/proton/d3d9:visual \
        wine/proton/d3d10:device wine/proton/d3d10:effect \
        wine/proton/d3d10:reflection wine/proton/d3d10_1:d3d10_1 \
        wine/proton/d3d10_1:reflection wine/proton/d3d10core:d3d10core \
        wine/proton/d3d11:d3d11 wine/proton/d3d12:d3d12 wine/proton/dxgi:dxgi
    sh tests/graphics/d3d/reference.sh ref-wined3dvk \
        wine/wined3dvk/d3d9:d3d9ex wine/wined3dvk/d3d9:device \
        wine/wined3dvk/d3d9:stateblock wine/wined3dvk/d3d9:visual \
        wine/wined3dvk/d3d10:device wine/wined3dvk/d3d10:effect \
        wine/wined3dvk/d3d10:reflection wine/wined3dvk/d3d10_1:d3d10_1 \
        wine/wined3dvk/d3d10_1:reflection wine/wined3dvk/d3d10core:d3d10core \
        wine/wined3dvk/d3d11:d3d11 wine/wined3dvk/d3d12:d3d12 \
        wine/wined3dvk/dxgi:dxgi
    sh tests/graphics/d3d/reference.sh ref-vkd3d \
        vkd3d/0 vkd3d/1 vkd3d/2 vkd3d/3 vkd3d/4 vkd3d/5 vkd3d/6 vkd3d/7

A stalled unit was found by its process in the guest, alive with no CPU time and its log unchanged for minutes, and killed there with SIGKILL, so that the run went on to the next row.

## Updating

The runs are taken again, and `venus.json` folded again from them, when the guest's Venus driver, GE-Proton, a probe, `run.py`'s unit list or vkd3d-proton's list changes. A unit that stalls is named with `-s`, as above, once it has stalled at the same line in every run that reached it.
