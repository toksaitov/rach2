# Linux's Venus references on the testing PC

Debian's results through Venus on the testing PC's WSL, the reference that Rach2's results in the same QEMU profile are compared with. They come from the Debian reference guest of the graphics tests ([its Readme](../../Readme.md#the-reference-guest)) on the `venus` profile, under KVM with 8 CPUs and 8 GiB, over the host's lavapipe with the udmabuf module loaded. The guest's Venus driver is Mesa 26.1.6-1~bpo13+1's, as Rach2's root has it ([Venus](../../../../vendor/gnu/Readme.md#venus)), and every other driver Mesa 25.0.7-2+deb13u1's. The files are the tools' own output, unedited; text is named `.out`, since it keeps the tools' own characters.

## Results

* `vulkaninfo/`, taken on 2026-11-10: `summary.out` is `vulkaninfo --summary` with every driver, which lists Venus (`Virtio-GPU Venus (llvmpipe (LLVM 21.1.8, 256 bits))`, Vulkan 1.4.307, `Mesa 26.1.6-1~bpo13+1`) and the guest's own llvmpipe (`Mesa 25.0.7-2+deb13u1`); `loader.out` is the same with `VK_LOADER_DEBUG=driver`, the ICD files the loader found and the library each names; `venus.out` is the whole `vulkaninfo` of Venus alone, with `VK_DRIVER_FILES` naming its ICD: 164 device extensions, `VK_KHR_load_store_op_none`, `VK_KHR_maintenance5`, `VK_KHR_maintenance6` and `VK_EXT_map_memory_placed` among them, with `memoryMapPlaced` and `memoryUnmapReserve` true, `memoryMapRangePlaced` false and a placed-map alignment of 4 KiB; `install.out` lists the guest's ICD files, the Venus library and the two diversions of 25.0.7's Venus files; `record.out` is the run's record.

The run that took `vulkaninfo/`, from the top of the tree, with the outputs then moved into it and `icd.out` named `install.out`:

    sh tests/graphics/debian-ref/debian-ref.sh run -p venus -r record.out \
        -o summary.out -o loader.out -o venus.out -o icd.out \
        'vulkaninfo --summary > /tmp/summary.out 2>&1;
         VK_LOADER_DEBUG=driver vulkaninfo --summary > /tmp/loader.out 2>&1;
         VK_DRIVER_FILES=/usr/share/vulkan/icd.d/venus_icd.x86_64.json vulkaninfo > /tmp/venus.out 2>&1;
         { ls -l /usr/share/vulkan/icd.d/ /usr/lib/x86_64-linux-gnu/venus/;
           cat /usr/share/vulkan/icd.d/venus_icd.x86_64.json;
           dpkg-divert --list | grep mesa25; } > /tmp/icd.out 2>&1'

* `deqp/smoke/` and `deqp/fraction/`, taken on 2026-11-10, are `debian.sh deqp`'s output for VK-GL-CTS 1.4.1.1's `deqp-vk` on Venus's ICD alone ([Venus's dEQP](../../Readme.md#venuss-deqp)): `smoke` the 9 cases of `deqp-vk-venus-smoke.txt`, all `Pass`, and `fraction` the 96,037 cases of `deqp-vk-lavapipe.txt.gz` whole, 37,012 `Pass`, 59,004 `NotSupported`, 15 `Crash` and 6 `Timeout`, 8 of the crashes and timeouts passing when run again, in 3009 s with 4 processes. Each has `results.csv.gz`, `flakes.out`, `summary.out`, `vulkaninfo.out`, `record.out`, the runner's `record`, and `log.out`, the command's output with QEMU's and the render server's lines.
* `lost/`, taken on 2026-11-10, is `venus-lost.sh` with the host's render server killed under its four processes: `lost.out` the lines the case compares, each process ended by SIGABRT and `vulkaninfo` failing with VK_ERROR_OUT_OF_HOST_MEMORY; `deqp-N.out` each process's output; `vulkaninfo.out` vulkaninfo's; `kill.out` the host's kill, the server and the four processes it forked, one for each context; `record.out` the run's record; and `log.out` the command's output with QEMU's lines.
* `vkcube/`, taken on 2026-11-10, is `debian.sh vkcube`'s output for vkcube 1.4.304.0 on Venus's ICD alone under Weston 14.0.2 ([vkcube presenting](../../Readme.md#vkcube-presenting)): `frames.out`, frame 300 `crc32 0x75d4ad32` and frame 345 `crc32 0x1a0d31d7`, both 1280x800, the same in each of three runs; `frame-300.png` and `frame-345.png`, the shots that gave them; `timing.out`, 300 presents in 2.959 s, 101.39 a second; `gpu.out`, vkcube's device line; `results.out` and `record.out`; and the logs of vkcube, the layer and Weston, and `log.out`, the command's output with QEMU's lines.
* `gles/`, taken on 2026-11-12, is `debian.sh deqp`'s output for the dEQP-GLES subset, every 10th case of each suite of [`deqp-gles-virgl.txt.gz`](../../deqp-gles-virgl.txt.gz), run by VK-GL-CTS 1.4.1.1's `glcts` on the profile's GL ([Venus's dEQP](../../Readme.md#venuss-deqp)): `renderer`, the renderer each case asks the record to name, `virgl (LLVMPIPE (LLVM 21.1.8, 256 bits))` at OpenGL ES 3.2; and for each suite `SUITE.csv.gz`, the runner's `results.csv.gz`, and `SUITE.record`, its record. dEQP-GLES2 gives 1,708 `Pass`, 1 `Fail` and 8 `NotSupported` of 1,717 cases, dEQP-GLES3 4,327 `Pass`, 14 `QualityWarning` and 3 `NotSupported` of 4,344, and dEQP-GLES31 3,640 `Pass`, 2 `Fail` and 26 `NotSupported` of 3,668. The run of each suite, with OUT then moved here:

        sh tests/graphics/runners/debian.sh deqp -p venus -c 8 -m 8192 OUT -v 1.4.1.1 -e 10 -s 1 \
            -l /tmp/deqp-gles-virgl.txt.gz SUITE

* Rates, taken on 2026-11-10, each run in the same QEMU profile with 8 CPUs, 8 GiB, KVM and 4 dEQP processes, as each run's summary line gives them; the suite records rates and judges none, and only their ratio means anything, since the host runs other work beside them:

| Cases | Debian guest | Rach2 | Rach2's rate of Linux's |
| --- | --- | --- | --- |
| part 1 of 8 of the fraction, 12,005 | 401 s, host load 15 | 512 s, host load 11 to 32; 513 s and 536 s in earlier runs | 0.78 |
| every 500th case, 193 | 11 s | 17 s and 45 s in two runs | too short a run to say |
| vkcube, 300 presents in FIFO | 101.39 a second, host load 29 to 37; 119.90 and 100.00 in earlier runs | 69.67 and 58.02 a second in two runs, with 11 and 15 of the host's 32 CPUs busy | 0.57 to 0.69 |

The runs that took them, from the top of the tree, with each output's files then named as here:

    sh tests/graphics/runners/debian.sh deqp -p venus -c 8 -m 8192 OUT -v 1.4.1.1 \
        -i /usr/share/vulkan/icd.d/venus_icd.x86_64.json -l /tmp/deqp-vk-venus-smoke.txt vk
    sh tests/graphics/runners/debian.sh deqp -p venus -c 8 -m 8192 OUT -v 1.4.1.1 \
        -i /usr/share/vulkan/icd.d/venus_icd.x86_64.json \
        -l /tmp/deqp-vk-lavapipe.txt.gz -f /tmp/deqp-vk-lavapipe.flakes vk

and for `lost/`, with `cts.tar.gz` VK-GL-CTS 1.4.1.1 of the GNU layer's build, `build/x86_64/vendor/gnu/vk-gl-cts/opt/vk-gl-cts-1.4.1.1`, and beside it on the host a loop that waits for `devlost-go` in the guest's console log and then runs `sh tests/graphics/runners/venus-lost.sh kill` on the directory of that log, the run's work directory, which QEMU's command line names:

    sh tests/graphics/debian-ref/debian-ref.sh run -p venus -c 8 -m 8192 -g conformance \
        -r record.out -i cts.tar.gz -i tests/graphics/runners/venus-lost.sh \
        -i tests/graphics/deqp-vk-lavapipe.txt.gz -o lost.tar.gz \
        'sudo tar -xzf /tmp/cts.tar.gz -C /opt &&
         sh /tmp/venus-lost.sh start -l /tmp/deqp-vk-lavapipe.txt.gz -o /tmp/out &&
         echo devlost-go | sudo tee /dev/ttyS0 > /dev/null &&
         sh /tmp/venus-lost.sh after -o /tmp/out && tar -czf /tmp/lost.tar.gz -C /tmp/out .'

The rates' Debian runs are the fraction's command with `-e 8 -s 1` and with `-e 500`, and Rach2's are its cases, as `RACH2_QEMU_ACCEL=kvm ARCH=x86_64 RACH2_CASES=machine.venus.fraction-1 tests/run venus` and the same with `machine.venus.sample`, whose summary lines the run's log has.

`vkcube/` and the vkcube rates come from these, the first in the Debian guest, with the output's files then named as here, and the second on Rach2, whose result line gives the rate:

    sh tests/graphics/runners/debian.sh vkcube -p venus -c 8 OUT \
        -i /usr/share/vulkan/icd.d/venus_icd.x86_64.json
    RACH2_QEMU_ACCEL=kvm ARCH=x86_64 RACH2_CASES=machine.vkpresent.vkcube tests/run vkpresent

The Direct3D results are stored in `../d3d-venus-wsl/` by the task that takes them on this guest.

## Updating

A new Venus pin in `vendor/gnu/debian/venus.list` takes every reference here again, on a guest prepared with it, as the Mesa qualification record in the development log lists. A new run replaces a directory whole, and its commit message says what was run and where.
