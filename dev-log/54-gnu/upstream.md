# Upstream faults of milestone 54

Faults this milestone found in another project's code, one section each, in the format of [`dev-log/upstream.md`](../upstream.md), which indexes them.

## BusyBox time reports four times the peak memory

* Project: BusyBox 1.36.1, `vendor/busybox/busybox-1.36.1.tar.bz2`.
* Fails: `time -v` prints a maximum resident set size four times the kernel's on a machine with 4 KiB pages: `%M` passes `ru_maxrss`, which Linux gives in KiB, through `ptok`, which converts pages to KiB (`miscutils/time.c:123-125, 283-284`).
* Found: by 2026-10-02, at the second sitting on the Lenovo, in milestone 54; the notes say upstream's `time` does the same.
* Reproduction: run `busybox time -v` on a program of known peak memory and compare with `getrusage`'s `ru_maxrss`.
* Evidence: `dev-log/54-gnu/notes.md`, the paragraph "What the sitting found that is not a fault of Rach2"; commit `19c659997`.
* Rach2: nothing.
* Status: unreported.
* Next: check current BusyBox's `miscutils/time.c`; the fix would print `ru_maxrss` as it is on Linux.
