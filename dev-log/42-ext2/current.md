Phase 5 is closed and committed; phase 6 is written and its suites are
running: drill-ext2, install-ext2, selfbuild-ext2 and sealed, in that
order, detached, their summaries in the job's tmp suites6.out.
Evidence gathered: the four-partition disk boots its ext2 candidate
through GRUB; the check, the root and recovery passed their review round
(A, C, E fixed; B's report still owed by its Codex thread).
Next action: read suites6.out, fix what fails, measure e2fsck's peak on
the 1 GiB image for hardware.md, record phase 6 in notes.md, commit in
scoped units, then phase 7.
File being read when written: dev-log/42-ext2/plan.md, phase 6.
