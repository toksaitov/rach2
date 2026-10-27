# The audit of the phase

This is the source audit for T96 of [the plan](plan.md). Its first pass, under #957 at 02661fb8a on 23 October 2026, read each task's done condition against the code, cases and closing commits on master. The recovery pass at 39dd88850 on 24 October updates the changed obligations below. The coverage count below was checked again at 97d26927d on 25 October. The pass of 27 October records the final whole runs. On that day the owner ruled that one complete run of each machine, made after every known failure was fixed, is the acceptance of T90 to T92 and the measurement of T94.

A line gives the task, its issue, its original closing commit and the implementation or evidence. An original task's closure does not accept its replacement in the recovered native collection. [Recovery](recovery.md) requires that collection to pass on every comparison platform. Every retained host test is selected by `host.host.tests`, `host.host.etude` or `host.host.etude_quiet`; the native host inventory and prepared executable records identify what actually runs.

On 23 October the owner set aside the proof of a new test by a deliberate fault (aaf6dbc1c). 9dc209d2b removed etude's proof driver and the fault modes of the tests, and 92421ca2a the catalogues of `tests/proofs/` and the fault scripts. A done condition below that asks for a deliberate fault is read without that part, and its line says so.

## The tasks

* T01, #590, cff6e7822: [the inventory](inventory.md), its three parts and [the migration map](migration.md) give every unit a fate. Met; the inventory against today's cases is below.
* T02, #591, 0b46b3d7b: [the measurements](measurements.md), `tools/etude/schemas/history.schema.json`, and `history.example.jsonl` and `history.incomplete.example.jsonl`, which casecheck's self-test (`tools/etude/fixtures/expect`) validates. Met.
* T03, #592, c13ffe37e: `tools/setup/prereqs` and `prereq.sh`, which print the commands that need rights the account lacks; `tests/system/prereq_test.sh`. Met.
* T04, #593, e1d2efa47: [the native inventory](native.md) and [its guest programs](native-guest.md). Met.
* T05, #594, 5b2f27552: the headers of `tools/etude/include/etude/` and the schemas, with examples of a host program, guest programs that also run natively, a native invocation of `errnoprobe` and a scenario; the machine example is `tests/cases/machine.shutdown.json` since e0a8cd2f8. Met.
* T06, #595, 230d88557: casecheck and its fixtures; the `sleep`, `bound` and `skip` rules of `tools/verify/suitecheck.sh` in the style case. The #961 repair removed every exemption from `tests/system/unconverted`; only its explanatory comments remain. The final whole runs of T90 to T92 are its integrated acceptance.
* T07, #596, 9e4ca92d7: `tools/etude/history.c`, `history_check.c` and `history_collect.c`; `tests/host/etude_history_hosttest.c`. Met.
* T08, #597, 6548c7fc0: `tools/etude/posix/exec.c`, `keeper.c` and `process.c`; `tests/host/etude_exec_hosttest.c`. Met.
* T09, #598, be2fe24de: `tools/etude/win/`; `tools/etude/wincheck.sh` checks that the programs import no MSYS or Cygwin runtime and runs the Windows fixtures; `tests/host/etude_winfmt_hosttest.c`. Met.
* T10, #599, fbc21bff6: `tools/etude/posix/qemu.c` and `qemu_profile.c`; `tests/host/etude_qemu_hosttest.c`. Met.
* T11, #600, 3666b12e8: `tools/etude/guest/`, `tests/host/etude_guest_hosttest.c` and `etude_serial_hosttest.c`. The redundant `machine.etudeguest.serial` case was removed under #924. The retained tests run the actual helper over stdio and serial, including outcomes, transfers, hashes, cancellation and framing errors; retained machine cases exercise that helper on Rach2. [The case audit](924-audit.md) records the equivalence.
* T12, #601, 245b949c9: the Rach2 build of etude by TinyCC and the family `machine.etudeexec`, which runs the executor's test on Rach2 under QEMU. Met.
* T13, #602, 22e2ee9da: `tools/etude/bootstrap.sh`, `version.sh` and `version.mk`; `tests/system/etude_version_test.sh`. Met.
* T14, #603, 58d24fa76: `tools/etude/compare.c`, `baseline.c` and `history_compare`; `tests/host/etude_compare_hosttest.c`. Met.
* T15, #604, aac4fd270: `tools/etude/watchdog.c` and `supervise.c`; `tests/host/etude_watchdog_hosttest.c`. Met; since the owner's decision of 23 October (#898) a case is stopped only for a lack of progress, as `AGENTS.md` says.
* T16, #605, 7d58df32e: `tools/etude/ledger.c`; `tests/host/etude_ledger_hosttest.c` and `etude_ledgercmd_hosttest.c`. Met.
* T17, #606, 52093f678: `tools/etude/posix/remote.c`, `serve.c` and `etude_remote.c`; `tests/host/etude_remote_hosttest.c`. Met.
* T18, #607, 3479bec8b: `tools/etude/sched.c` and `cost.c`, and admission in the ledger; `tests/host/etude_sched_hosttest.c`. The #962 repair gives the public native entry both architectures in one allocation and schedule. The final whole runs of T90 to T92 ran both architectures in one allocation and schedule.
* T19, #608, 8fbeeb797: `tools/etude/posix/store.c` and `prepare.c`; `tests/host/etude_artifact_hosttest.c`. Met.
* T20, #609, 689d56533: libetude-case, `tools/etude/posix/scenario.c` and `case_main.c`; `tests/host/etude_scenario_hosttest.c`. Met.
* T21, #610, 5194fd845: the services and machines of `scenario.c`; `tests/host/etude_scenario_hosttest.c`. Met.
* T22, #611, b3d1a88e5: `tools/etude/deploy.sh`; `tests/system/etude_deploy_test.sh`. Met.
* T23, #612, 4e0039eea: `tools/etude/posix/etude_host.c` and the family `host.host`, every sanitizer report fatal. The recovered native host inventories require every selected check and executable, with no missing-tool skip. The final whole runs of T90 to T92 ran the native host checks on each machine; the old tooling-machine execution is superseded by [recovery](recovery.md).
* T24, #613, a060faffa: `tools/etude/repeat.c`, `posix/repeat.c` and `posix/load.c`; `tests/host/etude_repeat_hosttest.c`. The original library has fixture evidence, but the recovered public collection did not call it or expose shuffle and controlled load. #990 supplies that integration; the focused repeated and loaded runs were not made, under the owner's ruling of 27 October.
* T25, #614, a88d8f766: the proof driver met its condition when it landed; 9dc209d2b removed it under the owner's decision. Nothing is left.
* T26, #615, f4b5b5b90: the process and protocol catalogues met their condition when they landed; 92421ca2a removed them under the owner's decision. Nothing is left.
* T27, #616, e265c0956: the storage and image catalogues, likewise removed by 92421ca2a. Nothing is left.
* T28, #617, 85b92e53f: `tests/musl/pselectmusl.c`, `preadv` and `pwritev` in `tests/musl/preadmusl.c` (661fad06f), and `tests/host/krandom_hosttest.c` for the generator. Met, without the deliberate faults.
* T29, #618, 7e7daae25: `tests/host/ps2mouse_hosttest.c` runs the whole bring-up against the existing 8042 model. Met.
* T30, #619, d07c2d7e7: `tests/fuzz/elf64_fuzz.c` and its corpus, which the case `host.host.fuzz` replays under the sanitizers. Met, without the deliberate fault.
* T31, #620, 2d8c219bc: the guest copies are gone; the host inventory keeps the board, limited, release_resume, tidy and worker_park checks. The removed `machine.agents.procfd` assertion now runs as `guest.default.proclinkmusl`'s `fd-directory`, with stat and S_ISDIR before descriptor-link checks. [The case audit](924-audit.md) records that transfer; its integrated run is pending.
* T32, #621, cda41cfb7: `tests/host/pwauth_hosttest.c` counts retries. The #961 repair removed its sleep exemptions.
* T33, #622, 697c56444: the probes wait for events. The #961 repair removed the remaining undeclared waits and the exemption list is empty. The final whole runs of T90 to T92 are its integrated acceptance.
* T34, #623, 025afa67d: the resize stopped at a witnessed write under e2fsprogs' test I/O manager, now in the families `machine.grow` and `machine.installer-repair`. Met.
* T35, #624, fccc4fa8b: SSH tries start at init's record of the server's start in `machine.metal` and `machine.diskdiag`; Doom's frames in `machine.doom` (4377f51d8); uefi by events (T81). Met.
* T36, #625, 3c2b1ba06: the hold and answer controls of `rach2/sys/block/lag.c`; `tests/host/request_hosttest.c`. Met.
* T37, #626, baab52ba2: `tests/host/ahci_hosttest.c`, `request_hosttest.c` and `tests/ahciprobe.c`. Met.
* T38, #627, a4ad4ca68: the waits for `/proc/usb`'s and the other records in place of spins and counts of tries. Met.
* T39, #628, 34e70d2b0: `tests/clockref.h`, `tests/musl/clockmusl.c` and `clockloadmusl.c`; the family `machine.clock`. Met.
* T40, #629, b92256725: `rach2/sys/arch/x86/refclock.c` and `apictimer.c`; `tests/host/timercal_hosttest.c` and `rtc_hosttest.c`. Met.
* T41, #630, 784a68b2a: `rngprobe` and `xsaveprobe` report their branches, which casecheck judges against the processor's features (`report-rng*.out` and `report-xsave-*.out` in its fixtures), in `guest.default`, `guest.fpu` and `guest.noentropy`. Met.
* T42, #631, 98d0f3899: `tests/system/checkers/wavcheck.c` and the family `machine.audio`, with no accelerator forced. Met.
* T43, #632, 9c0d2d214: the debug-entry rows of `tests/musl/ptracemusl.c` on both architectures. Met, without the deliberate fault.
* T44, #633, 4bfc09300: `tests/native/` and `tests/native/machine`, which `tools/verify/nativecheck.sh` in the style case checks against every built program. Met.
* T45, #634, 2cb42861a: the original tooling-machine contract is superseded by [recovery](recovery.md). Source production may use its declared build tools. Execution uses prepared native controllers and declared Rach2 helpers, with their resources and costs recorded. Missing or unsupported checks fail inventory validation.
* T46, #635, 2a495869e: the lazy boundary profile (ee9769ac9) and the site's permission (afdce9108); `tests/host/etude_scenario_hosttest.c`; [the memory record](memory.md). Met.
* T47, #636, 6b36893f5: `tests/host/etude_runtime_hosttest.c`. Met.
* T48, #637, ffa6b51c9: [the Mac's acceptance](mac.md) and [its artifacts](mac-artifacts.md) record the original implementation. The recovered native entry and artifact import require the complete T91 run; these earlier records do not accept that replacement.
* T49, #638, 003c9e59b: `tools/etude/win/winrunfix.c` and the 40 fixtures of `wincheck.sh` on Windows. Met.
* T50, #639, 96e01a721: `tests/native/tccbuild.sh` and the native transport adapter passed the four TinyCC modes over actual SSH on the Lenovo. [Acceptance record](t50-ssh.md), #963. Met; T93 remains separate.
* T51, #640, 8fb0e87b6: `tools/etude/posix/bridge.c` and `etude-bridge`; `tests/host/etude_bridge_hosttest.c`. Met.
* T52, #641, e25ba65f7: `etude-legacy`'s preparation; `tests/host/etude_legacy_hosttest.c`. Met.
* T53, #642, 95c59a1e0: the legacy suites' supervision met its condition; with every suite converted, T88 removed it (07fc3cc70). Nothing is left.
* T54, #643, 00935f221: `tools/etude/posix/status.c` and `tools/flow/test_status.py`; `tests/host/etude_lines_hosttest.c` and `tests/system/test_status_check.sh`. Met.
* T55, #644, 625efc989: `tools/ci/bisect.sh` and `bisect.yml` through `etude-bridge`; `tests/system/ci_test.sh`. Met.
* T56, #645, 3bb17f632: [the Ubuntu enrollment](ubuntu.md) and the installed policy `/etc/rach2/etude-site`. Met.
* T57, #646, b237b85d4: the public `etude run` selects and schedules the recovered collection in C, including both architectures. The allocator export identifies every selected item. The retained legacy fixture tests the preparation contract; the final whole runs of T90 to T92 went through the public entry.
* T58, #647, 69b46641c: `tools/etude/budget.c`, `budget_prepare.c` and the budget fixtures. Native routine preparation now reads the selected site's reference history and forecasts its selection; native CI requires the structured budget verdict. T94 must supply actual reference samples and T95 must activate and exercise those entries. No forecast from an unmeasured reference is accepted.
* T59, #648, 618cdd9bf: `tools/ci/run.sh` judges a run by casecheck's verdict on its history; `tests/system/ci_run_test.sh`. Met.
* T60, #649, 49d52128b: `tools/ci/report.sh` and `history_failures`; `tests/system/ci_test.sh` and `tests/host/etude_failures_hosttest.c`. Met.
* T61, #650, 44db2e196: `tools/ci/bisect.sh` leaves a range unresolved for a runner's fault; `tests/system/ci_test.sh`. Met.
* T62, #651, 88e0c2b28: `tools/ci/site`, `site.sh`, `tools/flow/release.sh` and `full.yml`; `tests/system/release_resume_test.sh`. Met.
* T63, #652, 9440ee655: the testing and milestone skills, the Readmes and `testing.sh`. Met; the commands are renamed by #931, which rewrites them.
* T64, #653, 7fadbf02a: the family `host.style`; `tests/system/style_cases_test.sh` makes a source fault and a manifest fault fail. Met.
* T65, #654, a93f3b075: the family `machine.gnu`, with every row of `gnu_rows.sh` in the inventory a case. Met; #938 and #942 are faults of the GNU root's build.
* T66, #655, bdd2259f3: the families `machine.agents`, `machine.agentsaccept` and `machine.fakeapi`. Met; #941 is the Mac's lost SSH client.
* T67, #656, 26a4a83ce: the families `host.codexapi` and `host.museapi`. Met.
* T68, #657, 890a991bf: `guest.default`, `machine.memory` and `guest.pools`. Met; f5a0169d2 deleted eight `default` cases that others cover.
* T69, #658, b73748607: `machine.smp`, `guest.fpu`, `machine.clock`, `guest.noentropy` and `machine.bigmem`, its 72 GiB boundary boot kept. Met.
* T70, #659, 67794748c: `guest.authority`, `machine.account`, `machine.login`, `machine.cred-persist`, `machine.procfiles`, `guest.fdlock` and `guest.textbusy`. Met.
* T71, #660, 5876ce41b: `machine.console`, `conload`, `exitline`, `bootline`, `servicelog` and `man`. Met.
* T72, #661, 4377f51d8: `machine.audio` and `machine.doom`. Met.
* T73, #662, 477365f64: `machine.usb` and `machine.usbdisk`. Met.
* T74, #663, 53ae5152f: `machine.diskdiag`, `machine.diskpolicy`, `machine.diskrate` and `guest.disk-q35`; `disk` and `disk-ahci` deleted (b7d1cc2b4, 6cc611d81). Met.
* T75, #664, 9fd367018: `machine.persist`, `guest.diskfull`, `guest.fault`, `guest.queue`, `machine.load`, `guest.stress` and `guest.stress-ahci`; `persist-ahci` deleted (b6ee54072). Met.
* T76, #665, 9ee6fedc1: `machine.fatwrite`, `machine.exfat`, `machine.ext2` and `machine.large`. Met.
* T77, #666, 7ab796ef8: `machine.mount`, `machine.krffsmount`, `machine.loopdev`, `host.krffscheck` and `guest.subset`. Met.
* T78, #667, 479e63912: `machine.parts`, `machine.bootpart`, `machine.bus`, `machine.bootorder` and `machine.grub`; `disks` deleted (ef7d2617d). Met.
* T79, #668, 2599fad17: `machine.install`, `machine.menu`, `machine.sealed` and `machine.drill`. Met.
* T80, #669, 153aecd7d: `machine.installer`, `machine.installer-repair`, `machine.installer-rehearse` and `machine.grow`. Met.
* T81, #670, eef66a1eb: `machine.uefi`. Met.
* T82, #671, db9aada70: `machine.selfbuild`, `machine.selfbuild-ext2`, `machine.tcc` and `machine.make`. Met.
* T83, #672, e0a8cd2f8: `machine.crash`, `machine.reset` and `machine.shutdown`. Met.
* T84, #673, 998fb18f4: `machine.socket`, `machine.dgram`, `guest.ping`, `guest.packet` and `machine.raw`; the `-e1000` twins deleted (c94cd3145, 0223ea8cf, 61f1c0f97, 59d8c622b). Met.
* T85, #674, bed765b2e: `machine.dhcp`, `machine.netif` and `machine.addressed`; the `-e1000` twins deleted (d5969a8bd, 014a159dc). Met.
* T86, #675, 51c280820: `machine.ssh`, `machine.e1000`, `machine.netconsole`, `machine.netconsole-e1000` and `machine.metal`; `ssh-e1000` deleted (decdd3508). Met.
* T87, #676, 5a6f57c3b: `host.imagerace`. Met.
* T88, #677, 2470763f8: no suite is a make target, and `tests/run` runs only cases. Met.
* T89, #678, 4b82fa153: the old harness, runtests, runutil and their tests, with the `runner` and `drive` suites, removed (9da97a088, #875). Met.
* T90, #679: the whole run `phase589-linux-a720d347d8ba-clean-01` on a720d347d (#1314), both architectures in one schedule: 1033 cases, 1032 passed. The one failure, `machine.agentsaccept.claude` on x86_64, was a fault of the test, fixed by a25201d5e (#1322). Met under the owner's ruling of 27 October; the earlier [Ubuntu runs](ubuntu-run.md) are historical adapter results.
* T91, #680: the final Mac run `phase589-macos-4c323664a065-clean-01` on 4c323664a: every one of its 1033 cases passed, in 9622 s, with cleanup confirmed (closed 27 October). The Mac's run 3 had failed three cases, whose fixes are on master.
* T92, #681: WINRESULT
* T93, #682: [the final Lenovo record](native-final.md) binds all 250 eligible cases to the exact repaired kernel, complete results and independent cleanup. The whole selection passed 249 cases; an ordinary-init reboot and unchanged procps retry passed the remaining case. The original failed attempt and diagnostic histories remain separate. Normal boot, network, root and original account restoration were verified. Met for that native candidate; desktop comparison and release remain separate.
* T94, #683: [the comparison of the test machines](machines.md) recommends RECO and proposes its site profile. The Lenovo's smaller selection is not ranked.
* T95, #684: waits for the runs.
* T96, #685: this audit, with the reviewer loop and the release to follow.

## The coverage inventory

The source at 97d26927d has 89 manifest families and 553 cases. The public both-architecture allocation contains 1033 items: 485 i686, 507 x86-64 and 41 without an architecture. All 1024 earlier case, architecture and repetition tuples remain. Splitting the scripts into five independent cases added eight tuples across both architectures; separating the two quiet Etude checks added one. The 45 original script assertions per architecture and 30 Etude assertions remain exactly once across their partitions, in their original order within each partition. Each partition also checks its own declared inventory. The e1d41bc63 selection record has SHA-256 `ad5b945c996174ace12663016842598999913683f6b7ae1be6505b17af4d8e42`; the manifests and host lists are unchanged at 97d26927d.

The 101 original suites of [the inventory](inventory-suites.md) became 88 retained families and 13 recorded removals; the additional retained family is `machine.etudeexec`. This phase's additional `machine.etudeguest` family was later removed under #924 with its assertions covered as T11 describes. `runner` and `drive` went with the old runner (9da97a088, #875). The twins `dgram-e1000`, `dhcp-e1000`, `netif-e1000`, `packet-e1000`, `ping-e1000`, `raw-e1000` and `ssh-e1000`, and `disk`, `disk-ahci`, `disks` and `persist-ahci`, were deleted on 23 October under #921, #922 and #923, each commit naming the case that covers its behavior.

The 67 retained GNU rows are cases of `machine.gnu`; [the case audit](924-audit.md) records the removed GNU and agent rows and their retained assertions. `stackoverrun_row.sh` is in `machine.memory` and `gpt_write_refusal.sh` in `machine.parts`.

Of the scripts, the kept ones that are gone moved into cases: `ata_rate_test.sh` into `machine.diskrate`, `krffscheck.sh` into `host.krffscheck`, `imagerace.sh` into `host.imagerace`, `clockload.py` into `machine.clock`, `init_refusal.sh` into the console families, the grow scripts into `machine.grow`, `installed_test.sh` and `ssh_flood_test.sh` into `machine.ssh` and `machine.metal`, and `usb_latency.sh` into `machine.usb`. `probe_roots.sh` lost its callers and went with the harness; the ext2 family runs the probes on an ext2 root, and the selfbuild families each probe of the list. [The conversion of the old programs](old-programs.md) records these moves.

Of the guest programs, every one is named by a case or a native case but nine that were deleted with a reason: `cowprobe`, `filetest`, `forkleak`, `touchtest` and `musl/timemusl` (f5a0169d2), `vmro` (2e88b17da), `placeprobe` (47ecd2fd6), `disksprobe` with its suite (ef7d2617d) and `calibprobe` with `calibrate.sh` (109d69662).

The host tests, the checkers' self-tests, the fuzz harnesses and the style checks are on `master` and selected, but for the old runner's self-tests, removed in T89, and the fault-text check and probe fault list, removed with the proofs. The proofs and the probe fault catalogue are gone under the owner's decision.

## The other issues of the phase

The original audit at 02661fb8a counted 322 phase issues: 100 sub-issues of #589 and the others whose body says `Part of #589`. Of these, 305 were closed, the tasks T01 to T90 among them. A commit on master with `Closes #N` had closed 283; each of the other 22 had a comment that gave the reason:

* A duplicate of another closed issue: #743 (#739), #805 (#803), #837 (#836), #880 (#879) and #944 (#942, which is open).
* Fixed by a commit without a `Closes` line: #825 (1f9697efb).
* A repair on a machine, with no change to the tree: #726, #732, #778, #793, #843, #850 and #928.
* A decision of the owner's: #878, the run under KVM dropped; #898, no new program with a fixed limit in seconds; #904, #905, #906 and #909, the proofs dropped.
* Nothing that a user, a test or the build can see: #721 and #794.
* A run whose result the issue records: #888, the Lenovo's candidate.

## Remaining acceptance

The former T50 gap, #963, is resolved by the [Lenovo SSH acceptance](t50-ssh.md). The source repairs for #961, #962, #950 and #951 are implemented as the task entries describe. Their existence does not establish a full native collection pass.

* T90 to T92 are met by each machine's final whole run above, both architectures together, with complete costs and confirmed cleanup. Under the owner's ruling each machine ran its own final candidate, not one candidate shared by all three.
* T93 is recorded by [the final Lenovo acceptance](native-final.md), including the failed process-name attempt, affected retry on the same kernel, exact 250-case index and independent restoration. The four T50 SSH outcomes remain a separate transport proof.
* T24's focused repeated and loaded runs were not made: the owner's ruling of 27 October closes the phase on each machine's one final run. The public shuffle, repetition and load of #990 are on master with their host test.
* T94 rests on one final run of each machine, which the owner's ruling puts in place of five runs of each configuration.
* T95 requires actual reference history, budget forecasts and public push, CI and release entry checks on the selected site. #991 supplies bounded history retention that preserves accepted baselines and evidence of open issues. Activation waits for T94 and for old jobs to finish.
* T96 is this audit; the reviewer loop and the release follow it.
