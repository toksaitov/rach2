# One phase at a time

## Why

The night, a run of every suite at 19:17 UTC, was the part of the process that cost the most in its first two days (incidents.md): GitHub dropped the scheduled run, nights were started by hand with work outstanding, nothing started a failed night again, the 09:00 UTC+6 deadline left room for one fix and one run, and the Lenovo received only a kernel. The board, meanwhile, held 417 finished cards after two phases.

The owner decided on 2026-10-16 to drop the night and to finish one phase at a time: review it, test it whole, install it on the Lenovo, and only then start the next. Codex (`gpt-6-astra`) wrote the design in three rounds with Claude Code, and checked each claim against the code; the owner accepted it.

## What changes

* A phase's Status is its stage: Backlog, planned; Ready, approved by the owner; In progress, the one phase being worked; Done, released. `board.sh start` starts the top Ready phase when none is In progress and records the commit it starts at on the phase.
* Workers take only `ci` issues and the members of the phase In progress, and `board.sh take` refuses any other. The manager makes each issue it keeps a member of that phase.
* `board.sh phase` decides by program, not by the manager's reading, when the phase is ready: no open member or `ci` issue but those labelled `intermittent`, no `hold`, a review that has read every commit up to master's head, and core passed on that head.
* The reviewer reviews ranges of commits: the phase's work since it started, then each round of fixes, until a review reaches master's head. A review that fails runs again, three times at most, or once when it reached its three hours, and then the manager diagnoses it; the owner is not asked.
* `full.yml` replaces `night.yml`: started only with a commit, it builds that commit from clean and runs every suite on both architectures. Failure issues carry the label `ci`. An `intermittent` label goes when the failure repeats, and bisect labels only from the run it tested. A core run started by hand tests master's head again.
* `tools/flow/release.sh` runs the release: the full run, the build from clean for the Lenovo, the recovery system first, then the kernel and the GNU root from recovery with `rach2-install --update`, the acceptance on the trial boot, `bootctl commit`, the closing of the phase and the archiving of its cards. It keeps a record of its steps and resumes from it, and never starts the installer twice.
* A worker whose finished work waits for a `hold` parks it and takes a repair of core, so that no loop waits while a repair waits for a worker.
* The day report takes its numbers from `status.sh day`.

## What was decided against

* A board per phase: archiving a released phase's cards keeps one board short.
* A cleanup procedure of its own for the full run and the release: `make clean` for each architecture keeps the vendored builds, as the manager's build from clean already did.
* A QEMU rehearsal of the release before the first one on the Lenovo, which is a sandbox the owner authorized; the first release is watched step by step.
* Bisection keyed to a run's attempt as well as its number: the process never runs an attempt again, and starts a new run instead.
* Checking a held issue again for a blocker, the label `owner` or its phase when a worker resumes it: its work may be committed only in that worker's clone, and the session hands it back when it cannot go on.
* Holding a release open on a failed push to the owner's phone: the phase is closed and archived, and the release's log says the push failed.
* A change to `rach2-install` now: a run started again in the same boot removes the copy of the homes it kept in `/dev/shm`. The release never starts it twice, and the fault is filed as a bug.

## What tells us it works

* Each phase closes with a comment that names the commit, its full run, the digests of what the Lenovo received and the acceptance's verdict, and `/etc/rach2/release` on the Lenovo names the same commit.
* No phase is In progress beside another, and no worker holds an issue outside the phase In progress that is not a `ci` issue.
* The board holds no Done card of a released phase.
