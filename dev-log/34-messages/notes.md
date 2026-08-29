# Milestone 34 notes

Deviations from `plan.md`, measurements, defects found and what was done about
them, and every decision the plan left to the run.

## Phase 1

Tree clean at `aee2cd6`, no stray machines, build current. Recent history:

```text
aee2cd6 Recast two sentences in the milestone 34 plan
f1f85e8 Rewrite the milestone 34 plan around the review
e66831c Plan milestone 34, the messaging and the tests
48431a7 Write down what the suite review found
2396530 Record the fast-suite milestone
deb2d50 Wait for the console, schedule the long suites first, and stop the credential race
bce5b2c Set the worker count from the processors, measured
9579f5c Run the second card's suites beside the first card's
c090f9c Run the suites through a pool and name each machine's owner
0d040f9 Stop the suites writing each other's images and ports
0b4d0f4 Open the milestone 33 record
115e793 Rewrite the fast-suite plan around what the review found
```

Baseline adopted rather than re-measured: milestone 33's warm run, 465s across
44 suites, recorded in `dev-log/33-fast/notes.md`. The only commits since are
this milestone's plan and findings, which change no code.
