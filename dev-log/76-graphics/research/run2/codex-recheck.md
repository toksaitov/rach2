Paths: `plan.md` and `run2/` are under `build/flow/coordinator/graphics-research/`. `report.md` is `dev-log/76-graphics/research/run2/report.md`. `git-rebase.1` is `/Library/Developer/CommandLineTools/usr/share/man/man1/git-rebase.1`.

1. Fixes not applied, or applied wrongly

* 17: the shared-file order exists, but the task dependencies contradict it. 76-19 must follow 76-9 for `procfs.c`; 76-2 must follow 76-19 for `syscall_proc.c`; 80-10's shared-file work must follow 76-19 too. The columns omit these waits (`plan.md:184`, `plan.md:201`, `plan.md:285`, `plan.md:347`, `plan.md:348`).
* 24: applied as requested, but the requested figure was wrong. Five Proton units return 5; `d3d9:visual` returns 245. Match each recorded exit (`plan.md:203`; `run2/step3-4/out/step3/8/wine-1/summary.txt:3`, `:4`, `:6`).
* 24: "neither is a harness kill" also needs qualification. WineD3D's dxgi stalls first, but the recorded timed run ends through `timeout`, returning 124 at 120 seconds. Keep progress-based acceptance and distinguish the stall from its termination (`plan.md:203`; `run2/step3-4/out/step3/8/wined3d-1/units-time.txt:13`; `run2/step3-4/scripts/guest/run-wine.sh:42`).
* 18's release-only handoff was superseded by stacking; its package move was applied. 19's deferred SHA-256 is acceptable if the SHA-512-verified archive yields a fixed SHA-256 manifest pin. The fetcher requires that expected value; computing a digest alone is not verification (`plan.md:195`, `plan.md:339`; `run2/step2/out/dl-geproton.txt:2`; `vendor/gnu/fetch.sh:59`, `:72`).

2. New errors

* 77-8 waits on 80-2, but 77 stacks only on 76, while 80 imports 77. The blanket requirement to merge a blocker's milestone would create a 77/80 release cycle. Declare this Linux trace an evidence-only handoff, requiring no branch merge (`plan.md:173`, `plan.md:223`, `plan.md:339`, `plan.md:341`).
* 77-6 still probes both bitnesses on Rach2, but neither its waits nor its stack includes 79-1's WoW64 support. Move its Rach2 32-bit acceptance into 79, or add the prerequisite and corresponding release order (`plan.md:221`, `plan.md:256`, `plan.md:339`).
* 80-1 now prescribes `WINE_D3D_CONFIG=renderer=vulkan` as a game-route setting. Crysis crashes on that renderer; the chosen route is GL. Keep Vulkan in 80-5's research comparison (`plan.md:276`, `plan.md:163`, `plan.md:280`; `report.md:79`).
* The unsupported new exit-code figure is covered under fix 24. I found no stale 82 task reference or newly imposed timeout in a Done; the explicit count-based rule remains (`plan.md:313`, `plan.md:316`, `plan.md:318`, `plan.md:332`, `plan.md:366`).

3. Stacking and release

* Sound in principle, but the proposed policy update is incomplete. Changing only the milestone skill leaves AGENTS.md's requirement to branch from master unchanged. Update both before opening stacked milestones (`plan.md:502`; `AGENTS.md:49`; `.agents/skills/milestone/SKILL.md:18`).
* Merge commits can be pushed by fast-forward. The push hook checks the resulting file diff; it does not prohibit merges or enforce linear history. Linear master remains a release obligation (`AGENTS.md:133`; `tools/git-hooks/pre-push:10`; `tools/verify/hookcheck.sh:333`, `:383`).
* Plainly: merging an open prerequisite, then rebasing onto master after its release, does not inherently lose or duplicate ordinary work. Git excludes upstream commits and skips equivalent patches, including unchanged patches whose commit identities changed during the prerequisite's rebase (`git-rebase.1:159`, `git-rebase.1:1927`).
* The promise that "only its own commits remain" is not unconditional. Default rebase drops merge commits, including edits recorded only in merge resolutions. If the prerequisite's release rebase changes patches, old variants may be replayed. Record consumed prerequisite tips and preserve resolution work when selecting and reviewing the consumer's final commits (`plan.md:341`; `git-rebase.1:838`, `git-rebase.1:1913`, `git-rebase.1:1976`).
* Rebase and force-push only at the consumer's end, with its writers stopped; then review and prove the resulting head. Releasing prerequisites first is compatible with that rule (`AGENTS.md:133`; `.agents/skills/milestone/SKILL.md:35`, `:36`).

4. Ready for the owner's yes

* Not yet. The one structural blocker is the inconsistent cross-milestone handoff contract, concretely the 77/80 cycle. Separate evidence handoffs from code imports and reconcile the dependency tables with the stack graph. The factual corrections above need edits, not another discovery run (`plan.md:173`, `plan.md:223`, `plan.md:339`; `run2/rulings.md:16`).