# Milestone 30 checkpoint

This file holds only the resume state for the item in progress. Replace its contents instead of appending.

## Position

* Active phase: 1, arrival.
* Item in progress: the required green run and the timed baselines.
* Evidence checked: none yet.
* Exact next action: run `make doctor`, `make full`, `make verify-style`, `make verify-host`, and `make test`, requiring exit zero from each, then time `make distclean && make full` and a no-op `make full`.
