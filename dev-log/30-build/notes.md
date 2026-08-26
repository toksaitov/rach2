# Milestone 30 notes

Deviations from the plan and defects deferred to later work are recorded here as they are found.

* The `*-test` make targets keep their recipes through phases 3 to 5 because the runner dispatches through them; they become notice-printing aliases in phase 6 when their bodies move to `tests/system/suites/`. Documents that name them (`make ssh-test` and the like in the vendor Readmes and test documents) are renamed to `tests/run` forms in phase 6 with the moves, not before.
* `tools/doctor.sh`, `bootstrap_mac.sh`, and several image scripts still print `make doctor` or reference it in messages; the alias works this milestone, and the remaining mentions move with the scripts in phase 6.
* The grub-test manifest had drifted from os-real: the `/bin/dmesg` line added by an earlier milestone never reached it, unnoticed because the grub suite needed Docker and had not run since. Found by the first native run; fixed in the phase 8 commits.
* Two integration faults in phase 9's own conversions, both found by running the suites rather than by reading the diff. The self-build suite's space preflight and the reproducibility check reached into `check_setid.py` as a module, so converting that file meant giving the C tool `--free` and `--nodes` subcommands to answer them. And the rewritten menu-truncation fragments padded from a literal head length of 35 bytes where the head is 37, which ran two bytes past the slot and destroyed the GRUB environment block behind it; the menu suite caught it on the armed-trial boot. Both padding sites now measure what they wrote.
