# Milestone 32 notes

Deviations from `plan.md` and defects found but not fixed in the phase that
found them. Each entry says what and why.

## Phase 1

* `AGENTS.md` ends with "Readme.md has the fresh Mac bootstrap instructions".
  Milestone 31 deleted that script and moved machine setup to
  `tools/Readme.md`, so the sentence names a procedure that is not there. Left
  for phase 5, which is where this milestone's document corrections land.

## Phase 3

* `rach2/sys/lib/kstring.c` cannot be compiled natively at `-O1` or above
  without a sanitizer. It defines `memset` as a wrapper around `k_memset`, and
  the native compiler recognises `k_memset`'s loop as the memset idiom and
  emits a call to `memset`, which is the wrapper: the pair becomes an infinite
  loop. The host suite always passes `-fsanitize=address,undefined`, whose
  interceptor breaks the recursion, so no suite run has ever shown this. It
  appeared here only because a mutation script compiled a host test without
  those flags. The e1000 driver therefore keeps to plain byte loops and lists
  no `host-deps:`, as `rtl8168.c` and its model already do. Any future host
  test that names `kstring.c` as a dependency inherits the same constraint.
