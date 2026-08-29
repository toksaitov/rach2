# Milestone 33: resume state

Phase: 3, the pool. Complete.

Done: `prepare-all` builds the union of every suite's prerequisites in one
Make; `tests/run all` runs one such build and then pools the suites eight at a
time, with `style` and `host` outside; per-suite leak attribution survives
through a `PATH` shim that records each machine against the suite that started
it; `tests/run NAME NAME...` pools a named list.

Evidence: 4877s to 1205s, 44 of 44, repeated at 1203s, 1202s, 1205s with no
stray machines. Attribution proved by restoring the `crash` defect: the run
names `crash` and blames no neighbour.

Next: phase 4, the worker count. Measure the finished pool at 4, 8, 12 and 16
workers, three runs each, and set the default to what was measured. Note that
`e1000` at 917s dominates a 1205s run, so the sweep will show little until
phase 5 breaks that block up; consider doing phase 5 first and then sweeping.
