# Milestone 33: resume state

Phase: 5, the e1000 members. Complete, and taken before phase 4 for the reason
in `notes.md`.

Done: the card picks the forwarded ports and tags every file a run writes, so
the second-card runs are ordinary neighbours of their twins; `run-e1000` runs
its nine members together at -j4.

Evidence: `e1000` 917s to 336s; the whole run 1205s to 727s, 44 of 44, no
stray machines; both cards' network suites concurrently, 10 of 10.

Next: phase 4, the worker count. Measure 4, 8, 12 and 16 workers on the
finished pool, three runs each, and set the default to what was measured.
`account` at 434s is the critical path, so the sweep is bounded by it.
