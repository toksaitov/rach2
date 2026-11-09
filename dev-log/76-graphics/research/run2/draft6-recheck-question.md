# Recheck of draft 6 after the review's fixes

You reviewed draft 6 of the graphics plan (`build/flow/coordinator/graphics-research/run2/codex-draft6.md`), as did Fable (`run2/fable-draft6.md`). The chat merged both reviews into 37 fixes (`run2/draft6-fixes.md`) and then changed how milestones that need each other's code work: they are stacked (a later milestone's branch is cut from the earlier's and merges it in as it moves) and each releases when it is done, never before one it stacks on. The revised plan is `build/flow/coordinator/graphics-research/plan.md`; the draft you reviewed is `run2/plan-draft6-as-reviewed.md`. The writer could not apply fix 19 as written (the run recorded only GE-Proton's SHA-512, so the SHA-256 is computed at fetch) and renumbered 82's tasks to 82-1 to 82-4 after dropping the finished 82-1.

Answer, numbered so, in at most 70 lines, ASCII, `*` bullets, `file:line` for each claim:

1. Each of the 37 fixes not applied, or applied wrongly; name it by number.
2. New errors the revision brought: a figure the report does not support, a task or cross-reference that now points wrong, a "Waits on" that contradicts the stacking in Order, or a Done that sets a time limit or departs from Linux without saying it is the owner's choice.
3. The stacking and release rule in Order and "After your approval": is it sound with this repository's git rules (`AGENTS.md` Git and Milestones, `.agents/skills/milestone/SKILL.md`, `tools/git-hooks/`), and what would break? Say plainly if a milestone branch that merges another open milestone's branch, then rebases onto master at its end once that milestone has released, loses or duplicates work.
4. Whether the plan can now go to the owner for a yes, and if not, the one thing that stops it.

Rules: read and change nothing; start no subagents; no GitHub. State each finding in its own message as you go and give the whole answer as your last message; if your context is compacted, reread this question (`build/flow/coordinator/graphics-research/run2/draft6-recheck-question.md`).
