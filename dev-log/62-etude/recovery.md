# Completing the portable collection

The recovery completes the original testing-system goal. Etude, its compiled helpers and QEMU run the collection natively on Windows, macOS and Ubuntu; etude-guest runs eligible cases on Rach2. The current executor, protocol, manifests and result store remain where they meet that contract. The temporary shell entry and host-tool adapters are replaced before the comparison runs.

The owner authorized this Codex-led recovery on 23 October, with GPT-6.1 Sol workers, effort chosen for each task, and implementation and repairs batched before expensive checks. The previous loops remain stopped. Issue #798 records coordination; the board records each active assignment. This document supplements the plan and supersedes its use of a Linux tooling machine during test execution.

## Acceptance

* The collection entry, case selection, scheduling and host control flow are C. A native execution uses prepared immutable artifacts and directly executable helpers. It invokes no host shell, Python, Perl, OpenSSH client, mtools, e2fsprogs or Linux tooling machine. Deployment may use the owner's SSH access, and source production retains its declared build tools. A program being tested is recorded separately from the runner's dependencies.
* Preparation builds guest artifacts and native helpers before execution. The record identifies their source, toolchain, platform, contents and required build verification. Every requested case remains visible; a missing artifact or unsupported case is incomplete work, never a pass or a silent omission. Preparation and execution costs are reported separately and together where the comparison requires them.
* Both architectures share one schedule. Cases retain stable identities, assertions, resource declarations, independent file-system checks and distinct failure, cancellation and incomplete outcomes. A deletion has a recorded redundancy argument. Moving an old host script into a helper guest does not convert its control flow to C.
* A case's watchdog uses successful history matching the machine, accelerator, architecture and workload. It measures genuine progress. Failed and interrupted attempts do not teach bounds. Initial, stage, cleanup and whole-case bounds remain explicit, and uncertain cleanup keeps its reservation.
* The complete desktop collection passes on both architectures on each comparison platform, including the declared large-memory profiles. The Lenovo runs every eligible native case, with the original exclusions explained. The assertion inventory, source, artifact hashes, capacity, outside load and tool versions make the results comparable.
* Routine testing moves to the fastest repeatably passing configuration. The plan's timing goals and budgets remain. The phase review covers all commits, the release runs the required complete collection, and the candidate becomes the Lenovo's default only after the release's trial acceptance. Passing a component or demonstration closes none of these obligations.

## Work and ownership

| Work | Owner | Record |
| --- | --- | --- |
| Native entry, selection, schedule and learned bounds | Codex runner worker | #973, with the preserved #962 and #966 repairs |
| Guest command/file channel and exact SSH cases | Codex channel worker | #974, with #969 and #970 portability repairs |
| Native API and time fixtures, protocol drivers and verifiers | Codex service worker | #975 |
| Independent image operations and prepared helper images | Coordinating chat | #976 |
| Remaining host and machine drivers, source verification and dependency audit | Coordinating chat, assigned by file as workers finish | #977 |
| Saved native/kernel fixes, redundant tests, cache repairs and acceptance runs | Coordinating chat | Existing open members of #589 |

The channel uses the existing framed protocol. SSH assertions use the exact client in a declared Rach2 helper where necessary; control-only operations use the target's framed channel. Image operations attach private raw images to a small Rach2 helper and invoke independent compiled file-system tools. Native services link their required libraries during preparation. Helpers have declared resources and owned cleanup; none is an unaccounted background machine.

Each worker has a separate worktree and starts no helpers. The coordinator owns shared interfaces, integration, board state and resources. A worker compiles while implementing, but starts no suite or review independently. Finished parts and all known repairs are integrated before the affected checks. A failure leads to a batch of repairs and repetition of only the checks those repairs can affect. Complete platform runs and phase review follow the completed implementation.

## Starting evidence

The starting commit is fc9f7421ac4ed5c081f666597ab6c576e5c21b21. Its routine checks passed on both architectures, but the phase has not had its review. The earlier Ubuntu and Mac results describe the adapter-based collection; they do not establish acceptance of the portable path. Windows has no complete passing collection record. The saved Lenovo result predates its branch's rebase and does not validate the final native fixes.

The first three workers use GPT-6.1 Sol at high effort. The account reported 4% of its weekly allowance used at the start. Usage and the remaining implementation path are checked at integration points. The initial target is 24 October; it is revised from completed code, required checks and measured run times, rather than from issue counts or human development estimates.

## Repairs already on master

Issue #938 is fixed by `0b8953edd`, which makes private copies of the GNU store's read-only trees writable before modifying them. The Chromium archive orders a directory before all of its descendants, so extraction finishes those descendants before restoring the directory's mode. The recovery's i686 GNU preparation completed the ports, native compiler, GDB, JavaScript, Chromium and sanitizer stages from the store; its complete log is retained with the preparation evidence.

Issue #942 is fixed by `c771b2363`, which runs util-linux's install with one job. The installed programs no longer link while libtool reinstalls `libmount`. The GNU ports stage completed with this recipe during the recovery preparation. The saved worker's equivalent fix is not applied a second time.
