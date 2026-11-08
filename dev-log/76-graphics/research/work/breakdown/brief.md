# The breakdown of the graphics plan: the drafters' brief

The owner approved the graphics plan on 2026-11-04 at 03:45 (UTC+6): `build/flow/coordinator/graphics-research/plan.md`, with its three changes (a trial root image on the Lenovo's data partition; the 10% performance gate for the native driver; Steam out). The next step the plan names is the breakdown: every task of milestones 73 to 80 split into subtasks, with the files each task owns, its tests and documents, what it waits on and what done means, at the level a worker loop takes and finishes without asking anyone. The chat, Codex and Fable research and argue it; Muse and Argon review the result. Five drafters write it at once, each for its own milestones; Codex and a separate Fable reviewer then read all five.

The owner's standing order for this work (2026-11-03, 23:07): plan for the quality of the goals, without dates; stay pragmatic; work at LLM speed.

## Read first

* `AGENTS.md`, whole: the rules every worker follows. Above all Milestones, Work, Build, Testing, Resources, Documentation, Comments and Writing.
* `plan.md`, whole; then your milestones' sections closely, with Decisions, Order, How it is verified, Risks, What the plan leaves out and Costs.
* `dev-log/70-wifi-chip/plan.md`: the form a milestone's plan takes. Copy its form.
* The code each task touches, before naming a file. Rach2: `rach2/` (the kernel), `utils/`, `ish/`, `tests/`, `man/`, `vendor/`, `tools/`, each component's `Readme.md`. Upstream sources on hand under `build/research/src/`: libdrm, libinput, libudev-zero, linux, mesa (25.0.7), proton, seatd, wayland, weston, wine. QEMU, virglrenderer, kvm-unit-tests and the rest are not on hand: read them on the web (GitLab and GitHub raw files) when you need them, and download nothing.
* `graphics-research/round3/`, `second/` and `comparison.md`: the evidence behind the plan, when a task's reason is unclear.

## What to write

One file per milestone, `build/flow/coordinator/graphics-research/breakdown/NN-name.md` (`73-display.md`, `74-vgpu.md`, `75-intel.md`, `76-ia32.md`, `77-games.md`, `78-kvm.md`, `79-assign.md`, `80-testbed.md`), in the form of `dev-log/70-wifi-chip/plan.md`:

* `# Title`, then `## The owner's words and decisions`: the owner's words and decisions from `plan.md` that bind this milestone.
* `## Measured`: what the milestone needs from a machine, either already measured (cite it) or to be measured as its first subtask, and how.
* `## The split`: why the tasks are cut as they are, and every file two tasks could both want, with its one owner or its rule for sharing.
* `## Tasks`: for each task, `### N. Title` with plan.md's number (73-1 becomes `### 1.`), then one paragraph: `Owns` every file the task creates or changes, `Waits on` the tasks it needs, in this milestone or another (by number, such as 76-3). Then the subtasks, lettered (`* 1a.`, `* 1b.`), each a sentence or two: what it makes true and in which file. Tests and documents are subtasks of their own: the host test or suite case, the `Readme.md` section that owns the contract, the `man/` page of every program, BusyBox applet or system call added or changed. Then `* Done:` an observable result (a test that passes, a probe line, a number measured against Linux) and where it is seen (host test; QEMU on the Mac or the testing PC; the Lenovo on a trial boot). Mark a step `owner-only` when it needs the owner's accounts (the Lenovo, the owner's hardware) and `linux-host` when it builds the GNU layer. Give each task's size in lines of new code.
* `## Shared with other milestones`: every file or interface this milestone changes that another milestone also changes or relies on, and which milestone defines it.
* `## Done`: the milestone's, from `plan.md`.
* `## Open questions`: what the code could not settle, and what would settle it.

## Rules

* `plan.md`'s tasks are the frame: keep their numbers and their Done. Split a task too large for one worker into two (`73-4` into `### 4.` and `### 4b.`), or merge two that would collide, and say why under The split.
* Tasks never share a file, and rarely wait on each other. A manifest or table that many tasks extend (`rach2/sources.mk`, `utils/programs.mk`, the two `systab.c`, `rach2/syscalls.md`, `man/`) is named with its rule: lines added in order that rebase cleanly, or one owning task that adds them for the others.
* Every file you name exists (cite it, `path:line` for a claim about its code) or is marked new. A claim about upstream code cites its file and line, or its URL.
* The kernel is freestanding C89 that builds with `i686-elf-gcc`, `x86_64-elf-gcc` and the staged TinyCC; Linux's uAPI numbers and structures; the licenses `plan.md` names.
* Tests follow `AGENTS.md`'s Testing section: a suite judges what the kernel counts, never the clock; a test removes what it makes. During a milestone nothing is checked but what a worker needs to see; style, the host tests and the suites run at its end and in its release.
* No dates and no estimates of human time. Size work by lines, files and tasks.
* Write as `AGENTS.md`'s Writing section says: terse and exact, ordinary words, ASCII only, `*` bullets, no em-dashes, no wrapped prose.
* Read; change nothing but your own output files. Start no subagents. Make no GitHub call (the owner has stopped all GitHub use), download nothing, and run nothing on any machine but the Mac's reading tools.
* Keep a short notes file beside each output (`NN-name.notes.md`) and write the output as you go, so that a compaction does not lose your place.
* When done, reply with each output's path and line count, the number of subtasks, and the three points you are least sure of.
