Paths below: `plan.md`, `plan-v5.md` and `tonight/` are under `build/flow/coordinator/graphics-research/`; `rulings.md` and `step*/` are under its `run2/`; `report.md` is `dev-log/76-graphics/research/run2/report.md`.

1. Wrong

* `plan.md:9`: "your criterion, which Linux meets at 44 to 46 FPS". The criterion includes the first level, still unplayed (`plan.md:137`; `report.md:86`). Replace with: "Linux's island flyby exceeds 24 FPS; the whole first level and working-sound route remain unproved."
* `plan.md:22`: "worse than WSL on every measure taken but one". Windows also wrote 2 GiB faster: 1.365 s against 1.769 s (`report.md:20`). Qualify this as the graphics-path comparison. `rulings.md:9` repeats the overstatement; the WSL recommendation still stands.
* `plan.md:24`: "26 program rows ... exit status and pixels". All rows matched exit status; pixels matched where a probe read them back (`report.md:71`). Use that qualification.
* `plan.md:38`, `plan.md:369`: "i915's huge-page objects". The observed trigger is `shmem_enabled=always` versus `never` or `deny`; the failing page was not inspected (`report.md:164`). Replace the claimed backing with the measured configuration and state that the page-level cause remains untraced, as `plan.md:289` already does.
* `plan.md:124`, `plan.md:195`: "renderers, storage and network services sandboxed". The network service had seccomp 0 and host namespaces (`report.md:28`; `step2/out/br/s2-chromium-default.procs.txt:6`). Replace with each process's measured Debian state. `rulings.md:6` contains the same inconsistency; its Linux-matching requirement and this factual description need reconciliation.
* `plan.md:199`: interrupted Wine units "are matched on where each stops". That contradicts the ruling that interrupted units are incomplete (`rulings.md:15`; `report.md:40`). Require complete runs; keep the interrupted results as evidence without counting them as Done.
* `plan.md:239`: "the WebGL suite at 99.72 percent ... and 97.95". These are CTS 2.0.1 results without `deqp/`, obtained beside builds (`report.md:143`). Add both qualifications. They do not establish the whole-suite reference required by `plan.md:195`.
* `plan.md:325`: "to map", "to free" and "the next ... map". Preserve the measured operations: map plus first touch; unmap plus free; and allocation plus map and first touch for 116.8 ms (`report.md:94`, `report.md:96`; `step10/blobtest.c:270`).
* `plan.md:236`: panel link, CD clock and power timings "saved in `run2/step11/facts/`". Those figures come from `tonight/lenovo-out/debugfs/i915_display_info:9`, `tonight/lenovo-out/debugfs/i915_cdclk_info:1` and `tonight/lenovo-out/debugfs/eDP-1-i915_panel_timings:1`. Correct those source paths; the figures agree.
* `plan.md:453`: "futex has only wait and wake". Add their bitset forms, implemented at `rach2/sys/proc/futex.c:465` and `rach2/sys/proc/futex.c:472`. The missing operation relevant here remains `futex_waitv`.

2. Renumbering

* I found no stale graphics milestone or task number. The mapping at `plan.md:11` is consistent with the task tables; `73-15` at `plan.md:5` is explicitly the old-number example.
* The Wi-Fi numbers remain 73 to 75 (`plan.md:5`), and milestone 70's references remain 70 (`plan.md:361`, `plan.md:464`).
* Draft 5's release 71 prerequisite was removed, not wrongly changed to 74 (`plan-v5.md:254`; `plan.md:271`). The reference session is complete (`rulings.md:10`).
* I found missing dependency edges, below, rather than cross-references accidentally redirected by renumbering.

3. Milestones

* The execution plan is not ready for approval. File ownership, subtasks and task Dones are deferred until afterwards (`plan.md:484`), although `AGENTS.md:47` requires them in the plan.
* The parallel work shares existing files. Discovery, compatibility files and Wine's sysfs work reach `rach2/sys/fs/procfs.c:5504`; mappings and scheduling reach `rach2/sys/syscall_proc.c:6354` and `rach2/sys/syscall_proc.c:7250` (`plan.md:180`, `plan.md:187`, `plan.md:197`, `plan.md:279`). Assign integration ownership or order those edits.
* Define cross-milestone code handoffs. Board order does not put a completed provider task onto a consumer's branch (`plan.md:169`; `AGENTS.md:57`). This already matters for 76-21 consuming 80-1 (`plan.md:199`).
* Correct the dependency columns: 76-14 needs the display, discovery, input and console work; 77-4 consumes 77-7's selected driver; 80-3 needs 80-1 and 77-2; 82-5 needs working VMX, interrupts and QEMU (`plan.md:192`, `plan.md:214`, `plan.md:272`, `plan.md:311`). Split early preparation from integration where waiting on the whole task would waste workers.
* 76 has evidence for its headless software foundation, not its completed desktop (`report.md:72`). Its software-video Done needs a matching Linux reference with llvmpipe and CPU decoding; the measured Firefox H.264 reference uses GPU decoding (`plan.md:201`; `report.md:141`).
* Taking 76-20 makes 76 depend on Crocus from 78 (`plan.md:198`, `plan.md:231`, `plan.md:234`). Split software and Intel acceptance, or explicitly join those releases.
* 77 remains conditional on stack qualification. No evidence yet establishes that Mesa 26.1.6 supplies the required Venus path (`plan.md:217`; `report.md:213`). Its current refusal-does-not-complete rule is right (`plan.md:219`).
* 78-1 needs a Rach2 watchdog driver, arming/feeding path and expiry proof. Its stated qualification is under Debian, and Rach2 has no TCO driver (`plan.md:227`; `report.md:111`).
* 78-3's eviction promise needs a measurable Done. "Memory pressure ending in a clean error" can pass without eviction (`plan.md:229`, `plan.md:242`). Exceed the aperture while fitting in RAM, verify contents after rebinding, and compare success or failure with Linux.
* 79's "covered or marked with a reason" permits unapproved omissions (`plan.md:260`). Require Linux-matched behavior or an approved scope exclusion. Apply the same restriction to unexplained differences and open-ended skip lists in 78 and 81 (`plan.md:242`, `plan.md:297`; `AGENTS.md:24`).
* 80's route is credible, but flyby speed does not finish gameplay acceptance. The retained first-level, fullscreen, sound and save work is necessary (`plan.md:271`, `plan.md:273`, `plan.md:278`).
* 81's TCG SVM path still needs its stated qualification; 82's Linux assignment route is demonstrated (`plan.md:294`; `report.md:112`). Neither establishes Rach2's implementation.
* 83 remains research: the reversed mapping and Haswell native context are unproved (`plan.md:325`, `plan.md:326`). Its task sequence can investigate them; success must remain conditional.
* Assign desktop recovery after VM death. Failing fences and revoking mappings do not themselves select llvmpipe and restore the compositor (`plan.md:323`, `plan.md:325`, `plan.md:329`). Specify whether restarting the compositor satisfies "the desktop goes on".
* The Wine stopping rule above is the remaining timeout-related acceptance fault. The ten-minute Windows launch is a bounded investigation authorized by the ruling, not a suite's Done (`plan.md:194`; `rulings.md:16`). Keep that distinction.

4. Decisions A to D

* A is the owner's host-configuration and scope decision (`AGENTS.md:33`). The module recommendation is supported, including its shared WSL panic and rebuild costs (`report.md:58`). But choices 2 and 3 need different Dones: the Debian fallback only has off-screen readback, and "neither" drops presenting and Direct3D on Venus, while 77's Done requires both (`plan.md:115`, `plan.md:116`, `plan.md:219`).
* B correctly asks whether to exceed Linux's measured behavior. Option 1 is supported; option 2 honestly has unknown cost (`plan.md:118`, `plan.md:120`; `report.md:80`, `report.md:84`). Baseline Linux matching itself is already settled by `AGENTS.md:24`.
* C is a product-default choice for the owner. Test-only is the supported recommendation; the measured switch gives the Intel adapter (`plan.md:121`, `plan.md:122`; `report.md:81`). Its working probe does not establish general reliability.
* D is explicitly the owner's optional additional scope under the binding ruling (`rulings.md:6`). Matching Linux is the supported default; taking the fix requires the release dependency correction above (`plan.md:125`, `plan.md:198`).
* The trial root is already accepted (`plan.md:144`). Remove "if you agree" and "Put to you" at `plan.md:334` and `plan.md:442`. The later ratification of the performance gate remains explicit in both drafts (`plan-v5.md:140`; `plan.md:147`).

5. What draft 5 had right that draft 6 lost

* Restore GE-Proton's SHA-256 pinning requirement (`plan-v5.md:253`). An abbreviated SHA-512 record is insufficient integration work (`plan.md:270`); the fetcher verifies SHA-256 (`vendor/gnu/fetch.sh:9`, `vendor/gnu/fetch.sh:71`; `AGENTS.md:96`).
* Restore the early Linux KVM measurement subtask. Draft 5 placed it in the early reference session; draft 6 makes measurement wait on the implementation while still promising early measurements (`plan-v5.md:277`; `plan.md:295`, `plan.md:363`).
* Restore independent WebGPU scheduling. Draft 5 explicitly freed it from Proton bring-up; draft 6 makes all of 77-6 wait on 76-21's Direct3D qualification (`plan-v5.md:202`; `plan.md:216`). Split the acceptance work.
* Do not restore draft 5's mandatory sandboxed GPU process: its removal is required by the ruling (`plan-v5.md:186`; `rulings.md:6`).