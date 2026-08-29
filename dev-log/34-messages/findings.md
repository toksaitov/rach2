# Findings to act on in milestone 34

Two sets. The first came from a review of the test suite as a whole, run after
milestone 33 made the suite fast enough to run often. The second is the state
of the kernel's own messaging, which the first set keeps running into: several
tests cannot check what they claim because the kernel says things in places
nothing reads.

## Tests that do not check what they claim

* The kernel runs self-tests at every boot and prints `[FAIL]` when one fails,
  in 23 places in `rach2/kernel.c`. Nothing in `tests/system/runner/runtests.c`
  looks for that word: its four uses of `FAIL` are its own probe reporting. A
  boot whose self-tests fail still passes the default suite.
* `subset` boots once per `rach2.no*` device-disable option and asserts only
  that the machine reaches `Type 'help'` (`Makefile`, `run-subset`). A kernel
  that ignored every one of those options would pass.
* `reset` asserts the pre-reset announcement and rejects the fallback
  diagnostic, but the harness kills the machine itself and passes on the
  token, so a kernel that prints `reboot: restarting` and then hangs without
  touching any reset mechanism passes (`tests/system/suites/reset.sh`).
* `addressed` re-runs all 63 probes with an address given on the boot line, but
  no probe checks that the address in force came from there rather than from
  the compiled default.

## Coverage that is missing

* The kernel's fixed limits are never reached with the resources still held:
  16 processes, 16 descriptors per process, 16 sockets, 16 transmission
  control blocks. No test fills one, confirms the next attempt is refused with
  the right error, releases one, and confirms recovery.
* No test fills a KRFFS file system and checks the failed create, grow and
  truncate paths, then deletes and allocates again. `bootpart` checks a raw
  device boundary, which is not the allocator.
* The AHCI driver has no host model, so its reset, timeout, fatal-status and
  port-recovery paths are untested. Milestone 10's own record says its worst
  defects were in paths QEMU does not exercise.
* Audio has no automated functional test. The suite proves only that a boot
  with `rach2.noaudio` reaches a shell, which does not prove the option was
  honoured.
* TCP has one host test, receive-window reopening, although milestone 12 found
  its most serious defects in the closing states and in send-side backpressure.

## Work that is duplicated

* `e1000` re-runs nine complete suites on the second card. Keeping `ping`,
  `packet`, `dgram`, `netconsole` and `ssh` plus the three card-selection
  boots would cover the device without repeating the stack.
* `procfiles` runs the 1130-line `procprobe` three times to read one value out
  of each run.
* `stress-ahci` repeats the cache-pressure workload `stress` already runs.
* `addressed` repeats all 63 probes to prove something no probe checks.

## Dead and stale

* `tests/echoprobe.c` is an empty file.
* `tests/Readme.md` still describes `harness.py`, which milestone 30 replaced
  with `harness.c`.
* `tests/host/Readme.md` says 36 host tests; there are 38.
* `tests/ttyprobe.c` names `drive_tty.py`, which does not exist.
* Five guest probes are built and staged but run by nothing: `winprobe`,
  `sockettest`, `cowprobe`, `ttyprobe` and `dnstest`. `winprobe` reproduces a
  kernel panic that actually happened.

## Structure

* All 44 registry entries are marked as members of `all`, so that field
  encodes no policy. There is no small, fast set to run while working. Now
  that `all` takes eight minutes rather than eighty-one, a core tier of about
  ninety seconds would be worth having.
* `tests/run all` builds everything before it checks any suite's runtime
  tools, so a missing `qemu-system-i386` is reported after the build.
* `RACH2_WORKERS` is not validated. Zero makes the pool wait forever.
* The `e1000` suite counts as one worker while starting four inner jobs, so
  the pool's accounting understates what is running.
