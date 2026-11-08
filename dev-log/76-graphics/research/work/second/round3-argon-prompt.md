Read AGENTS.md first. This is a read-only review: run no shell commands, edit no files, and use no subagents; read files with your own file tools.

The file build/flow/coordinator/graphics-research/plan-draft4.md is the final draft of Rach2's graphics plan, after a review by Codex and Fable (their reports and rebuttals are in build/flow/coordinator/graphics-research/round3/). Review only deliverable 2, the test bed for GPU driver VMs: the answers to the questions "KVM-like virtualization", "What to proxify" and "Existing protocols or our own", and milestones 78 kvm, 79 assign and 80 testbed.

The question: what in those parts is wrong, missing, or ordered so that it would stop the test bed from working on the Lenovo G50-70 (Haswell i3-4030U, HD 4400, Lynx Point-LP, VT-x and VT-d) with Debian trixie's QEMU 10.0.13 unchanged? Check each claim you raise against code: Linux, Mesa and Proton sources are in build/research/src/; Rach2's kernel is in rach2/; QEMU and virglrenderer sources are on the web (GitHub mirrors).

Answer with at most ten findings, most serious first. For each: the plan's line or task, what is wrong or missing, the evidence (path:line or URL), and the change you propose. If you find nothing serious, say so. Plain text, ASCII only.
