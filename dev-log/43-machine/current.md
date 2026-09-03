Milestone 43, phase 6: the acceptance script end to end.
Evidence gathered: phase 5 closed; the machine runs the tree's kernel
0e2d1d11... on the ext2 root as the saved default with the KRFFS root
the fallback, proved by a trial, a deliberate fallback, a commit and a
power cut; one difference recorded under "Phase 5" in notes.md. The
stick carries the recovery image with the RACH2SAVE partition holding
the saved copies.
Next action: fix the script's disk section for the installed system,
its choice of the stick's volume, and its use of the first disk slot;
rehearse with tests/run metal; run it against the machine.
File being read when written: none.
