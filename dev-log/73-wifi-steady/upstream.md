# Upstream faults of milestone 73

Faults this milestone found in another project's code, one section each, in the format of [`dev-log/upstream.md`](../upstream.md), which indexes them.

## BusyBox ping stops at an interval of 0

* Project: BusyBox 1.36.1, `vendor/busybox/busybox-1.36.1.tar.bz2`.
* Fails: `ping -i 0` sends one request and no more, and `-w` never ends it: after each send ping arms `ITIMER_REAL` with `it_value` set to the interval, and a value of 0 disarms the timer that sends the next request and checks the deadline (`networking/ping.c:532-541, 973`).
* Found: by 2026-11-06, in milestone 73, while building ping for ordinary accounts; the machine is not recorded.
* Reproduction: `busybox ping -i 0 -w 3 HOST`.
* Evidence: the description at the head of `vendor/busybox/patches/0009-ping-fall-back-to-datagram-socket.patch`; commit `0fc2ad58d`.
* Rach2: the patch makes an interval of 0 the shortest the timer takes.
* Status: unreported.
* Next: check current BusyBox's `networking/ping.c`; send the interval fix apart from the patch's datagram-socket change.
