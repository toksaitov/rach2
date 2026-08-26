# Milestone 30 checkpoint

This file holds only the resume state for the item in progress. Replace its contents instead of appending.

## Position

* Active phase: 10, finishing. Python is gone from the tree: the three scoped searches the plan names come back empty (no tracked project `.py` outside the vendor trees and `dev-log/`, no `python3` invocation in any current script, Makefile or test source, and no `.py` path or `python3` command in any current document). `python3` has left the required tool set in `tools/setup/preflight.sh`, the `doctor.sh` report, and all 42 records in `tests/registry`.
* Item in progress: `tests/run all`, the complete suite set, running now. When it passes, phase 10 is done and phase 11 is the completion record.
* Still to do in phase 10: regenerate the prose baseline in its own commit and read the report. Converting the checkers moved about 17000 words of prose from the shell and Python population into the C one, so every C rate moved with them; `prose_report.sh --baseline` prints the movement, and `--fixture` is already regenerated with its upstream counts unchanged.
* The rebuilt measurements are recorded in `notes.md`: cold `make` 119 s serial and 34 s at `-j8` against a 60 s phase 1 baseline, no-op `make` 1 s against 3 s.

## Phase record

* Phases 1 to 9 complete; see e4d3187 through 6edc902.
* Phase 10's conversions, each proved by differential before its Python original was removed:
  * 94c4bf6, `tools/inspect/cfgcheck.c`: the GRUB core's embedded configuration and the sealed disk's fixed address, 22 selftest cases and identical output on the real files and every refusal fixture.
  * d2df25a, `tools/verify/prosecheck.c` with `prose_report.sh`: comment width, the banned vocabulary, the watched-phrase counts and the dev-log index. 24 selftest cases, identical findings over the whole tree, and a byte-identical report once both tools exempt the same file.
  * 1f80e58, `tools/verify/guestcheck.c`: the three guest makefiles against the host build, byte-identical output and five contract mutations caught alike.
  * 3586fc6, the three runners: `harness.c`, `runtests.c` and `ptydrive.c`. The matcher agrees with Python's `re` on all 28 patterns the tree uses plus the subset's own edges; five live harness paths agree on exit status and step outcomes; the 63-probe report is identical line for line; the pty driver matches on the window size, the prompt count, the timeout token and the refusal. Six suites pass: exitline, ssh, diskdiag, reset, diskpolicy and drill.
* Two faults of my own in phase 10, both caught by running rather than by reading: seven suite targets invoke a runner from their suite script rather than their recipe, so the prerequisite scan missed them and the binary was never built (exitline failed on it); and the ssh suite passes `--` before its command, which argparse drops and the C driver first took as the command name.
