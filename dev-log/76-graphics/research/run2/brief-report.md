# Brief: the second graphics discovery run's report, first draft

The owner approved the second graphics discovery run on 2026-11-06 (`dev-log/76-graphics/research/work/run2-plan.md`, eleven steps) and ordered it on 2026-11-08 at 20:05 UTC+6. It ran to 08:46 on 2026-11-09, one agent per step. Codex and Fable now review the results; the chat writes the run's report, then draft 6 of the graphics plan. You write the report's first draft. The chat corrects it with the reviews and their argument round afterwards.

## Read

* `AGENTS.md`, above all Writing and its Register rule.
* `dev-log/76-graphics/research/work/run2-plan.md` (the table's "Steers" column is what each step was to decide) and `build/flow/coordinator/graphics-research/run2-machinery.md`.
* The step reports in `build/flow/coordinator/graphics-research/run2/`: `step1.md`, `step2.md`, `step3-4.md`, `step5-7.md`, `step6.md`, `step8.md`, `lenovo-setup.md`, `step9.md`, `step9b.md`, `step10.md`, `step11.md`, with raw output in the directory of the same name beside each. A report's own words are not evidence: check every figure you carry into the report against its raw file, and cite that file.
* `run2/fable-review.md`, Fable's review, as a second reading. Its figures are Fable's; check any you use.

## Write

`dev-log/76-graphics/research/run2/report.md`, at most 320 lines:

* A summary paragraph: what the run measured, on which machines, from when to when, and what it decided, in five or six sentences.
* `## The decisions`: one short section per row of the run's table (eleven, steps 3 and 4 and steps 5 and 7 may share one each as their reports do). Each says the question, Linux's result (Debian 13, the reference), Rach2's result where the step measured one (steps 1, 7, 8 and 11 did; elsewhere say "not yet run on Rach2"), and the decision with the evidence that carries it. Include the owner's decisions A, B and C as draft 5 (`build/flow/coordinator/graphics-research/plan.md`) words them, the owner's native-QEMU question (step 5) and the TCG default (step 7).
* `## Reference numbers`: one table of the figures each later gate compares with, a row per measure: measure, machine and setting, Linux's figure, Rach2's figure or "not run", raw file. On the Lenovo give the IOMMU-off figure beside the IOMMU-on one, since Rach2 has no IOMMU driver.
* `## Faults met`: each fault, whose it is (Rach2's, its tools', Linux's, or the run's own method), and whether the conclusions that rest on it stand.
* `## Errors in the step reports`: each claim the raw output does not support, with the correction.
* `## Not settled`: what remains, by priority, each with the machine and the measurement.
* `## Files`: where the briefs, reports and raw output are. The reports and briefs will be copied beside this file; the raw output stays on the Mac in `build/flow/coordinator/graphics-research/run2/`.

Leave one line under the summary, `Reviews: to be added after Codex's and Fable's argument round.`, and nothing more about the reviews.

## Known so far

Checked by the chat against raw output; put each in its section:

* Step 8's report omits `step8/out/e2fsck-after-stop.log`: after Rach2 was stopped uncleanly, `e2fsck -fn` found nine deleted inodes with zero dtime, block bitmap differences and inode bitmap differences; `e2fsck -fy` repaired them ("FILE SYSTEM WAS MODIFIED", exit 1) and a recheck exited 0. Read the step's scripts and log to say when the unclean stop happened and what the image was doing; say what is not known.
* `run2-machinery.md:20` said Crysis was on p3. It was not (`lenovo-setup.md:99`); the chat wrote that from expectation. This is the chat's error and is listed first.
* `step1.md:120` says QEMU on Windows gets no host GPU when started over SSH; step 5's raw output shows stock QEMU running `egl-headless` in session 0 with virgl over the RTX 4080 SUPER (`step5-7/out/win-gl0.txt`, `step5-7/out/p3a-d3d11/info.txt`). Check, and say whether step 1's choice of WSL still stands on the other measurements.
* Step 9's claim that IGT loaded `snd_hda_intel` and `vgem` (`step9.md:214`) and step 5-7's etude admission error (`step5-7.md:128`) rest on the agents' words; no raw file holds them.
* vkd3d-proton's describe string differs between `step2.md:100` and `step9.md:26` for the same commit 8293130eb; name the commit.
* Step 3-4 used VK-GL-CTS 1.4.1.1 (Mesa 25.0.7's CI pin), step 9 used 1.4.7.1; the two machines' dEQP-VK references are not the same case list.
* "Debian translates DMA by default" is measured on this Lenovo only; the mechanism was not traced (`step11.md:23`).

## Rules

* Read only, except the one file you write. No subagents, no GitHub, no `ssh` or other access to the testing PC or the Lenovo, no git commands that change anything.
* ASCII; `*` bullets; no wrapped prose; no em-dashes and no spaced `--`; little bold; tables only for tabular data. Write the claim first, then what is not obvious. Plain words: name the mechanism, give measured numbers, mark an agent's estimate as an estimate.
* Times in UTC+6 as the reports give them.
* When done, reply with the file's line count and a list of every figure you could not match to raw output.
