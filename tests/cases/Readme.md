# Etude's cases

This directory has the case manifests of the suites that etude runs as cases, one family to a file, `FAMILY.json`, and the programs of their host cases under `host/`. A manifest says what each case runs, what it asserts and how it ends, in the contract that [etude's Readme](../../tools/etude/Readme.md#cases) gives. The suite of a family is the last part of its identity, split at dots: `host.style.json`, the family `host.style`, is `tests/run style`, the source checks of the tree, whose program is [`host/style.sh`](host/style.sh). The other suites are still the `run-` targets of [`system/suites.mk`](../system/suites.mk).

## Running

`tests/run NAME` runs a family's cases as it runs any suite, and prints a line for each case as it starts and the suite's result at the end. Before any suite runs, `casecheck families` checks every manifest here against its schema and its rules, and a manifest at fault refuses the run, whatever suites it names. A host case's program can also be run alone, from the root of a tree that `make` has built:

    sh tests/cases/host/style.sh conventions

## Interfaces

A host case's program is the node its `prepare` names, which the preparation builds under `build/`, or under the build of the architecture when the step is `per_architecture`; the rule in the root `Makefile` copies `host/NAME.sh` to `build/tests/cases/host/NAME`. The set driver runs it with the workload's arguments, in the root of the tree, with `ETUDE_CASE` the case's identity, `ARCH` the item's architecture when the case has one, `ETUDE_SEED` when the workload gives a seed, and `TMPDIR` a scratch directory of the item's own, which the driver keeps after a failure and removes otherwise. The program writes its report records on its standard output, as [Report records](../../tools/etude/Readme.md#report-records) gives them, with any lines for a person between them, and `casecheck attempt` judges the item by the manifest, the records and how the program ended. A case passes only when every assertion its branches select passed, the program ended as `termination` says and nothing in the report contradicts the manifest; a check whose tool is absent or at a version that does not judge reports a branch, never a printed skip.

## Adding tests

A new case goes into its family's manifest with a new `case_id`, and a new assertion with a new `assertion_id`; changing what an identity means takes a new one. The program reports each assertion the manifest lists for the branches it chooses, and nothing else. A new family takes a row in `tests/registry` whose name is the last part of its identity, a sentence `` `tests/run NAME`: `` in `system/suites.mk` with no target under it, and the suite's line in `system/suites.md`, which `tests/run inventory` prints. `tests/run style` refuses a row that has both a target and a family, and a manifest that is at fault.

## Tests

`tests/run style` checks the manifests with `casecheck` and the programs with ShellCheck, and runs [`style_cases_test.sh`](../system/style_cases_test.sh), which requires a planted source fault and a planted manifest fault each to fail its case. [`verify_temp_test.sh`](../system/verify_temp_test.sh) requires `style.sh` to keep its logs in a private directory of each run.
