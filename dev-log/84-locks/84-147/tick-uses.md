# The tick-rate audit

The inventory covers every direct `TICK_HZ` use in the kernel after the conversion. The disposition applies to each listed line. `84-147.md` gives the additional searches for tick counts that do not name the rate and the timing rows read without running them.

| File | Lines | Disposition |
|---|---|---|
| `rach2/kernel.c` | 581, 1259, 1263, 1264, 3850 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/arch/x86/alps.c` | 2707 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/arch/x86/apictimer.c` | 39, 71, 436, 437 | Calibration and reference arithmetic use counts per actual tick. Physical periods, counts and phases scale together; fine events are independent. |
| `rach2/sys/arch/x86/iommu/fault.c` | 370 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/arch/x86/iommu/hw.c` | 187 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/arch/x86/itco.c` | 578, 580, 720, 727 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/arch/x86/pit.c` | 19, 168, 169, 620 | Calibration and reference arithmetic use counts per actual tick. Physical periods, counts and phases scale together; fine events are independent. |
| `rach2/sys/arch/x86/pit.h` | 31 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/arch/x86/platform.c` | 359, 371, 381, 430 | Calibration and reference arithmetic use counts per actual tick. Physical periods, counts and phases scale together; fine events are independent. |
| `rach2/sys/arch/x86/pmu.c` | 282 | Calibration and reference arithmetic use counts per actual tick. Physical periods, counts and phases scale together; fine events are independent. |
| `rach2/sys/arch/x86/ps2kbd.c` | 191 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/arch/x86/ps2mouse.c` | 1414, 1416 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/arch/x86/speaker.c` | 168 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/audio/audio_debug.c` | 309, 310, 404, 405, 534, 539 | Upward time conversion and multiply before dividing sample counts; fractional sample phase is retained. |
| `rach2/sys/audio/hda.c` | 3410, 5958, 5969, 5970 | Elapsed jack countdown, upward 250 ms conversion, one-second beep idle and millisecond active-stream service. |
| `rach2/sys/block/ahci.c` | 1349 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/block/ata.c` | 1330 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/block/lag.c` | 405 | Convert the complete hundredths value with tick_centisecs, rounding up. |
| `rach2/sys/block/request.c` | 216, 1498 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/clock.c` | 99, 100 | NTP interval frequency and fixed-point tick length scale together; USER_HZ stays 100. Fine interpolation uses the rate, not tick invocation count. |
| `rach2/sys/display/blank.c` | 25, 177 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/drm/i915/i915.h` | 1641 | The actual rate scales the interval; fine events retain their own nanosecond deadline. |
| `rach2/sys/drm/i915/i915_debugfs.c` | 341 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/drm/i915/i915_handoff.c` | 208 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/drm/i915/i915_pci.c` | 124 | The actual rate scales the interval; fine events retain their own nanosecond deadline. |
| `rach2/sys/drm/i915/i915_uncore.c` | 223 | Forcewake uses monotonic nanoseconds; an armed release requests millisecond service and publishes its deadline after the register lock. |
| `rach2/sys/drm/i915/intel_gmbus.c` | 195 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/drm/i915/intel_pps.c` | 46, 69 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/drm/vkms.c` | 949, 954 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/fs/procfs.c` | 3442, 3443, 5195, 5196, 8552 | Seconds and remainders use TICK_HZ and TICK_NS/TICK_US. User clock_t uses tick_clock. Real itimers use nanoseconds. |
| `rach2/sys/idle_timer.h` | 17, 19 | One default rate; upward milliseconds, 100 Hz user units and 4 ms ticks. The two literal clock models explicitly compile at 1000 Hz. |
| `rach2/sys/input.c` | 522, 528 | Fine polling uses elapsed monotonic milliseconds. Coarse service queries retain the separate millisecond event. |
| `rach2/sys/io/poll.c` | 243 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/io/timerfd.c` | 50, 2143 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/ipc/unix.c` | 1911 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/klog.c` | 172 | Seconds and remainders use TICK_HZ and TICK_NS/TICK_US. User clock_t uses tick_clock. Real itimers use nanoseconds. |
| `rach2/sys/kvm/pvclock.c` | 37 | Calibration and reference arithmetic use counts per actual tick. Physical periods, counts and phases scale together; fine events are independent. |
| `rach2/sys/network/driver/ath9k/pci.c` | 503 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/network/driver/netdev.c` | 503, 599 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/network/ip/icmp.c` | 67, 73 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/network/ip/ipfrag.c` | 131, 315 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/network/ip/pmtu.c` | 15 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/network/ip6/frag6.c` | 141, 454 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/network/ip6/icmp6.c` | 31 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/network/ip6/mld6.c` | 22, 23 | Milliseconds round up; protocol seconds multiply by the rate. TCP ISN time scale remains 250000 per second. |
| `rach2/sys/network/ip6/nd6.c` | 35, 36, 41, 219, 905, 1623, 1719 | Milliseconds round up; protocol seconds multiply by the rate. TCP ISN time scale remains 250000 per second. |
| `rach2/sys/network/ip6/pmtu6.c` | 18 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/network/ip6/route6.c` | 100, 293 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/network/netconsole.c` | 139, 1436, 1552, 1563 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/network/socket/netlink.c` | 1174, 1564, 2609, 3083 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/network/tcp/tcp.c` | 107, 1754, 2171, 2172, 4378, 4391, 4392, 4401 | Milliseconds round up; protocol seconds multiply by the rate. TCP ISN time scale remains 250000 per second. |
| `rach2/sys/ntp.c` | 18, 40 | NTP interval frequency and fixed-point tick length scale together; USER_HZ stays 100. Fine interpolation uses the rate, not tick invocation count. |
| `rach2/sys/platform.h` | 64 | Second-based lifetimes or bounded service waits scale by the actual rate; milliseconds use MS_TO_TICKS or multiply before division. |
| `rach2/sys/proc/coredump.c` | 267 | Seconds and remainders use TICK_HZ and TICK_NS/TICK_US. User clock_t uses tick_clock. Real itimers use nanoseconds. |
| `rach2/sys/proc/sched.c` | 1653, 1749 | Elapsed accounting and steal time use TICK_NS; load sample is five seconds plus one tick. Slice events use nanoseconds. |
| `rach2/sys/syscall.c` | 681, 937, 1245, 1424, 1463, 1651, 1874, 2957 | Seconds and remainders use TICK_HZ and TICK_NS/TICK_US. User clock_t uses tick_clock. Real itimers use nanoseconds. |
| `rach2/sys/syscall_proc.c` | 3600, 3602 | Seconds and remainders use TICK_HZ and TICK_NS/TICK_US. User clock_t uses tick_clock. Real itimers use nanoseconds. |
| `rach2/sys/tick.c` | 65, 71, 73, 78 | Multiply before division; seconds and remainders avoid overflowing CPU-time conversion; hundredths round up. |
| `rach2/sys/timer.h` | 11, 12, 14, 15, 18, 24 | One default rate; upward milliseconds, 100 Hz user units and 4 ms ticks. The two literal clock models explicitly compile at 1000 Hz. |
| `rach2/sys/tty/tty.c` | 42, 1097, 1166, 1168 | VTIME tenths and console backup fifths divide 250 exactly; service intervals stay in ticks. |
| `rach2/sys/usb/usb.c` | 565 | Fine polling uses elapsed monotonic milliseconds. Coarse service queries retain the separate millisecond event. |
| `rach2/sys/watchdog.c` | 214, 228, 590, 1072, 1086, 1210, 1295, 1297, 1481, 1505 | Seconds and accounting ticks scale together. Watchdog samples remain absolute monotonic events and PMU counts use the actual rate. |

There are 154 direct occurrences in 62 files, including declarations and comments. The search also covered `TICK_NS`, `TICK_US`, `kernel_ticks`, `rtc_ticks_now`, millisecond divisors, and rate-independent numeric tick constants.
