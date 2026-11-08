# Codex round 3: attack plan v1

You are reviewing a preliminary plan for Rach2 (read AGENTS.md and Readme.md first): GPU-accelerated Chromium and Firefox on the Lenovo G50-70 (Haswell HD 4400, i3-4030U) and in QEMU by 2026-11-30, then the original Crysis (GOG) under Proton on the Lenovo by 2026-12-31, plus Linux's full ia32 compat ABI as its own milestone.

Read, in build/flow/coordinator/graphics-research/: plan-v1.md (the plan to attack), comparison.md (what you and the chat already settled in rounds 1 and 2), and findings-chat.md (the chat's evidence, including the measured Debian closure). Your own earlier reports are codex/report.md and codex/round2.md. Upstream sources are cloned read-only under build/research/src/ (mesa, libdrm, weston, libinput, wayland, seatd, libudev-zero, proton, wine, a sparse linux v6.12). If your context is compacted, reread this file, build/flow/coordinator/graphics-research/codex/task-round3.md, and go on.

Do not repeat what rounds 1 and 2 settled unless plan v1 gets it wrong. Attack the plan:

1. What is missing that would stop a goal: a kernel interface, a package, a measurement, an acceptance case, a document. Name the upstream or Rach2 file that proves the need.
2. What is wrong or misordered: a dependency that is not real, or a real one the tables omit; a task that will collide with another in the same files; a task too large for one loop session (3 to 7 subtasks of 10 to 60 minutes each).
3. Decision 13, "Crysis's 64-bit executable first, so December does not wait for full ia32": is it right? What 32-bit code does Proton (PROTON_USE_WOW64=1) still run for a 64-bit game, and what does that need from the kernel?
4. The pace section: at measured agent pace (a task of 3 to 7 subtasks takes one loop 10 to 60 minutes, nine loops), are the targets credible, and what serial limits does it miss?
5. Each milestone's Done: is it measurable by a test or a probe? Propose a sharper one where it is not.
6. The three biggest risks you see in this plan as written, each with the cheapest step that would clear or confirm it.

Check every claim against the code before you make it, and cite files with line numbers or URLs. Write plainly and briefly: at most 120 lines, the claim first, then the evidence. Do not change any file; your answer is your last message.
