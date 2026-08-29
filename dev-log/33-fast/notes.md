# Milestone 33 notes

Deviations from `plan.md`, measurements, and defects found but not fixed
in the phase that found them.

## The baseline

The last complete run of the current tree, 4877s (81.3 min) across 44
suites. Adopted rather than re-run: the only commits between it and the
start of this milestone are this plan, so a fresh run would have spent 81
minutes reproducing a number already in hand. Per suite, slowest first:

* e1000          915s
* account        436s
* sealed         327s
* dhcp           316s
* selfbuild      227s
* menu           160s
* procfiles      152s
* install        152s
* drill          146s
* raw            144s
* netif          139s
* stress-ahci    135s
* stress         135s
* tcc            115s
* bootpart       95s
* default        94s
* addressed      94s
* noentropy      91s
* shutdown       86s
* make           78s
* packet         66s
* ping           65s
* cred-persist   60s
* exitline       59s
* login          54s
* bootline       54s
* authority      49s
* persist-ahci   48s
* persist        47s
* subset         42s
* host           39s
* dgram          36s
* ssh            29s
* disk-q35       24s
* disk-ahci      24s
* disk           24s
* style          22s
* grub           22s
* diskdiag       20s
* diskpolicy     17s
* crash          13s
* netconsole     10s
* metal          10s
* reset          6s

## Phase 2: the collisions

Measured rather than run whole: the touched suites, then two batches.

* Eight suites that share images or ports (`dhcp`, `account`, `diskdiag`,
  `metal`, `drill`, `procfiles`, `login`, `sealed`): 1461s in series, 435s
  together, 8 of 8 passing. The parallel figure is `account`'s own 436s, so
  those eight now cost what the longest of them costs.
* The sixteen-suite batch that failed before: 99s, 16 of 16, where it was 98s
  with `disk-ahci` and `disk-q35` failing on the shared disk.

What changed:

* `run-disk-q35` staged and booted `disk-test-ahci.disk`, the same path
  `run-disk-ahci` stages. It has its own now. It also invoked `$(HARNESS)`
  without depending on it, which worked only because another suite had built
  it first.
* `rach2-metal-image` and `rach2-recovery-image` were phony, so they restaged
  their image on every invocation. Three suites that only boot the metal image
  each restaged it. Both are real file targets now, with the components as
  order-only prerequisites, which is the shape `$(TEST_IMAGE)` already used.
* `run-dhcp` restaged the metal image and `run-account` restaged the os-simple
  image, each of which another suite boots. Both now depend on the image
  instead of writing it.
* Every forwarded port is named in one place in the Makefile. Two pairs
  collided: 2224 for `dhcp` and `diskdiag`, 2298 for `procfiles` and `drill`.
  `procfiles` also hardcoded its port rather than taking it as an argument, so
  the number lived in two files; it takes it as an argument now, as every other
  suite does.

## Phase 3: the pool

4877s to 1205s, all 44 passing. Repeated three times at 1203s, 1202s and
1205s, so the result does not depend on how the workers happened to interleave.
Eight workers, `style` and `host` outside the pool, one `make -j prepare-all`
first, which costs 1.3s warm.

`e1000` is now the critical path on its own: 917s of a 1205s run. Phase 5 is
about that.

Two things worth keeping:

* The leak census had to change shape. Listing every QEMU on the host works
  only while one suite runs at a time; with eight running, each would see the
  other seven and call them its own. Instead a shim named `qemu-system-i386`
  goes first on `PATH`, writes its own pid to the ledger the current suite
  owns, and execs the real binary, so the pid it recorded is the machine. The
  harness reaches QEMU through `execvp` and the shell suites through their own
  `PATH` lookup, so both are covered without either knowing about it.
  Attribution is proved by putting the `crash` defect back and requiring the
  run to name `crash` and no neighbour.

* `run_pool` first returned its failed list through a command substitution,
  which runs the function in a subshell, so a failing suite's replayed output
  went to that subshell rather than to the person reading it: a leaking `crash`
  reported `failed: crash` and nothing else. Found by the attribution proof,
  which could not see the message it was looking for. The result is a variable
  now.

`tests/run` also takes several names, which is how that proof was run cheaply
and is worth having on its own: the suites a change touches, at the cost of the
slowest rather than the sum.

## Phase 5, taken before phase 4

The plan puts the worker sweep first, but with `e1000` at 917s of a 1205s run
the sweep would have measured that one suite rather than the workers. The
plan's own rule is to measure rather than estimate, so the order was swapped.

`e1000` alone: 917s to 336s. The whole run: 1205s to 727s, 44 of 44. The
critical path is now `account` at 434s.

Coverage is unchanged. All nine members still run on the second card; they run
together instead of one after another. What stopped them being schedulable was
that each wrote the same files and forwarded the same ports as its first-card
twin, so the card now picks the ports and tags every file a run writes. Both
cards' full network suites were then run at once: 10 of 10, in 340s, which is
`e1000`'s own 339s, so the first-card suites cost nothing beside it.

The inner job count is fixed at 4 rather than inherited. This suite is already
one worker of an outer pool, and an unbounded `-j` would multiply the two.

## Phase 4: the worker count

Measured on this ten-core machine, one run per count. Three runs each was not
worth 2.4 hours: the variance was already measured in phase 3 at three seconds
across three runs.

* 4 workers: 1286s
* 8 workers: 728s
* 12 workers: 587s
* 16 workers: 527s
* 20 workers: 499s
* 24 workers: 500s

It stops improving at twenty. Four, which the first draft of the plan called a
measured ceiling without having measured it, is two and a half times slower
than the knee. The reason is plain in the earlier numbers: a guest costs about
a fifth of a processor, so ten cores are nowhere near busy at four machines,
which is why the old serial suite used three percent of one.

The default is twice the processor count rather than the constant twenty, so
it is right on a machine that is not this one. Here that computes to twenty and
the run takes 499s, matching the sweep.

The floor is `account` at 434s. Nothing but phase 6 can go below it.

## Phase 6: the waits, and what they uncovered

The whole run is 464s, repeated at 465s and 465s, 44 of 44. From the 4877s
baseline that is a factor of ten and a half.

Three changes, one of them not in the plan.

* `account`, the critical path, went from 434s to 398s by waiting for the
  console to go quiet before the first line instead of counting to twelve.
  Only the first line: the per-line pacing cannot become a condition wait,
  and finding out why cost a failed run. The suite types AHEAD of the prompts
  that read it, and the line discipline queues what is typed until each prompt
  arrives, which is what lets a password answer a prompt no shell has printed.
  A feeder that waits for quiet is waiting for the prompt, and `passwd` reads
  three prompts back to back with nothing printed between them, so the queue
  was empty when it asked and all three passwords landed at the shell as
  `-sh: testeepw1: not found`. That is written above `feed_settle` now.

* The pool starts the longest suite first, from what each took last time,
  recorded under `build/`. The three longest sat at registry positions 20, 32
  and 40 of 44, so they began late and became a tail nothing overlapped: a
  pool finishes when its longest member does. A suite with no record sorts
  first, since an unknown cost is safest treated as a large one. This was
  worth about 30 seconds and, more usefully, it made the next defect appear.

* The credential generators raced, which is what the scheduling change
  exposed. Thirty-two recipe lines called `gen_demo_key.sh` and
  `gen_demo_password.sh`. Both are idempotent and do nothing when their output
  is present, which is enough while one thing runs at a time; with `make -j`
  and a pool, two mint a key at once and collide on the temporary file, which
  reads as `mv: authorized_keys.tmp: No such file or directory`. They are file
  targets now, built once, and the sixteen run targets that read a credential
  depend on them. This was the cause of both failures seen under scheduling,
  `selfbuild`'s included: it passed alone in 230s and failed in 52s under
  load, which was a password file read while another suite was replacing it.

Both defects predate this milestone. Serial execution and registry order hid
them, as they hid the `crash` suite's leaked machines.

## Phase 7: completion

Cold from `make distclean`, which compiles the five vendored projects and Doom
before a machine boots: 618s. Warm: 465s. Both 44 of 44, no machine surviving.

The plan's first draft asked for `distclean`, then `make`, then one
`tests/run all`, and called the result cold and warm. That sequence produces
two warm numbers: the standalone `make` does the building, so the run that
follows it has nothing left to build. The cold number is the run that builds
what it needs, so the standalone `make` is left out.
