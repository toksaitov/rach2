# Brief: draft 6 of the graphics plan

The owner approved the second graphics discovery run on 2026-11-06 and it ran on the night of 2026-11-08 to 09. Codex and Fable reviewed its results and argued their differences; the chat ruled on what they left open. You write draft 6 of the graphics plan from draft 5 and those results. Codex and Fable will review your draft, then it goes to the owner for a yes.

## Read

* `AGENTS.md`, above all Milestones, Writing and its Register rule.
* Draft 5: `build/flow/coordinator/graphics-research/plan.md`, whole. Draft 6 keeps its shape and sections and changes what the run changed.
* The rulings, which bind you: `build/flow/coordinator/graphics-research/run2/rulings.md`.
* The run's report draft, `dev-log/76-graphics/research/run2/report.md`, for the figures (each checked against raw output), as amended by the rulings.
* The reviews, for the line-by-line changes: `run2/fable-review.md` section 3 and `run2/codex-review-b.md` section 3 (both list draft 5's lines that the run makes wrong, with replacements), then `run2/fable-argue.md` and `run2/codex-argue.md` where they settle a change differently. Where a review and the rulings differ, the rulings win. Check a figure against the report or raw output (`build/flow/coordinator/graphics-research/run2/stepN/`) before you use it.

## Write

The whole of draft 6 to `/private/tmp/claude-501/-Users-toksaitov-Desktop-rach2/dbb56312-4e4d-40d2-8f9f-2dd1572abdb5/scratchpad/plan-draft6.md`. The chat moves it into place; do not write anywhere else.

* Renumber the graphics milestones: draft 5's 73 to 80 become 76 to 83 (73 to 76, 74 to 77, 75 to 78, 76 to 79, 77 to 80, 78 to 81, 79 to 82, 80 to 83), and every task number and cross-reference with them (73-15 becomes 76-15). Milestones 73 to 75 are the Wi-Fi work (`dev-log/73-wifi-steady`, `74-wifi-soak`, `75-beacon-line`); a number that names a release or the Wi-Fi work (such as "release 71", "milestone 74's release") is not a graphics milestone and keeps its number. Say once, near the top, that the numbers moved and why.
* Replace "What the night changed" with what the second run changed, in the same terse form: each finding that changes the plan, with its measured figure and where it comes from (the report's section).
* Decisions: A becomes the WSL module as the Venus host for the agents' work (the step 6 measurements, its costs: root loads it after each WSL start or the owner allows it at boot, a tainted kernel, an oops panics WSL for every account, a rebuild per WSL kernel), with draft 5's option 1 as the fallback and option 2's premise struck. B and C keep option 1, now measured, with the evidence. Add the sandbox decision of ruling 2 as the owner's. Each decision: the question, the options with what each gains and costs, and the plan's recommendation. Keep "Yours, kept" and the rest of the decisions' structure.
* Every task whose content, order, dependencies or Done the run changes, changed: the reviews' section 3 lists, as the rulings settle them. Work the run made unnecessary goes; work that can start earlier says so; new tasks the reviews agreed on are added (the Linux limits step 8 found, in 76; Wine's gaps, in 80; the stack-qualification task of ruling 4; the sandbox task of ruling 2, marked as the owner's choice). Keep each milestone's size estimate, changed only where the run measured something that changes it, and mark estimates as estimates.
* Order, verification, risks (cleared, changed, new), what the plan leaves out, costs the owner has not seen, and "The research": brought up to date. Add the second run to "The research" in a few lines, pointing to `dev-log/76-graphics/research/run2/report.md`. The next measurements of ruling 12 go where each belongs, not as a list of their own.
* The plan states what is true now, with no history of earlier drafts.
* No longer than draft 5 by more than 40 lines.

## Rules

* Read only, except the one scratchpad file. No subagents, no GitHub, no `ssh` or other access to the testing PC or the Lenovo, no git commands that change anything.
* Draft 5's voice: ASCII, `*` bullets, no wrapped prose, no em-dashes and no spaced `--`, little bold, tables only for tabular data, the claim first. Run `build/tools/verify/prosecheck banned FILE` on the result if it exists and fix what it finds.
* When done, reply with the line count, the decisions as draft 6 words them, and every change you made to a task's Done, in one line each.
