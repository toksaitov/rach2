# Review of the second graphics discovery run

The owner approved the run's design on 2026-11-06 (`dev-log/76-graphics/research/work/run2-plan.md`, eleven steps) and ordered it started on 2026-11-08 at 20:05. It ran from 20:11 to 08:46 on 2026-11-09 (UTC+6), each step by one agent with a written brief. Your task is to judge its results before the chat writes the run's report and draft 6 of the graphics plan. Draft 5, `build/flow/coordinator/graphics-research/plan.md`, is the plan the results correct; the owner has not approved it.

Read, in this order:

* `AGENTS.md` (the project's rules; above all Writing and "Fix every fault").
* `dev-log/76-graphics/research/work/run2-plan.md` (what each step was to decide) and `build/flow/coordinator/graphics-research/run2-machinery.md`.
* `build/flow/coordinator/graphics-research/plan.md`, draft 5, whole.
* The run's rules and briefs, `build/flow/coordinator/graphics-research/run2/common.md` and `run2/brief-*.md`, enough to know what each agent was asked.
* The step reports in `build/flow/coordinator/graphics-research/run2/`: `step1.md`, `step2.md`, `step8.md`, `step3-4.md`, `step5-7.md`, `step6.md` (the testing PC); `lenovo-setup.md`, `step9.md`, `step9b.md`, `step10.md`, `step11.md` (the Lenovo). Raw output is in the directory of the same name beside each report (`run2/step9/`, and so on). Check against it the claims the plan will rest on; a report's own words are not evidence.

Answer in at most 250 lines, ASCII, `*` bullets, no em-dashes, terse:

1. Errors: every claim in the reports that the raw output does not support, contradicts, or that one report states against another. Quote it with file and line, give what the raw output shows (file and line or value), and say whether it changes a conclusion.
2. For each of the run's eleven rows, the decision it was to settle (`run2-plan.md`'s "Steers" column): is it settled, and how? Include the owner's decisions A, B and C in draft 5, the owner's native-QEMU question (step 5) and the TCG default (step 7). Where a step's agent recommends something, say whether the evidence carries it.
3. Draft 5: every statement the run makes wrong or out of date, quoted with its line number, with the measurement and the replacement text; every task (by number, such as 74-6) whose content, order, dependencies or Done should change, with the change, including work the run made unnecessary or that can start earlier; risks cleared and new risks. The plan's milestones 73 to 75 have since been used by the Wi-Fi work (`dev-log/73-wifi-steady`, `74-wifi-soak`, `75-beacon-line`), so draft 6 renumbers 73 to 80 from 76; name tasks by draft 5's numbers here.
4. Faults the run met, each judged: a fault in Rach2 or its tools to fix in the milestone that touches it, a fault of the run's own method, or none. At least these: step 8's ext2 image with errors after an unclean stop of Rach2, repaired by `e2fsck`; step 5-7's `tests/run` on Windows failing admission ("etude admit: start: /usr/bin/env: No such file or directory"); step 9's IGT loading `snd_hda_intel` and `vgem` by name despite the kernel's blacklist; step 3-4's progress monitor that kept polling guests through later runs; step 10 and 9b's KVM "Bad address" with i915's huge pages; step 6's module built with gcc 14 set to gcc 13's answers and loaded into WSL's running kernel. Say for each whether the conclusions that rest on it stand.
5. What the run did not settle and should be measured next, in priority order, each with the machine and the command or step, and whether the plan can go to the owner without it.

Rules: read and change nothing; start no subagents; no GitHub (no `gh`, no github.com fetches); cite `file:line` for every claim; say plainly where you are unsure. Do not restate what the reports say; give only findings, judgments and reasons.
