# Milestone 33: resume state

Phase: 1, arrival and the baseline. Complete.

Done: tree clean at 115e793, nothing else running, build current. The baseline
is the last complete run of this tree, 4877s across 44 suites, recorded per
suite in `notes.md`. A fresh baseline run was started and then stopped once it
was clear the existing one was still valid.

Next: phase 2, the collisions. Give the q35 suite its own disk and the
`$(HARNESS)` dependency it uses without declaring; give the second suite of
each port pair its own port; move the two cross-suite restagings into
preparation; make the shared phony image builders real file targets. Suites
stay serial. The proof is the sixteen-suite batch that previously failed.
