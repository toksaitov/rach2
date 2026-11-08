# Review of the graphics plan against tonight's measurements

You review `build/flow/coordinator/graphics-research/plan.md` (the graphics plan, draft 4; the owner has accepted three changes and has NOT approved the plan) against what was measured tonight, 2026-11-04, on the owner's order: "do research, experiments ... collect useful info ... reviews plan at the end, use Codex and Fable to discuss, argue, figure out what to check."

Read, in this order:

* `AGENTS.md` (the project's rules; above all Writing).
* `build/flow/coordinator/graphics-research/plan.md`, whole.
* `build/flow/coordinator/graphics-research/tonight/lenovo.md`: the Lenovo G50-70 under Alpine Linux tonight (Crocus, hasvk, VA-API, Chromium 156, Firefox 151, Wine and DXVK, VT-x, the watchdog, the i915 interface). Raw outputs under `build/tmp/gfxref-lenovo/out/`.
* `build/flow/coordinator/graphics-research/tonight/qemu-wsl.md`: QEMU 10.2.1 with KVM, virgl and Venus on the testing PC's WSL tonight (still being written; read what is there). Raw outputs in `tonight/qemu-wsl/`.
* `tonight/codex-checks.md` and `tonight/fable-checks.md`: what Codex and Fable proposed to check before the runs.
* Facts the chat measured tonight that are not in those files: Windows 11 build 26300 with tonight's updates KB5132402 and KB5127936 gives WSL nested virtualization: `svm` on all 32 threads, kvm_amd loaded with `nested=1`, `/dev/kvm` root:kvm 660 (`.wslconfig` already had `nestedVirtualization=true`); the testing PC's seven test accounts were added to the `render` group of its Ubuntu on the owner's order of 04:10, so 74-1's setup change is done.

Answer, in at most 200 lines, ASCII, `*` bullets, no em-dashes, terse:

1. Every statement of `plan.md` that tonight's measurements make wrong or out of date: quote it with its line number, give the measurement that contradicts it (file and line or value), and write the replacement text.
2. Every task (by number, such as 74-6) whose content, order, dependencies or Done should change because of tonight, with the change. Include tasks that become easier or can start earlier, and work that tonight made unnecessary.
3. Risks that are now cleared, new risks tonight revealed, and decisions the owner must make because of tonight (each as one question with the options and your recommendation). Keep the owner's earlier decisions unless a measurement shows one cannot work, and then say which.
4. What tonight did not settle and should be measured next, in priority order, each with the command or step and the machine.

Rules: read and change nothing; start no subagents; no GitHub (no `gh`, no github.com URLs fetched); cite `file:line` for every claim about the plan or the code; say plainly where you are unsure. Do not restate what the files say; give only changes and reasons.

Still being measured while you read: on the Lenovo, DXVK 2.7.1 on hasvk (two runs, with hasvk reported as 1.2 and as 1.3, printed nothing in 90 s and were killed by the timeout; a third run with DXVK's debug log is under way), Venus over hasvk through virglrenderer's vtest server, and a kernel booted under KVM against TCG; on the testing PC, the QEMU report until about 06:20. Where an answer depends on one of these, say what each outcome would change.
