# 84-159: tick-unit inventory before the fixes

This inventory was read at 7f895324d before changing code. Searches over all of `rach2/sys`, including headers, used `tick_count|timer_ticks|jiffies|\bHZ\b|TIMER_HZ|TICKS_PER|ticks_per`, then `kernel_ticks|rtc_ticks_now|TICK_HZ|TICK_MS|\bticks\b|_tick\b`, and rate divisions and the wireless deadline fields and constants. The table lists every matching source line, including declarations and non-time counters. The unit and boundary disposition apply to each listed use unless the exceptions below name it. References are to the code before this task.

* Global `kernel_ticks`, `tick_count`, `rtc_ticks_now`, and deadlines made from them are kernel ticks at 250 Hz. Sleeping interfaces use tick_span, MS_TO_TICKS, TICK_US or TICK_NS at entry; readers use tick_clock for USER_HZ, or multiply the remainder by TICK_US/TICK_NS after division into seconds.
* Scheduler slice, fine service, i915 forcewake, timerfd and real itimer expiries are monotonic nanoseconds; slice_ns never holds ticks. CPU clocks and CPU timers count accounting ticks. Event counts, selection counts, IRQ counts and bkl_tick_count rows count occurrences, with no duration conversion.
* Device clocks named ticks are separate: HDA wall clock is 24 MHz, KVM PIT is 1193182 Hz, iTCO hardware ticks are 0.6 s, APIC counts and TSC counts use their calibrated rates. Those are not kernel ticks. Input and USB service clocks are elapsed monotonic milliseconds; their service queries convert milliseconds to kernel ticks. WLAN TSF is microseconds and wsim already multiplies kernel ticks by TICK_US. Minstrel uses packet windows, not ticks.
* Block request, ATA and lag clocks add elapsed kernel ticks; timeouts are MS_TO_TICKS or seconds times TICK_HZ, and disk-wait-ticks and release tick report ticks explicitly. Writeback converts hundredths with tick_centisecs. MM filemap age uses converted expiry ticks. Display blanking is seconds times TICK_HZ. Watchdog and carrier lifetimes are seconds times TICK_HZ. IP and IPv6 use MS_TO_TICKS or seconds times TICK_HZ; TCP timers use TCP_MS/MS_TO_TICKS, and socket timeouts use tick_span. Random seeding uses the raw tick bits, not a time unit.

| File under rach2/sys | Every matching line | Unit and boundary disposition |
|---|---|---|
| `acpi/osl.c` | 325, 329, 346, 408, 411, 413, 484, 500, 502 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `arch/i686/vdso.c` | 21 | Kernel deadlines convert at entry; device/reference/TSC counts use their own rates. Rate divisions below. |
| `arch/x86/alps.c` | 1421, 1495, 1497, 1504, 2707, 2710 | Kernel deadlines convert at entry; device/reference/TSC counts use their own rates. Rate divisions below. |
| `arch/x86/alps.h` | 104 | Kernel deadlines convert at entry; device/reference/TSC counts use their own rates. Rate divisions below. |
| `arch/x86/apictimer.c` | 1, 2, 39, 51, 52, 68, 71, 74, 77, 217, 218, 225, 235, 239, 252, 260, 266, 275, 278, 280, 333, 366, 392, 396, 436, 437, 446, 458, 459, 464, 489, 495, 501, 539, 643, 647, 699, 701 | Kernel deadlines convert at entry; device/reference/TSC counts use their own rates. Rate divisions below. |
| `arch/x86/apictimer.h` | 36, 43, 68, 69 | Kernel deadlines convert at entry; device/reference/TSC counts use their own rates. Rate divisions below. |
| `arch/x86/cmos.c` | 228 | Kernel deadlines convert at entry; device/reference/TSC counts use their own rates. Rate divisions below. |
| `arch/x86/iommu/fault.c` | 23, 185, 267, 370 | Kernel deadlines convert at entry; device/reference/TSC counts use their own rates. Rate divisions below. |
| `arch/x86/iommu/hw.c` | 187 | Kernel deadlines convert at entry; device/reference/TSC counts use their own rates. Rate divisions below. |
| `arch/x86/isr.c` | 742, 968, 1097, 1108, 1171, 1186, 1188, 1197, 1203, 1205, 1207, 1251, 1281, 1289, 1295, 1307 | Kernel deadlines convert at entry; device/reference/TSC counts use their own rates. Rate divisions below. |
| `arch/x86/itco.c` | 27, 110, 115, 120, 122, 123, 173, 208, 327, 578, 580, 618, 688, 691, 697, 716, 720, 727 | Kernel deadlines convert at entry; device/reference/TSC counts use their own rates. Rate divisions below. |
| `arch/x86/itco.h` | 24 | Kernel deadlines convert at entry; device/reference/TSC counts use their own rates. Rate divisions below. |
| `arch/x86/pit.c` | 3, 19, 91, 93, 95, 117, 143, 145, 156, 160, 168, 169, 173, 174, 192, 243, 259, 262, 310, 327, 332, 399, 401, 403, 407, 446, 457, 487, 562, 576, 582, 583, 584, 587, 589, 592, 594, 600, 603, 608, 609, 610, 613, 620, 631, 644, 661, 663, 669, 710, 713, 715, 724, 725, 739, 747, 750, 801, 819, 826, 848, 874, 894, 921 | Kernel deadlines convert at entry; device/reference/TSC counts use their own rates. Rate divisions below. |
| `arch/x86/pit.h` | 15, 31, 50, 60, 92 | Kernel deadlines convert at entry; device/reference/TSC counts use their own rates. Rate divisions below. |
| `arch/x86/platform.c` | 11, 77, 80, 82, 324, 327, 341, 342, 345, 359, 371, 378, 381, 387, 396, 398, 403, 411, 419, 422, 430, 534, 536, 537, 538, 539, 541 | Kernel deadlines convert at entry; device/reference/TSC counts use their own rates. Rate divisions below. |
| `arch/x86/pmu.c` | 281, 282 | Kernel deadlines convert at entry; device/reference/TSC counts use their own rates. Rate divisions below. |
| `arch/x86/ps2kbd.c` | 30, 46, 58, 94, 99, 101, 106, 108, 191 | Kernel deadlines convert at entry; device/reference/TSC counts use their own rates. Rate divisions below. |
| `arch/x86/ps2kbd.h` | 16, 26 | Kernel deadlines convert at entry; device/reference/TSC counts use their own rates. Rate divisions below. |
| `arch/x86/ps2mouse.c` | 239, 301, 1360, 1405, 1409, 1410, 1414, 1416, 1421, 1434, 1441, 1597, 1598, 1616, 1619, 1628, 1754, 1780, 1804, 1939, 2171, 2487, 2494, 2496, 2497, 2499 | Kernel deadlines convert at entry; device/reference/TSC counts use their own rates. Rate divisions below. |
| `arch/x86/ps2mouse.h` | 154 | Kernel deadlines convert at entry; device/reference/TSC counts use their own rates. Rate divisions below. |
| `arch/x86/refclock.c` | 47, 174 | Kernel deadlines convert at entry; device/reference/TSC counts use their own rates. Rate divisions below. |
| `arch/x86/smpboot.c` | 205, 326 | Kernel deadlines convert at entry; device/reference/TSC counts use their own rates. Rate divisions below. |
| `arch/x86/smpboot.h` | 74 | Kernel deadlines convert at entry; device/reference/TSC counts use their own rates. Rate divisions below. |
| `arch/x86/speaker.c` | 26, 106, 122, 130, 168, 171 | Kernel deadlines convert at entry; device/reference/TSC counts use their own rates. Rate divisions below. |
| `arch/x86/synaptics.c` | 928 | Kernel deadlines convert at entry; device/reference/TSC counts use their own rates. Rate divisions below. |
| `arch/x86/tsc.c` | 19, 139, 209, 370, 441, 447, 502, 533 | Kernel deadlines convert at entry; device/reference/TSC counts use their own rates. Rate divisions below. |
| `arch/x86/tsc.h` | 35, 106, 128, 132, 134 | Kernel deadlines convert at entry; device/reference/TSC counts use their own rates. Rate divisions below. |
| `arch/x86_64/ia32_abi.h` | 281 | Kernel deadlines convert at entry; device/reference/TSC counts use their own rates. Rate divisions below. |
| `arch/x86_64/vclock.c` | 276, 278, 281, 284, 288, 291, 334, 384, 470, 532, 543, 561, 562, 756, 758 | Kernel deadlines convert at entry; device/reference/TSC counts use their own rates. Rate divisions below. |
| `audio/audio_debug.c` | 17, 27, 258, 309, 310, 328, 404, 405, 534, 539 | Kernel ticks for service/deadlines; durations converted at entry, sample phase retained; HDA hardware clock 24 MHz. Readback divisions below. |
| `audio/hda.c` | 242, 257, 857, 1177, 1369, 1792, 1796, 2506, 2525, 3365, 3388, 3389, 3410, 3420, 3648, 4238, 4361, 4390, 4392, 4400, 4402, 4700, 4705, 4707, 5495, 5541, 5551, 5668, 5690, 5958, 5965, 5969, 5970 | Kernel ticks for service/deadlines; durations converted at entry, sample phase retained; HDA hardware clock 24 MHz. Readback divisions below. |
| `audio/hda.h` | 321, 414, 416 | Kernel ticks for service/deadlines; durations converted at entry, sample phase retained; HDA hardware clock 24 MHz. Readback divisions below. |
| `block/ahci.c` | 1348, 1349, 1364, 1371, 1382, 1384, 1388, 1395, 1396, 1398, 1401, 1405 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `block/ata.c` | 69, 212, 233, 247, 248, 479, 480, 486, 490, 491, 497, 538, 758, 760, 765, 767, 769, 782, 784, 1330 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `block/ata.h` | 31 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `block/block.c` | 181, 240, 250 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `block/block.h` | 84, 352, 540, 550, 583, 590, 591, 597 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `block/diskinfo.c` | 132, 145 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `block/lag.c` | 5, 66, 67, 77, 84, 249, 250, 253, 256, 259, 300, 405 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `block/request.c` | 74, 100, 101, 109, 140, 141, 145, 149, 150, 151, 156, 216, 452, 500, 527, 528, 658, 772, 906, 1196, 1410, 1412, 1417, 1420, 1422, 1423, 1426, 1430, 1442, 1498 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `clock.c` | 2, 3, 4, 63, 72, 77, 81, 85, 88, 99, 100, 135, 136, 143, 155, 156, 275, 276, 277, 295, 296, 325, 329, 335, 349, 365, 371, 375, 487, 494, 499, 501, 504, 505, 514, 651, 661, 894, 949 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `display/blank.c` | 23, 24, 25, 44, 129, 134, 142, 156, 177, 183 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `display/blank.h` | 29, 39 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `dmabuf/fence.c` | 331, 335, 337, 340 | Kernel ticks for coarse deadlines, ages and uptime; milliseconds or nanoseconds converted at entry/readback. Forcewake is monotonic ns. Rate divisions below. |
| `drm/drm.h` | 1766, 2281, 2283, 2298 | Kernel ticks for coarse deadlines, ages and uptime; milliseconds or nanoseconds converted at entry/readback. Forcewake is monotonic ns. Rate divisions below. |
| `drm/drm_atomic.c` | 1455, 1525, 1543, 1599, 1714, 1818, 1861, 1891, 1945, 1947, 2140, 2164, 2175, 2176, 2180 | Kernel ticks for coarse deadlines, ages and uptime; milliseconds or nanoseconds converted at entry/readback. Forcewake is monotonic ns. Rate divisions below. |
| `drm/drm_client.c` | 80, 615, 632, 633, 635 | Kernel ticks for coarse deadlines, ages and uptime; milliseconds or nanoseconds converted at entry/readback. Forcewake is monotonic ns. Rate divisions below. |
| `drm/drm_syncobj.c` | 335, 336, 340, 343 | Kernel ticks for coarse deadlines, ages and uptime; milliseconds or nanoseconds converted at entry/readback. Forcewake is monotonic ns. Rate divisions below. |
| `drm/drm_vblank.c` | 605, 629, 630, 631, 634 | Kernel ticks for coarse deadlines, ages and uptime; milliseconds or nanoseconds converted at entry/readback. Forcewake is monotonic ns. Rate divisions below. |
| `drm/i915/i915.h` | 440, 915, 1195, 1654, 1655, 1658, 2439, 3726, 3774, 4132, 4273, 4281 | Kernel ticks for coarse deadlines, ages and uptime; milliseconds or nanoseconds converted at entry/readback. Forcewake is monotonic ns. Rate divisions below. |
| `drm/i915/i915_debugfs.c` | 338, 341, 617, 629, 630, 631, 1109, 1218, 1337, 1352 | Kernel ticks for coarse deadlines, ages and uptime; milliseconds or nanoseconds converted at entry/readback. Forcewake is monotonic ns. Rate divisions below. |
| `drm/i915/i915_gem.c` | 1592 | Kernel ticks for coarse deadlines, ages and uptime; milliseconds or nanoseconds converted at entry/readback. Forcewake is monotonic ns. Rate divisions below. |
| `drm/i915/i915_gem_execbuffer.c` | 1231, 1340 | Kernel ticks for coarse deadlines, ages and uptime; milliseconds or nanoseconds converted at entry/readback. Forcewake is monotonic ns. Rate divisions below. |
| `drm/i915/i915_gpu_error.c` | 194 | Kernel ticks for coarse deadlines, ages and uptime; milliseconds or nanoseconds converted at entry/readback. Forcewake is monotonic ns. Rate divisions below. |
| `drm/i915/i915_handoff.c` | 181, 184, 208, 209, 210, 212 | Kernel ticks for coarse deadlines, ages and uptime; milliseconds or nanoseconds converted at entry/readback. Forcewake is monotonic ns. Rate divisions below. |
| `drm/i915/i915_hangcheck.c` | 85 | Kernel ticks for coarse deadlines, ages and uptime; milliseconds or nanoseconds converted at entry/readback. Forcewake is monotonic ns. Rate divisions below. |
| `drm/i915/i915_pci.c` | 107, 114, 124 | Kernel ticks for coarse deadlines, ages and uptime; milliseconds or nanoseconds converted at entry/readback. Forcewake is monotonic ns. Rate divisions below. |
| `drm/i915/i915_request.c` | 54, 65, 112, 311, 312, 378, 402, 404, 605, 830, 838, 955, 1251, 1319 | Kernel ticks for coarse deadlines, ages and uptime; milliseconds or nanoseconds converted at entry/readback. Forcewake is monotonic ns. Rate divisions below. |
| `drm/i915/i915_reset.c` | 130 | Kernel ticks for coarse deadlines, ages and uptime; milliseconds or nanoseconds converted at entry/readback. Forcewake is monotonic ns. Rate divisions below. |
| `drm/i915/i915_ring.c` | 411, 618, 626, 655, 656, 657, 659 | Kernel ticks for coarse deadlines, ages and uptime; milliseconds or nanoseconds converted at entry/readback. Forcewake is monotonic ns. Rate divisions below. |
| `drm/i915/i915_uncore.c` | 116, 242, 265 | Kernel ticks for coarse deadlines, ages and uptime; milliseconds or nanoseconds converted at entry/readback. Forcewake is monotonic ns. Rate divisions below. |
| `drm/i915/intel_gmbus.c` | 183, 195, 200 | Kernel ticks for coarse deadlines, ages and uptime; milliseconds or nanoseconds converted at entry/readback. Forcewake is monotonic ns. Rate divisions below. |
| `drm/i915/intel_hotplug.c` | 80, 305 | Kernel ticks for coarse deadlines, ages and uptime; milliseconds or nanoseconds converted at entry/readback. Forcewake is monotonic ns. Rate divisions below. |
| `drm/i915/intel_pps.c` | 46, 49, 52, 69, 70, 71, 73, 77 | Kernel ticks for coarse deadlines, ages and uptime; milliseconds or nanoseconds converted at entry/readback. Forcewake is monotonic ns. Rate divisions below. |
| `drm/vgem.c` | 19 | Kernel ticks for coarse deadlines, ages and uptime; milliseconds or nanoseconds converted at entry/readback. Forcewake is monotonic ns. Rate divisions below. |
| `drm/virtio_gpu.c` | 508, 509, 511 | Kernel ticks for coarse deadlines, ages and uptime; milliseconds or nanoseconds converted at entry/readback. Forcewake is monotonic ns. Rate divisions below. |
| `drm/vkms.c` | 541, 949, 954 | Kernel ticks for coarse deadlines, ages and uptime; milliseconds or nanoseconds converted at entry/readback. Forcewake is monotonic ns. Rate divisions below. |
| `fs/ext2.c` | 639, 648, 656, 660, 680, 686 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `fs/ext2_batch.c` | 633 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `fs/procfs.c` | 1444, 3436, 3438, 3442, 3443, 3453, 3454, 3455, 3521, 3819, 3823, 3892, 5195, 5196, 8552 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `fs/vfs.h` | 1258 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `fs/writeback.c` | 28, 46, 124, 153, 164, 191, 194 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `idle_timer.h` | 6, 17, 19 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `input.c` | 352, 522, 528 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `input.h` | 160, 192, 193 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `io/poll.c` | 218, 227, 229, 243, 385, 443, 477, 560, 564, 570, 575, 576, 579 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `io/timerfd.c` | 46, 49, 50, 66, 67, 87, 159, 186, 190, 294, 297, 402, 407, 415, 458, 462, 486, 530, 598, 600, 686, 690, 692, 695, 757, 760, 800, 807, 840, 842, 886, 887, 903, 921, 923, 935, 1022, 1117, 1144, 1155, 1238, 1240, 1250, 1503, 1509, 1515, 1517, 1519, 1606, 1711, 1726, 2139, 2143 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `io/timerfd.h` | 12, 146, 164, 177, 179, 183, 186, 211 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `iommu.h` | 112 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `ipc/mqueue.c` | 573, 588, 601, 624, 627, 637 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `ipc/sysv.c` | 1114, 1126, 1128, 1195, 1201, 1246, 1968, 1969, 1970, 1980, 1988, 1989, 1991, 2201, 2213, 2217 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `ipc/unix.c` | 199, 912, 918, 920, 937, 994, 998, 1003, 1004, 1890, 1893, 1898, 1902, 1904, 1910, 1911, 1941, 1947, 1952, 1954 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `klog.c` | 59, 152, 172 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `klog.h` | 14 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `kvm/i8254.c` | 23, 27, 71, 78, 80, 82, 90, 94, 96, 98, 101, 103, 177, 189, 191, 197, 200, 229 | Kernel wake ticks converted from ns; guest device counts use guest clock rates, not HZ. |
| `kvm/irq.c` | 748 | Kernel wake ticks converted from ns; guest device counts use guest clock rates, not HZ. |
| `kvm/kvm.h` | 1676 | Kernel wake ticks converted from ns; guest device counts use guest clock rates, not HZ. |
| `kvm/pvclock.c` | 37 | Kernel wake ticks converted from ns; guest device counts use guest clock rates, not HZ. |
| `kvm/vlapic.c` | 172, 362, 1665, 1715, 1718, 1720, 1723, 1725 | Kernel wake ticks converted from ns; guest device counts use guest clock rates, not HZ. |
| `latency.c` | 13, 18, 40, 44, 47 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `latency.h` | 44 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `lib/krandom.c` | 270, 271 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `lib/krandom.h` | 30 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `lib/spinlock.h` | 11 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `mm/filemap.c` | 497, 1220 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `mouse.c` | 146, 191, 195 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `network/driver/ath9k/ath9k.h` | 712, 1052, 1425 | Kernel ticks for worker/calibration/recovery times; missing millisecond boundaries below. Hardware counters and IRQ counts have their own units. |
| `network/driver/ath9k/calib.c` | 1088, 1145 | Kernel ticks for worker/calibration/recovery times; missing millisecond boundaries below. Hardware counters and IRQ counts have their own units. |
| `network/driver/ath9k/intr.c` | 26, 259, 285, 299, 324 | Kernel ticks for worker/calibration/recovery times; missing millisecond boundaries below. Hardware counters and IRQ counts have their own units. |
| `network/driver/ath9k/pci.c` | 201, 270, 342, 466, 489, 497, 503 | Kernel ticks for worker/calibration/recovery times; missing millisecond boundaries below. Hardware counters and IRQ counts have their own units. |
| `network/driver/ath9k/recover.c` | 142, 217, 240, 264 | Kernel ticks for worker/calibration/recovery times; missing millisecond boundaries below. Hardware counters and IRQ counts have their own units. |
| `network/driver/netdev.c` | 498, 503, 524, 599 | Kernel ticks for deadlines, age and credit; MS_TO_TICKS or seconds*TICK_HZ at entry, seconds/ms/USER_HZ at readback. Rate divisions below. |
| `network/driver/netdev.h` | 200, 278 | Kernel ticks for deadlines, age and credit; MS_TO_TICKS or seconds*TICK_HZ at entry, seconds/ms/USER_HZ at readback. Rate divisions below. |
| `network/ip/icmp.c` | 16, 66, 67, 72, 73, 202, 345, 355, 370, 383, 427, 447, 449, 456, 462 | Kernel ticks for deadlines, age and credit; MS_TO_TICKS or seconds*TICK_HZ at entry, seconds/ms/USER_HZ at readback. Rate divisions below. |
| `network/ip/icmp.h` | 74, 98, 103 | Kernel ticks for deadlines, age and credit; MS_TO_TICKS or seconds*TICK_HZ at entry, seconds/ms/USER_HZ at readback. Rate divisions below. |
| `network/ip/ip.c` | 238, 246, 260, 274 | Kernel ticks for deadlines, age and credit; MS_TO_TICKS or seconds*TICK_HZ at entry, seconds/ms/USER_HZ at readback. Rate divisions below. |
| `network/ip/ip.h` | 141 | Kernel ticks for deadlines, age and credit; MS_TO_TICKS or seconds*TICK_HZ at entry, seconds/ms/USER_HZ at readback. Rate divisions below. |
| `network/ip/ipfrag.c` | 31, 131, 286, 293, 315 | Kernel ticks for deadlines, age and credit; MS_TO_TICKS or seconds*TICK_HZ at entry, seconds/ms/USER_HZ at readback. Rate divisions below. |
| `network/ip/pmtu.c` | 15, 20, 37, 76, 96 | Kernel ticks for deadlines, age and credit; MS_TO_TICKS or seconds*TICK_HZ at entry, seconds/ms/USER_HZ at readback. Rate divisions below. |
| `network/ip6/frag6.c` | 35, 141, 422, 430, 454 | Kernel ticks for deadlines, age and credit; MS_TO_TICKS or seconds*TICK_HZ at entry, seconds/ms/USER_HZ at readback. Rate divisions below. |
| `network/ip6/frag6.h` | 94, 96 | Kernel ticks for deadlines, age and credit; MS_TO_TICKS or seconds*TICK_HZ at entry, seconds/ms/USER_HZ at readback. Rate divisions below. |
| `network/ip6/icmp6.c` | 30, 31, 41 | Kernel ticks for deadlines, age and credit; MS_TO_TICKS or seconds*TICK_HZ at entry, seconds/ms/USER_HZ at readback. Rate divisions below. |
| `network/ip6/ip6.c` | 407, 409, 410 | Kernel ticks for deadlines, age and credit; MS_TO_TICKS or seconds*TICK_HZ at entry, seconds/ms/USER_HZ at readback. Rate divisions below. |
| `network/ip6/ip6.h` | 357, 358, 359 | Kernel ticks for deadlines, age and credit; MS_TO_TICKS or seconds*TICK_HZ at entry, seconds/ms/USER_HZ at readback. Rate divisions below. |
| `network/ip6/mld6.c` | 16, 22, 23, 63, 219, 227, 235, 267, 282, 295, 299, 343 | Kernel ticks for deadlines, age and credit; MS_TO_TICKS or seconds*TICK_HZ at entry, seconds/ms/USER_HZ at readback. Rate divisions below. |
| `network/ip6/mld6.h` | 37, 38 | Kernel ticks for deadlines, age and credit; MS_TO_TICKS or seconds*TICK_HZ at entry, seconds/ms/USER_HZ at readback. Rate divisions below. |
| `network/ip6/nd6.c` | 30, 32, 35, 36, 41, 127, 128, 205, 207, 219, 905, 1016, 1616, 1623, 1640, 1641, 1719 | Kernel ticks for deadlines, age and credit; MS_TO_TICKS or seconds*TICK_HZ at entry, seconds/ms/USER_HZ at readback. Rate divisions below. |
| `network/ip6/nd6.h` | 42, 220, 223 | Kernel ticks for deadlines, age and credit; MS_TO_TICKS or seconds*TICK_HZ at entry, seconds/ms/USER_HZ at readback. Rate divisions below. |
| `network/ip6/pmtu6.c` | 18, 24, 41, 84, 105 | Kernel ticks for deadlines, age and credit; MS_TO_TICKS or seconds*TICK_HZ at entry, seconds/ms/USER_HZ at readback. Rate divisions below. |
| `network/ip6/route6.c` | 99, 100, 293, 331, 338 | Kernel ticks for deadlines, age and credit; MS_TO_TICKS or seconds*TICK_HZ at entry, seconds/ms/USER_HZ at readback. Rate divisions below. |
| `network/ip6/route6.h` | 98, 99 | Kernel ticks for deadlines, age and credit; MS_TO_TICKS or seconds*TICK_HZ at entry, seconds/ms/USER_HZ at readback. Rate divisions below. |
| `network/net.h` | 23 | Kernel ticks for deadlines, age and credit; MS_TO_TICKS or seconds*TICK_HZ at entry, seconds/ms/USER_HZ at readback. Rate divisions below. |
| `network/netconf.c` | 802 | Kernel ticks for deadlines, age and credit; MS_TO_TICKS or seconds*TICK_HZ at entry, seconds/ms/USER_HZ at readback. Rate divisions below. |
| `network/netconsole.c` | 19, 139, 488, 523, 1072, 1084, 1436, 1552, 1559, 1563 | Kernel ticks for deadlines, age and credit; MS_TO_TICKS or seconds*TICK_HZ at entry, seconds/ms/USER_HZ at readback. Rate divisions below. |
| `network/netconsole.h` | 152, 170 | Kernel ticks for deadlines, age and credit; MS_TO_TICKS or seconds*TICK_HZ at entry, seconds/ms/USER_HZ at readback. Rate divisions below. |
| `network/socket/netlink.c` | 1174, 1318, 1559, 1561, 1564, 2609, 3083, 3086 | Kernel ticks for deadlines, age and credit; MS_TO_TICKS or seconds*TICK_HZ at entry, seconds/ms/USER_HZ at readback. Rate divisions below. |
| `network/socket/netlink.h` | 112 | Kernel ticks for deadlines, age and credit; MS_TO_TICKS or seconds*TICK_HZ at entry, seconds/ms/USER_HZ at readback. Rate divisions below. |
| `network/tcp/tcp.c` | 107, 111, 114, 115, 116, 120, 122, 133, 441, 1754, 1847, 2171, 2172, 2206, 2216, 4378, 4391, 4392, 4401 | Kernel ticks for deadlines, age and credit; MS_TO_TICKS or seconds*TICK_HZ at entry, seconds/ms/USER_HZ at readback. Rate divisions below. |
| `network/tcp/tcp.h` | 13, 71, 201, 331, 335, 349, 354, 355, 378, 414, 508, 650, 652, 665, 667 | Kernel ticks for deadlines, age and credit; MS_TO_TICKS or seconds*TICK_HZ at entry, seconds/ms/USER_HZ at readback. Rate divisions below. |
| `network/wireless/agg_rx.c` | 105, 176, 185, 211, 229, 242, 344, 380, 427, 456, 468, 469, 510, 557 | Kernel ticks for worker/BSS/data/status times; missing millisecond boundaries and readback are listed below. TSF uses TICK_US; counts and packet windows are unitless. |
| `network/wireless/data.c` | 163, 164, 172, 279, 405, 465, 479 | Kernel ticks for worker/BSS/data/status times; missing millisecond boundaries and readback are listed below. TSF uses TICK_US; counts and packet windows are unitless. |
| `network/wireless/key.c` | 175, 245, 252, 254, 345 | Kernel ticks for worker/BSS/data/status times; missing millisecond boundaries and readback are listed below. TSF uses TICK_US; counts and packet windows are unitless. |
| `network/wireless/mlme.c` | 115, 149, 672, 812, 852, 871, 914, 945, 986, 1042, 1084, 1135, 1176 | Kernel ticks for worker/BSS/data/status times; missing millisecond boundaries and readback are listed below. TSF uses TICK_US; counts and packet windows are unitless. |
| `network/wireless/nl80211.c` | 1081, 1253, 1300, 1665, 2026, 2178, 2564 | Kernel ticks for worker/BSS/data/status times; missing millisecond boundaries and readback are listed below. TSF uses TICK_US; counts and packet windows are unitless. |
| `network/wireless/rx.c` | 187, 357, 608, 619, 789 | Kernel ticks for worker/BSS/data/status times; missing millisecond boundaries and readback are listed below. TSF uses TICK_US; counts and packet windows are unitless. |
| `network/wireless/scan.c` | 44, 51, 99, 197, 220, 252, 316 | Kernel ticks for worker/BSS/data/status times; missing millisecond boundaries and readback are listed below. TSF uses TICK_US; counts and packet windows are unitless. |
| `network/wireless/sta.c` | 365 | Kernel ticks for worker/BSS/data/status times; missing millisecond boundaries and readback are listed below. TSF uses TICK_US; counts and packet windows are unitless. |
| `network/wireless/tx.c` | 579 | Kernel ticks for worker/BSS/data/status times; missing millisecond boundaries and readback are listed below. TSF uses TICK_US; counts and packet windows are unitless. |
| `network/wireless/wireless.h` | 32, 90 | Kernel ticks for worker/BSS/data/status times; missing millisecond boundaries and readback are listed below. TSF uses TICK_US; counts and packet windows are unitless. |
| `network/wireless/work.c` | 2, 17, 60, 136, 173, 187, 189, 190 | Kernel ticks for worker/BSS/data/status times; missing millisecond boundaries and readback are listed below. TSF uses TICK_US; counts and packet windows are unitless. |
| `network/wireless/work.h` | 46, 49, 119 | Kernel ticks for worker/BSS/data/status times; missing millisecond boundaries and readback are listed below. TSF uses TICK_US; counts and packet windows are unitless. |
| `network/wireless/wsim.c` | 41, 242, 356, 679 | Kernel ticks for worker/BSS/data/status times; missing millisecond boundaries and readback are listed below. TSF uses TICK_US; counts and packet windows are unitless. |
| `ntp.c` | 18, 40 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `ntp.h` | 49 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `panic.c` | 229 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `platform.h` | 64, 70, 116 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `proc/auxv.h` | 40 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `proc/coredump.c` | 262, 263, 267 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `proc/futex.c` | 248, 250, 342, 348, 354, 355, 356, 371, 384, 398, 401, 483, 502, 561, 563, 588, 592, 601, 629, 630, 637, 639, 641, 662, 680, 681, 683, 686, 687, 1366, 1389, 1390, 2152, 2161, 2167, 2183, 2186, 2188, 2223, 2236, 2250, 2314, 2324, 2341 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `proc/percpu.h` | 43, 83, 84, 85, 86 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `proc/process.c` | 199, 203, 206, 208, 210, 214, 302, 341, 342, 349, 356, 359, 369, 404, 405, 414, 427, 428, 429, 430, 431, 660, 824, 828, 842, 854, 873, 876, 881, 883, 898, 901, 932, 937, 966, 983, 986, 988 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `proc/process.h` | 161, 202, 337, 363, 397, 405, 409, 418, 469, 475, 727, 732, 945, 993, 1068, 1172, 1303, 1304, 1306 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `proc/sched.c` | 3, 695, 707, 710, 711, 718, 721, 856, 1212, 1252, 1298, 1460, 1464, 1521, 1522, 1523, 1532, 1533, 1534, 1541, 1545, 1550, 1570, 1579, 1580, 1583, 1588, 1600, 1616, 1617, 1620, 1623, 1627, 1628, 1636, 1650, 1653, 1657, 1702, 1707, 1709, 1712, 1746, 1749, 1754, 1775, 1781, 1784, 1787, 1790, 1794, 1854, 1860, 1903, 1992 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `proc/sched.h` | 3, 121, 263, 369, 380, 387, 398, 403, 416 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `proc/smp.c` | 179, 184, 192, 468, 1010, 1013, 1044, 1513 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `proc/smp.h` | 310 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `proc/tgroup.h` | 69, 104, 106 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `proc/wait_queue.c` | 221, 238, 242, 450, 462 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `proc/wait_queue.h` | 82, 85 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `proc/waketime.c` | 107, 113, 114 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `proc/waketime.h` | 46 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `rtc.h` | 68, 73, 74, 75, 92, 94, 95, 177, 181, 183, 184, 185, 188, 263 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `speaker.h` | 65 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `syscall.c` | 509, 518, 602, 631, 636, 642, 681, 936, 937, 949, 953, 965, 1213, 1228, 1245, 1277, 1283, 1324, 1344, 1358, 1367, 1373, 1386, 1391, 1424, 1463, 1489, 1560, 1564, 1568, 1571, 1582, 1598, 1602, 1611, 1615, 1623, 1645, 1651, 1663, 1861, 1863, 1868, 1870, 1871, 1874, 1941, 1969, 1995, 1997, 2910, 2957, 3160 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `syscall_fs.c` | 6369, 6380, 6382 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `syscall_net.c` | 1641 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `syscall_proc.c` | 3594, 3600, 3602, 4254, 6213, 6463, 6470, 6475, 6476, 6527, 6559, 6568, 6569, 10332 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `tick.c` | 3, 65, 68, 71, 73, 78 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `timer.h` | 10, 11, 12, 14, 15, 18, 22, 24, 26, 27, 31, 32, 34, 35, 38, 39, 71, 73, 77, 90, 91, 100, 102, 104, 108, 109, 110, 113, 120, 121, 127, 148, 151, 160, 165, 183, 184, 199, 201, 222, 241, 283, 316, 342, 348 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `tty/tty.c` | 41, 42, 458, 491, 504, 508, 591, 1091, 1097, 1101, 1124, 1126, 1160, 1166, 1168 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `tty/tty.h` | 178, 247 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `usb/msc.c` | 162 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `usb/usb.c` | 565 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `usb/usb.h` | 42 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `vdso.h` | 143 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `vfio/hw.c` | 25, 35, 38, 40 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `virtio/shmem.c` | 441, 444, 445, 451, 453, 456, 471, 479, 481, 487 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `watchdog.c` | 70, 90, 214, 225, 228, 590, 816, 835, 871, 878, 914, 1072, 1086, 1095, 1141, 1159, 1189, 1199, 1210, 1211, 1213, 1219, 1222, 1295, 1297, 1376, 1416, 1435, 1437, 1481, 1505, 1506, 1508 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |
| `watchdog.h` | 54, 58 | Kernel ticks for deadlines/accounting, converted at entry/readback as above; fine clocks in ns, occurrence counters unitless. |

## Missing boundaries to fix

All constants in these rows are milliseconds unless TU or nanoseconds is explicit. Stored stamps remain kernel ticks; conversion belongs where a duration enters arithmetic or a reader reports another unit.

| File | Lines | Missing boundary |
|---|---|---|
| wireless/mlme.c | 149 | arm_in adds ms directly; convert ms, covering authentication, SAE, association, comeback and probe waits. |
| wireless/mlme.c | 679, 828, 1110 | loss_ms added to beacon stamp; convert it. |
| wireless/mlme.c | 764, 765, 781, 782, 787, 795, 796, 812, 838, 842, 843, 871, 915, 1102 | ASK_WAIT, KEY_WAIT, ASK_AGAIN, CSA_LOOK, IDLE_POLL, DS_PROOF and DS_RETRY added to tick stamps; convert each duration. |
| wireless/mlme.c | 986 | CSA beacons_ms added directly; convert beacon duration. |
| wireless/scan.c | 99 | WLAN_BSS_EXPIRE_MS passed as tick age; convert. Dwell/home/away/flush at 225, 237, 262, 275, 305, 318 already convert. |
| wireless/agg_rx.c | 79, 176, 254 | WLAN_BA_TIMEOUT and WLAN_BA_STALE_WAIT compared with tick ages; convert. |
| wireless/agg_rx.c | 269 | BA inactivity timeout in TU (1024 us) divided to ms then added as ticks; convert TU directly to rounded-up ticks. |
| wireless/agg_rx.c | 349, 351, 352, 356, 400, 456, 468, 510 | BA_TX_RESPONSE, twice that response, BA_TX_AGAIN, BA_TX_RETRY and DATA_BA_WAIT; convert durations. |
| wireless/data.c | 465 | DATA_REKEY_WAIT added to tick stamp; convert. rx_wait_most explicitly reports ticks and stays ticks. |
| wireless/key.c | 345 | DATA_REKEY_SETTLE added to tick stamp; convert. renewed and renew_acked remain ticks. |
| wireless/rx.c | 332, 382 | DATA_FRAG_TIMEOUT compared with tick ages; convert. old_until is already a tick deadline once its writer is fixed. |
| wireless/nl80211.c | 1230, 1232, 1261, 1314, 2026, 2188, 2208-2212 | BSS age and inactivity must report ms, LAST_SEEN_BOOTTIME ns, connected duration seconds; BSS expiry, LEAVE_SETTLE and NEW_KEY_RETRY require ms-to-ticks. Scan-result pruning at 2566 is tick-to-tick and is correct. |
| driver/ath9k/calib.c | 1068, 1096, 1137, 1157 | elapsed compares tick spans with ms; queued ANI and selected work interval add ms; convert at these boundaries. All last_* fields stay ticks. |
| driver/ath9k/recover.c | 219, 224, 240 | RX_WINDOW_MS and twice it compare with ticks; receive checks per second divide tick span by TICK_HZ, not 1000; HANG_MS queue converts. |
| driver/ath9k/pci.c | 342, 466 | RFKILL_POLL_MS queue converts. recover.window stamps at 201 and 270 are ticks. |
| wireless/wireless.h, driver/ath9k/ath9k.h, driver/ath9k/intr.c | 32, 137; 712; 28 | Comments incorrectly equate a tick with one ms; state kernel ticks. |

## Rate and fractional-divisor inventory

| File | Uses | Disposition |
|---|---|---|
| arch/x86/pit.c | 19, 169, 332, 487, 620, 899 | Reload divides hardware Hz by kernel Hz; counter reference retains fractional counts using mult=TICK_HZ; frequency bound multiplies by rate. Calibration at 486-487 truncates HZ/4; replace with full product divided by seen*TICK_HZ. Clock resolution at 899 divides ns by device Hz and rounds up. |
| arch/x86/apictimer.c | 204, 668, 676, 682 | Full frequency divided by hz is correct; counter phase uses a predivided second. Change phase scaling to multiply before dividing the full denominator; period is an integer us readout. |
| timer.h, clock.c, block/ahci.c | TICK_NS, TICK_US, RTC_NANOSECONDS_PER_TICK, AHCI_TICK_US | Integer tick length at fixed 250 Hz, exact 4000000 ns and 4000 us. No quarter-HZ division. MS_TO_TICKS splits seconds and remainder, rounding up after multiplication. |
| tick.c | 65, 71, 73, 78 | tick_span rounds subsecond up; tick_clock divides whole seconds first and multiplies remainder before division; tick_centisecs multiplies before division with upward rounding. |
| tty/tty.c | 42, 504, 1097 | Predivided HZ/10 for VTIME; multiply complete VTIME in ms before conversion. Backup fifth-second interval becomes MS_TO_TICKS(200). |
| network/socket/netlink.c, network/ip6/nd6.c | 1564; 1623 | HZ/10 service interval becomes MS_TO_TICKS(100); nd6 retransmission 219 becomes MS_TO_TICKS(RETRANS_TIMER_MS). |
| network/tcp/tcp.c | 107, 248, 1754 | ISN uses predivided 250000/HZ; scale tick count before dividing. Refusal diagnostic predivides 1000/HZ; convert full tick span to ms. |
| audio/audio_debug.c | 309-310, 404-405, 534, 539 | Sample output retains remainder across calls; timeout multiplies before divide. Diagnostic milliseconds predivide 1000/HZ; convert the complete tick span. |
| drm/i915/i915_debugfs.c, arch/x86/iommu/hw.c | 341; 187 | Millisecond readbacks predivide 1000/HZ; convert the complete span. |
| fs/procfs.c | 3442-3443, 5195-5196, 8552 | Seconds plus remainder, TCP_RTO*1000/HZ and bound INT_MAX/HZ are correct; no fractional divisor. |
| watchdog.c | 214, 228, 590, 1086, 1295, 1297 | Seconds readback and limits divide full ticks by HZ; sample multiplies threshold*HZ before /5; PMU multiplies frequency*sample ticks before /HZ. |
| arch/x86/ps2mouse.c | 1414, 1416 | Quotient and remainder compare elapsed ticks with whole seconds, preserving strict greater-than; correct. |
| drm/i915/intel_pps.c, i915_handoff.c, intel_gmbus.c | 46, 69; 208; 195 | Multiply tick stamp by 1000 before /HZ; multiply durations by HZ before upward /1000 or /1000000; correct. |
| network/socket/netlink.c, ip6/nd6.c, ip6/route6.c | 1174; 905; 100, 293 | Address ages and lifetimes seconds convert by full HZ, route readback rounds up; correct. |
| syscall.c, syscall_proc.c, proc/coredump.c, klog.c, ipc/unix.c | clock/getrusage/itimer/uptime/diagnostic/socket time readbacks | Whole seconds divide tick count by HZ; subsecond remainder multiplies by TICK_US/TICK_NS. USER_HZ stays 100 and uses tick_clock; correct. Real itimer timeval truncation and alarm rounding are separate review fixes. |
| kvm/pvclock.c, arch/x86/pmu.c, arch/x86_64/vclock.c, arch/x86/tsc.c | TSC Hz, kHz, mult/shift and calibration | counts-per-tick multiplied by HZ before /1000; clock rate uses TICK_NS*shift/counts, TSC time windows multiply before dividing TICK_US; correct. |
| ntp.c, clock.c | NTP_INTERVAL_FREQ and fixed-point tick length | Full fixed-point second divided by interval frequency; USER_HZ input scaled to kernel tick. Fraction retained by clock. |

This inventory was posted before code changes. The changes that follow it: correct these boundaries and the scheduler and itimer findings, strengthen the forcewake publication stub, then compile both kernels and the changed host fixture only.
