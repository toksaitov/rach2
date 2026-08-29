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
