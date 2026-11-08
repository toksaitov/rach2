This is a read-only review; use no subagents.

The file build/flow/coordinator/graphics-research/plan-draft4.md is the final draft of Rach2's graphics plan, after a review by Codex and Fable (their reports and rebuttals are in build/flow/coordinator/graphics-research/round3/). Review only deliverable 1, the graphics stack for browsing the web and playing games: the answers to the questions "Mode setting" and "Why the industry drops 32-bit on 64-bit", the decisions, and milestones 73 display, 74 vgpu, 75 intel, 76 ia32 and 77 games.

The question: what in those parts is wrong, missing, or ordered so that it would stop users from browsing the web and playing games (Chromium, Firefox, native Linux games, and the original Crysis under GE-Proton at lowest settings and about 24 FPS) on the Lenovo G50-70 (Haswell i3-4030U, HD 4400) and in QEMU? Check each claim you raise against code: Linux, Mesa, Weston, Wine and Proton sources are in build/research/src/; Rach2's kernel is in rach2/.

Answer with at most ten findings, most serious first. For each: the plan's line or task, what is wrong or missing, the evidence (path:line or URL), and the change you propose. If you find nothing serious, say so. Plain text, ASCII only.
