# Milestone 33: a fast suite

`tests/run all` went from 81 minutes to under 8, and not one check was weakened to do it.

## What was wrong

The suite was not slow. It was asleep, and it was serial.

A boot to the kernel debug shell takes 4.76 seconds and a fifth of one processor. The machine has ten cores and 24 GB, and one emulated machine costs 71 MB. During a complete run, one QEMU process sat at about three percent of one processor while the other nine cores did nothing. Measured on `tests/run ping`: the success token arrived at 11 seconds and the harness exited at 66, because `wait=SECS` is an unconditional drain that never asks whether what it is waiting for has already happened.

## What was done

Four things, in the order the measurements asked for rather than the order first planned.

The suites run through a pool. `tests/run all` builds everything once through a single `make -j prepare-all` and then runs the suites concurrently, with `style` and `host` outside the pool because they share `/tmp/rach2_verify` and `style` builds the kernel with its own flags. `tests/run NAME NAME...` pools a named list, which is the common case of running the handful of suites a change touches.

The second card's suites became ordinary neighbours of the first card's. `e1000` re-runs nine suites on the other card and was 917 seconds of a 1205 second run, entirely on the critical path. Trimming it was considered and rejected: the host model proves the driver in isolation but does not run the stack through the device, so dropping the second-card `netif`, `raw`, and `dhcp` would have dropped live interface reconfiguration, the raw socket, and packet-socket address reconfiguration on that card. Instead the card picks the forwarded ports and tags every file a run writes, so both cards' suites run at once. Full coverage, 917 seconds to 336.

The worker count is twice the processors, measured rather than assumed. On this machine: 1286 seconds at four workers, 728 at eight, 587 at twelve, 527 at sixteen, 499 at twenty, 500 at twenty-four. Four, which an earlier draft called a measured ceiling without having measured it, is two and a half times slower than the knee.

The pool starts the longest suite first, from what each took last time. The three longest sat at registry positions 20, 32 and 40 of 44, so they began late and became a tail nothing overlapped.

## What it uncovered

Going faster found three defects that serial execution had hidden for as long as the suites had existed. None was caused by this work.

The `crash` suite leaked three machines every run. `qemu_disk` was a shell function backgrounded with `&`, so `$!` held the subshell's pid and never QEMU's; killing the subshell left QEMU to init. Fifteen were alive on one development machine, the oldest three days old. The function `exec`s now, and `tests/run` fails any suite that leaves a machine behind.

The credential generators raced. Thirty-two recipe lines called `gen_demo_key.sh` and `gen_demo_password.sh`; both are idempotent and do nothing when their output exists, which is enough while one thing runs at a time. Under `make -j` and a pool, two mint a key at once and collide on the temporary file. This was the cause of both failures seen once scheduling changed, `selfbuild`'s included: it passed alone in 230 seconds and failed in 52 under load, reading a password file while another suite replaced it.

Several suites wrote each other's files. `disk-q35` staged and booted the same disk as `disk-ahci`; two image builders were phony and restaged on every invocation, so three suites that only boot the metal image each rebuilt it; `dhcp` restaged the metal image and `account` the os-simple image, both of which other suites boot; and two pairs of suites forwarded the same host port.

## What was not done, and why

`pump` was not changed. A global early exit on the success pattern would stop a recipe whose token appears early from ever receiving the input its later steps need, and it would silently defeat every timed absence assertion: `reset` prints `reboot: restarting` before it attempts the reset, and the six seconds after it are what let the forbidden fallback appear.

`account`'s per-line pacing was not converted. The suite types ahead of the prompts that read it, and the line discipline queues what is typed until each prompt arrives, which is what lets a password answer a prompt no shell has printed. A feeder that waits for quiet is waiting for the prompt, and `passwd` reads three prompts back to back, so the queue was empty when it asked and the passwords landed at the shell. Only the wait before the first line became a condition, which is a genuine wait for the machine to boot.

No dynamic port allocation, no per-worker copy of the repository, no container, no new language runtime, and no reduction in the number of boots.

## Verification

Cold from `make distclean`, which compiles the five vendored projects and Doom before a machine boots: 618 seconds. Warm: 465, repeated at 464 and 465. All 44 suites pass in every run and no machine survives one. The leak guard was proved by restoring the `crash` defect and requiring the run to name `crash` and blame no neighbour; `account` was proved by a marker the machine never prints.
