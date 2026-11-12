# piglit's quick_gl against Linux on virgl and Venus

Task 77-13 ran piglit's `quick_gl` subset in QEMU on the testing PC on Rach2 at the milestone branch's head and on Debian's Linux 6.12 guest, on virgl and on Venus, and lists each test that passes on Linux and fails, crashes or is missing on Rach2. There is none: on both drivers every test and subtest that passed or skipped in the Debian guest did the same on Rach2, and Rach2 ran every test Debian ran. No subtask was filed, since no gap has a cause to fix.

## The subset

[`tests/graphics/piglit-quick-gl.txt.gz`](../../tests/graphics/piglit-quick-gl.txt.gz) is every second test of Debian's piglit 0.0~git20250409.af62c0dea-2's `quick_gl` profile, 3,941 of its 7,888, without six that end the testing PC's QEMU on virgl or get the host's device removed under the next test; its header gives the commands that made it. Every name is a test of the `quick` profile, which `piglit.sh` runs. In the Debian guest every 16th test of the profile, 493, took 191 s on virgl and 171 s on Venus, so the whole profile would take about 51 and 46 minutes there; half of it fits in an hour on Rach2 too, whose dEQP-VK on Venus ran at 0.78 of Linux's rate. The 493 tests also ran in the reference runs below, and the two Debian runs gave the same result for every one of their tests and subtests on both drivers.

The two drivers are:

* virgl: the `virgl` profile, virgl over the host's D3D12 driver on the RTX 4080 SUPER, OpenGL 4.2 and OpenGL ES 3.1, with `piglit.sh -g`, one test at a time.
* Venus: the `venus` profile, zink over Venus over the host's lavapipe, OpenGL 4.6 and OpenGL ES 3.2, with the new `piglit.sh -z /usr/share/vulkan/icd.d/venus_icd.x86_64.json`, a process on each CPU. Zink takes Venus only in software mode, `LIBGL_ALWAYS_SOFTWARE=1`, since Venus gives the host device's type, lavapipe's CPU type, and zink otherwise passes over a CPU device: without it EGL printed "ZINK: failed to choose pdev" and fell back to the guest's llvmpipe, in the Debian guest as it would on Rach2.

## How it was run

Each side ran in the same QEMU 10.2.1 profile with 8 CPUs, 8 GiB and KVM, one test list whole in one boot. Debian's ran the two profiles in two guests at once:

    sh tests/graphics/runners/debian.sh piglit -p virgl -c 8 OUT -g -l /tmp/piglit-quick-gl.txt.gz
    sh tests/graphics/runners/debian.sh piglit -p venus -c 8 OUT \
        -z /usr/share/vulkan/icd.d/venus_icd.x86_64.json -l /tmp/piglit-quick-gl.txt.gz

Rach2's ran as the cases that judge it against those results, stored under `tests/graphics/ref/virgl-wsl/piglit/` and `tests/graphics/ref/venus-wsl/piglit/`, one after the other:

    RACH2_QEMU_ACCEL=kvm ARCH=x86_64 \
        RACH2_CASES='machine.virgl.piglit-quick-gl machine.venus.piglit-quick-gl' tests/run virgl venus

The team accounts' `.bashrc` sets `RACH2_QEMU_ACCEL=tcg`, under which the first Rach2 attempt booted; it was stopped after 20 minutes and run again under KVM, the references' accelerator, as the earlier Venus rate comparisons were.

## Results

Counts of tests and subtests, piglit's result for each, on 2026-11-12. Both Rach2 cases passed `judge.py results`: no test or subtest that passed or skipped in Debian ended otherwise on Rach2, and none was missing.

| Driver | System | Results | Pass | Skip | Fail | Other | Wall |
| --- | --- | --- | --- | --- | --- | --- | --- |
| virgl | Debian, Linux 6.12 | 14,098 | 12,932 | 696 | 250 | 220 | 1185 s |
| virgl | Rach2 | 14,098 | 12,934 | 696 | 248 | 220 | 1113 s |
| zink on Venus | Debian, Linux 6.12 | 14,198 | 13,141 | 725 | 106 | 226 | 1210 s |
| zink on Venus | Rach2 | 14,198 | 13,142 | 725 | 106 | 225 | 1247 s |

Debian's 220 others on virgl are 201 crashes, 15 notrun and 4 warns, and its 226 on Venus 210 crashes, 12 notrun and 4 warns. Rach2's two more passes on virgl and one more on Venus are tests or subtests that did not pass on Linux; the cases keep no output when they pass, so those three are not named here. The guest's processors were 11.6% busy on virgl and 94.3% on Venus on Rach2, against 8.2% and 51.0% in the Debian guest, where the host's load was 15 to 25 during both runs.

## Gaps against Linux

None. The Rach2 kernel code the two drivers exercise, the virtio-gpu driver's 3D contexts, blobs and context init and its fences, gives piglit on both the results Linux 6.12 gives in the same QEMU.
