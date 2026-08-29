# Milestone 33: complete

All seven phases are done. The record is in `Readme.md` beside this file, the
measurements and the defects in `notes.md`, and `33-fast` is in the dev-log
index.

Final: cold from `make distclean` 618s, warm 465s, repeated at 464s and 465s.
All 44 suites pass in every run and no machine survives one. The baseline was
4877s, so the warm run is a factor of ten and a half faster.

Phases 4 and 5 were taken in the other order, for the reason in `notes.md`:
sweeping worker counts while one suite was 917s of a 1205s run would have
measured that suite rather than the workers.
