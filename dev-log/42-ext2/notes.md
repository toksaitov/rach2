# Milestone 42 notes

## Phase 1 record

Arrival on 2026-09-02 at `cbf7d80 Close milestone 41's record` with a
clean tree. The twelve commits before it:

```
cbf7d80 Close milestone 41's record
2c71a28 Keep the default boot's transcript
b2336d9 Read a failed kernel self-test summary at its level and score the group in one function
e855085 Take the clock check's walk up to three times against a late tick
42777c2 Record milestone 41's review round
58192f8 Correct the hub claims the closing review found in the Readmes
9aed48c Fix what the closing review found in the hub tree and its tests
3a88dc4 Keep a transfer event on the ring when the completion queue is full
896a944 Draft milestone 41's record and index entry
16b57ac Record phase 5 of the hub milestone
369d82f Build /proc/usb's path where the records' path is built
547dfd8 Add the hub checks to the acceptance script and rehearse them behind a hub
```

### The review units

Three rounds: after phase 3, after phase 5, and before the sweep, on
Codex through its rescue agent, units of at most six files and ten
minutes, at most two units in a round running at once and none starting
another:

* unit A, the driver: `ext2.c`, `ext2.h`, `ext2.Readme.md`, and after
  phase 4 the writer's half of `ext2.c` read on its own;
* unit B, the walker and the syscalls: `vfs.c`, `vfs.h`, `vfs.Readme.md`,
  the syscall table and `vfsprobe.c`;
* unit C, the tools: `mkext2.sh`, `stageimage.krffs.c`, `setidcheck.c`,
  `mkdisk_grub.sh`, `preflight.sh`, `doctor.sh`;
* unit D, the tests: `ext2_hosttest.c`, `ext2.sh`, the matrix script,
  `metal_accept.sh`;
* unit E, the port and recovery: `build_e2fsprogs.sh`, the recovery
  manifest and its `init`, `vendor/e2fsprogs/Readme.md`;
* unit F, the reader-facing documents: `rach2/Readme.md`, `Readme.md`,
  `tools/Readme.md`, `krffs/Readme.md`, `utils/Readme.md`,
  `tests/system/Readme.md`.

The round after phase 3 runs A and B; after phase 5, A, C and E; before
the sweep, D and F, with A once more over the whole driver.

## Open items

* None yet.
