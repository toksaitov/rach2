# Milestone 30 notes

Deviations from the plan and defects deferred to later work are recorded here as they are found.

* The `*-test` make targets keep their recipes through phases 3 to 5 because the runner dispatches through them; they become notice-printing aliases in phase 6 when their bodies move to `tests/system/suites/`. Documents that name them (`make ssh-test` and the like in the vendor Readmes and test documents) are renamed to `tests/run` forms in phase 6 with the moves, not before.
* `tools/doctor.sh`, `bootstrap_mac.sh`, and several image scripts still print `make doctor` or reference it in messages; the alias works this milestone, and the remaining mentions move with the scripts in phase 6.
