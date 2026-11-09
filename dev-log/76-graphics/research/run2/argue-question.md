# The argument round on the second graphics discovery run

Codex and Fable each reviewed the second graphics discovery run apart, answering `build/flow/coordinator/graphics-research/run2/review-question.md`. The answers, in `build/flow/coordinator/graphics-research/run2/`:

* `fable-review.md`: Fable, all eleven rows.
* `codex-review-a-findings.md`: Codex on rows 1 to 8 (the testing PC). Only the findings it stated as it went survive; its final answer was lost to a context compaction, so it gave no row-by-row judgment of rows 1 to 8.
* `codex-review-b.md`: Codex on rows 9 to 11 (the Lenovo).

The chat has since drafted the run's report, `dev-log/76-graphics/research/run2/report.md`, with each figure checked against raw output; its "Faults met", "Errors" and "Not settled" sections carry what the chat could check. Draft 5 of the plan is `build/flow/coordinator/graphics-research/plan.md`. Paths below without a directory are in `build/flow/coordinator/graphics-research/run2/`.

Corrections already made by the chat against raw output: Fable's "11 deleted inodes" is 9 (`step8/out/e2fsck-after-stop.log:3-11`), and that log does record the repair (`== e2fsck -fy`, "FILE SYSTEM WAS MODIFIED"). Do not argue these again.

## The differences to settle

For each, say which reading the evidence carries and what draft 6 should say, citing `file:line`. Read only what each point needs, starting from the lines cited; do not reread whole reports.

1. Step 8's ext2 errors after the unclean stop. Fable: a Rach2 data-safety fault to fix before 73's release. Codex: requires investigation under the data-safety rule. The report: boot 2 was stopped while two copies of the suite ran Wine's `wineboot`, with no `sync` after; `rach2/sys/fs/ext2.Readme.md:68,103-111` says a stop leaves orphans of files removed while open, kept for `e2fsck`, and a writable mount of a volume not marked clean is checked first; nine "deleted inode has zero dtime" with matching bitmap differences fit that. Is this a fault in Rach2, the documented result of an unclean stop on a write-back mount, or not yet known? What one forced test settles it, and must it run before draft 6 goes to the owner?
2. Chromium's GPU sandbox, 73-17 and 75-12. Fable (`fable-review.md:48`): Chromium's GPU process is unsandboxed on Debian in every configuration, so Done takes Linux's state as the mark or a Chromium or Mesa fix becomes a task. Codex (`codex-review-b.md`, section 1 first item and section 3, 73-17): two failing configurations, not impossibility; keep "sandboxes on" and trace the start-up and the denied EGL load. AGENTS.md says a limit Linux lacks is fixed with Linux's behaviour as its Done. What is 73-17's Done, and is the trace needed before the owner sees draft 6?
3. `LP_NUM_THREADS=0`. The report (step 3 and 4's decision) and Fable: in every test guest. Codex (`codex-review-a-findings.md`, the `LP_NUM_THREADS` item): too broad, since one program alone gains about 3 times in glmark2 and 7 times in WebGL's shader loop with 8 threads. Which profiles take which setting?
4. The Venus guest driver's Mesa. Fable: measure trixie-backports' Mesa 26.1.6 under Venus with the WSL module, about an hour, not blocking. Codex: one coordinated stack-qualification task over 73-13, 74-4, 74-6 and 77-1, since nothing proves 26.1.6 fixes everything. Measurement now, a task in the plan, or both?
5. The owner's native-QEMU question (step 5). Fable: settled, WSL. Codex: step 5 never switched on Venus or blob memory in the Windows QEMU, so their absence there is not shown (`step5-7/scripts/win-path.sh:19`). Does the answer to the owner stand as "native QEMU on Windows is worse than WSL on every measure taken", and does it need one more check (for example whether the Windows build's virglrenderer has Venus at all)?
6. 77-2 and Crysis. Fable: 77-2's Debian session is done and leaves the Lenovo queue; HDMI stays for the sitting. Codex: 77-2 becomes a remainder task (umu, the fullscreen route, a silent working audio sink, the whole first level, native-game replays). What remains of 77-2, and where does each piece go?
7. Fullscreen. Fable: the game needs a fullscreen X client at the origin, cage rather than Weston's Xwayland. Codex: do not silently put cage in place of the owner's desktop choice; fix the chosen desktop path or name a launcher arrangement. What does draft 6 say?
8. VT-d's cost. The report and Fable: Rach2 compares with the translation-off figures; a VM with the assigned GPU trails native by about 15 percent from VT-d. Codex: the off runs also changed i915's huge pages and stolen memory, so the 15 percent is not VT-d alone and is no fixed allowance. What does draft 6 say for 77-2's native gate, 79's Done and the 10 percent gate?
9. Step 10's blob costs. Fable: 3.5 ms per map, 50 ms queue stall per unmap. Codex: serial allocate-and-free differs from batched reuse; 32 frees took 15.9 ms and the next map 116.8 ms (`step10/blob-guest-2.txt:10`). What is 80-5's Linux baseline?
10. KVM's "Bad address". Fable: i915's huge pages, confirmed both ways. Codex: the trigger is isolated, the mechanism not traced; the saved source's comment concerns non-compound pages (`step10/linux-src/virt/kvm/kvm_main.c:2944`). What may draft 6 state as fact, and what goes to 78-1?
11. Rows 1 to 8, which Codex did not judge row by row: Codex, say where you disagree with Fable's section 2 for rows 1 to 8 and Fable's section 3 items on tasks those rows touch (73-3, 73-9, 73-13, 73-16, 74-1, 74-2, 74-5, 74-6, 77-3, 78-6, 78-7). Fable, say where you disagree with Codex's section 3 for rows 9 to 11.
12. Next measurements: one merged list of at most eight, in priority order, each with the machine and the command or step, and for each whether draft 6 can go to the owner without it.

## Answer

At most 160 lines, ASCII, `*` bullets, no em-dashes, terse, `file:line` for every claim, numbered as above. Where you now agree with the other reviewer against your own review, say so. Say plainly where you are unsure.

Rules: read and change nothing; start no subagents; no GitHub (no `gh`, no github.com fetches). Codex: state each point's answer in its own message as soon as you settle it, and give the whole answer as your last message; if your context is compacted, reread this question (`build/flow/coordinator/graphics-research/run2/argue-question.md`) before going on.
