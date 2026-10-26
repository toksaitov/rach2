# The scripts left in the collection

This is the inventory that issue #977 starts from: every shell, Python or Perl script under `tests/` that the native collection still runs, and where each runs, at `master` 04153d8e8. Every case of every manifest in `tests/cases/` names a C program as its `execution.program`; the scripts below are run by those programs, by preparation, or by nothing in the collection.

Four groups are allowed by [the recovery's acceptance](recovery.md#acceptance): a product run in its own language when its behavior is what a case checks, a command run inside a Rach2 machine that is the system under test, a step of preparation, which may keep its declared build tools, and a host test that preparation places in the GNU helper machine. Two are the work left: the fake API's Python and the routine entry. Issue #1251 removed the third, the legacy commands of `host.list`, as [its section](#the-legacy-hostlist-commands) records.

## Removed with this inventory

These 20 scripts were the programs of cases, or the tests those programs ran, before the cases were converted to C. No manifest, C program, make rule or script ran them, and every assertion identity they reported is still declared by the manifest whose C program replaced them. Their sub-checks were compared with the C controllers that replaced them, row by row, for the five that were tests in their own right.

* `tests/cases/host/krffscheck.sh`: replaced by `tests/cases/host/krffscheck.c`.
* `tests/cases/host/imagerace.sh` and `tests/system/imagerace_probe.sh`: replaced by `host/family_scenario.c` with `host/family_image*.c`; the probe row by `family_image_probe` in `family_image_root.c`.
* `tests/cases/host/grow.sh`: replaced by `host/family_scenario.c` with `host/family_grow*.c` and `host/family_check.c`.
* `tests/cases/host/diskpolicy.sh`: replaced by `host/family_diskpolicy.c`, which still runs the product `tools/inspect/check_diskpolicy.sh`.
* `tests/cases/host/metalchecks.sh`, `tests/system/accept/transport_test.sh` and `tests/system/accept/password_test.sh`: replaced by `host/family_metal_transport.c` and `host/family_metal_password.c`, which keep all 34 transport rows and all nine password rows and still run the products `metal_accept.sh` and `su_command.sh`.
* `tests/cases/host/usbchecks.sh`, `tests/system/usb_latency_test.sh` and `tests/system/vfs_unmount_test.sh`: replaced by `host/usbchecks.c`, which judges the latency records with `tests/system/checkers/records.c` in place of `usblatency.awk`, and runs the `vfs_unmount` fixture that preparation builds.
* `tests/cases/host/fstool.sh`: replaced by `host/fstool.c`, run in the declared GNU helper machine.
* `tests/cases/machine/agent_accept.sh`, `agent_fakeapi.sh`, `agent_relay.sh`, `agent_rows.sh` and `agent_server.sh`: replaced by `machine/agents_scenario.c`, `agent_accept.c` and `agent_native.c`.
* `tests/cases/machine/grow_read.sh` and `installer_volume.sh`: replaced by calls of the compiled `mkexfatfixture` from the grow and installer scenarios, which keep its output whole.
* `tests/cases/host/agentapi.sh`: replaced by `machine/agentapi_scenario.c`. The agents layer supplies both clients; host prerequisites and the tooling machine install neither.

## Products run in their own language

A C controller copies the product into a private tree and runs it, in the GNU helper machine or on the host, because the product's behavior is what the case checks. These stay.

* Release and machine tools: `tests/system/accept/metal_accept.sh`, `su_command.sh` and `agents_guest.sh`; `tools/dev/machine_grow.sh` and `machine_grow_plan.sh`; `tools/inspect/check_diskpolicy.sh`; `rach2/run*.sh`, `debug.sh` and `qemu_devices.sh`.
* Build tools: `krffs/mkimage.sh`, `manifest_deps.sh` and `programs.sh`; `vendor/gnu/build_port.sh` and its port recipes; `tools/setup/prereq.sh`; `tools/etude/provision_machine.sh`, `launch.sh`, `hold.sh` and `make.sh`.
* Loop and ci tools: `tools/flow/board.sh` and `lib.sh`; `tools/ci/run.sh`, `report.sh` and `bisect.sh`; `tools/dev/testing.sh`; the measurement scripts of `tools/dev`.

## Commands inside the machine under test

These run on Rach2, in the machine a case boots, and exercise that system: `tests/system/gnu_rows.sh` (the 66 rows of `machine.gnu`, sent by scp), `tests/cases/machine/etudeexec_guest.sh` (etude built by TinyCC on Rach2), `tests/native/tccbuild.sh` (the workload of the native proof in `tests/native/proof.c`, which `tests/host/etude_native_hosttest.c` also runs in the GNU helper machine), the agent workloads of `tests/system/agents/*.sh` and `tests/system/agents_rows.sh`, which `machine/agent_native.c` runs with the GNU root's bash, and the command lines that the scenarios type at a console. None of them was a host driver.

## Preparation

Make runs these to build artifacts before any case starts. Preparation records what it made; a desktop that runs the collection does not run them.

* Disk and stick builders of `tests/system/suites.mk`: `agent_disk.sh`, `rehearse_fixture.sh`, `grow_fixture.sh`, `fixtures/grow_tool.sh`, `krffsvol.sh`, `loopdev_disks.sh`, `metal_sticks.sh`, `metal_installed_disks.sh`, `parts_disks.sh`, `usbdisk_sticks.sh`, `mkext2fixture.sh` and `mkrootdisk.sh`.
* Build helpers: `tests/musl/build.sh`, `tests/fuzz/build.sh`, `tests/system/fakeapi/build_openssl.sh`, `tests/host/mkexfat_external.sh` and `tests/system/tooling_ready.sh`.
* Source checks: `tests/cases/host/style.sh`, which `tools/etude/host_prepare_style.c` runs to record the evidence that `host/style.c` reads, and the tests it runs, `tests/system/*_test.sh` of the style list in [`tests/system/Readme.md`](../../tests/system/Readme.md).
* Python in preparation, which issue #1176 removes: `tests/host/fixtures/extract.py`, `tests/native/prepare.py`, `tests/cases/machine/prepare_codex_digest.py` and `tools/setup/prepare_clang.py`.

## Host tests in the GNU helper machine

`tools/etude/host_prepare.c` gives a host test of `tests/host` to the GNU helper machine when its sources call an interpreter, and names seven more that need its POSIX environment: `ext2`, `fat32`, `etude_volume`, `etude_compare`, `etude_step`, `etude_native` and `doom_platform`. On Windows every host test but the native platform checks goes there. These run the tree's own C under test with a shell or a product script as a fixture; they are host tests placed in a machine, not old drivers moved into one.

## The legacy host.list commands

`tests/cases/host/host.list` gave each of its 45 checks a COMMAND, the script that `tests/cases/host/host.sh` ran. The collection did not run these: `host_scripts_scenario.c` and `host.c` run each row's prepared C controller (`host_build_check`, `host_image_check`, `host_doom_check` and `host-flow-check` in the GNU helper, the kernel fixtures and `fakeapi_test` natively). `host.sh` was still run by `tools/verify/coverage.sh`, with `HOST_PRINT_COMMANDS`, and was the subject of `tests/system/host_suite_test.sh`, itself one of the commands.

The commands were 26 shell tests under `tests/system/`, seven Python tests (`progress_test.py`, `case_select_test.py`, `etude_launch_test.py`, `agents_credentials_test.py` and the three of `tests/system/fakeapi/`), seven kernel fixture scripts under `tests/host/` (`extract_check.sh` five times, `kstack_check.sh`, `endothers_check.sh`), `tests/host/krffs_tools_test.sh`, `tests/host/doom_libc_check.sh`, the two TinyCC checks of `vendor/tcc/`, and `etude_platform_hosttest --collection-only`, a prepared program. Each was compared with its controller before it went, so that an assertion the controller lacked was added or recorded as redundant. The comparisons are issues #1252 (the kernel fixtures and the fake API), #1253 (images), #1254 (build), #1255 (ci, board and release), #1256 (the loops' and the testing host's tools) and #1257 (etude's deployment, versions and tooling machine); each records its result below. Issue #1251 then removed the column and the scripts, as the last part of this section says.

### Etude deployment, versions and tooling

Issue #1257 compares the six commands of `host.host.scripts_tools`. Their controllers are in `tests/cases/host/` and run through `host-flow-check`. No script is removed by this comparison.

`etude_version_test.sh`, all 15 rows, against `flow_version.c`:

* `build`, `reuse` and `versions`: `version_reuse_identity` and `version_publication_identity` check the SSH command, publication paths, compile counts, compiler and flags, program digest, record key, unrelated changes, distinct source and compiler keys, and three versions with the first unchanged. `version_program` restores the exact single executable and single program-record checks for POSIX and Windows.
* `shells` and `busybox`: `version_account_shells` and `shell_row` build and reuse under sh, bash, each available optional shell, and BusyBox's shell and applets, with startup noise and a false result line. The script's fallback tooling transport is unnecessary because this controller already runs in the GNU helper with BusyBox.
* `term`, `kill` and `failed`: `version_interruption` checks unpublished failures, TERM cleanup, an abandoned KILL build without a version record, a fresh build after either signal, and the compiler failure diagnostic.
* `truncated`, `damaged`, `race` and `makeflags`: `version_truncation`, `version_source_damage`, `version_racing_publication` and `version_make_settings` retain all four truncation points, the whole-script control, damage refusal before compilation, both racing requests' result and one published build, and removal of inherited parallel make settings.
* `windows`, `revision` and `real`: `version_windows_version`, `version_revision` and `version_real_build` check the Windows command, cache, platform and executable; committed versus changed working source and restoration with two compiles; and the real compiler, schema and history examples, five argument-error forms and their diagnostics. The revision row changes native collection source, which the same version key includes. The controller also checks the native source set, exact byte boundaries and ordered source hashes.

`etude_deploy_test.sh`, all 18 rows, against `flow_deploy.c`:

* `deploy` and `again`: `basic_rows` checks installation, the three SSH calls, version path, exact machine record, initial ledger and state entries, clone and key snapshots, scratch removal, reuse without a compile, and an unchanged second deployment.
* `admin`, `sudo` and `check`: `prerequisite_rows` checks the printed administrator and account commands, missing prerequisites, installation through sudo, check-only refusal to install, and successful completion after repair.
* `busy`, `format` and `rights`: `busy_row` and `format_rights_rows` compare the active ledger, verdict, old version and process; preserve the machine record on an incompatible ledger; and check the permission diagnostic, unchanged refused ledger, and repair. The helper runs as an ordinary account, so the rights row always runs.
* `lost` and `stopped`: `interrupted_rows` loses each of the three SSH calls and resumes each, interrupts compilation before state publication, and interrupts replacement of the machine record while preserving the old record, then completes with the new version.
* `local` and `revision`: `local_revision_rows` checks no local SSH, an unchanged source clone with an untracked file, the chosen state directory, and use of committed ledger-format source despite a changed working file.
* `kvm-open`, `kvm-unavailable`, `kvm-missing`, `ovmf-discovery`, `ovmf-missing` and `windows`: `feature_rows` and `windows_row` check each KVM open result without changing or creating the device, firmware discovery and an absent override, and Windows prerequisite installation, both MSYS SSH calls, version path and exact machine record.

`etude_launch_test.py` against `flow_launch.c`, `flow_launch_entries.c` and `flow_launch_parallel.c`:

* `observer`, `launch_saw` and `launch_clear` check durable registration before execution, running state, memory and worker limits, absent processor and disk reservations, aligned port offsets, actual host and port locks, claim counts and shared or distinct identities, and release after each row.
* `build`, `jobs` and `worker` retain the real Makefile entry, quoted argument, explicit parallel limits, all eight make-wrapper argument cases, recursive make, and admission before the worker's compiler starts. `architectures` restores the missing `ARCH='i686 x86_64'` launch of `x86_64:default`.
* `owner`, `legacy`, `bisection` and `nested` retain quotes, stdin and exit 7; an occupied first port block and result file; installed policy before a private ledger; a shared nested claim and its separate result; signal status and incomplete result; and nested refusal of an old fixed-port runner.
* `bad_policies`, `bad_environment` and `launch_refusals` retain all five invalid policies, four invalid inherited settings, a bad ledger format, a fixed-port checkout, and the accepted boundary profile plus both refused alternatives, without executing a refused command.
* `parallel_row` and `cancellation` retain both ordinary accounts running together with distinct port blocks, serial and large jobs waiting then taking whole-site memory, the serial worker limit, and cancellation of waiting and running launches with all claims released.
* On a host with an installed authority, `/etc/rach2/etude-authority`, the copied launcher hands its launch to `etude authority launch`, so the Python script's `cancellation` stage tested that owner as well, which gave 125 for a cancelled launch until #1279 made it give 128 plus the signal. The controller's machine has no authority, and `etude_route_hosttest.c` checks the owner's status instead.
* The old CI row's four shell-entry claims tested the retired driver; `ci_unconfigured` checks native CI's refusal before work, while `etude_platform_hosttest --routine-entry-only` checks native projections, refusals and preparation order, and `flow_ci_run.c` checks CI result entries.

`test_status_check.sh` and `test_status_test.py` against `flow_status.c`:

* `claim_row` checks port, shared-host and exclusive-host waits, the UTC wait record, actual holder identity and path, an unchanged wait start across retries, and removal after admission. `two_holders` checks both shared holders.
* `pool_rows`, `owner_rows` and `zombie_row` check live and dead memory holders, fault reporting, reused run and tree-lock PIDs, stale and live tree locks, and an unreaped zombie reported dead. `flow_status` checks the final empty-run report.
* The shell wrapper's compile and Linux-tooling fallback are preparation work: the collection supplies the compiled Etude and runs this controller in the GNU helper, so neither wrapper branch is needed during execution.

`etude_tooling_test.sh`, all 12 rows, against `flow_tooling.c`:

* `made`, `digest`, `provision` and `unmade`: `preparation_rows` checks the read-only published overlay and tool record, relative backing image, scratch removal, setup history, check and reuse, refusal of a bad digest without publication, failed provisioning without publication, and status 126 before setup.
* `job` and `status`: `job_rows` checks file and directory inputs, exact stdout, stderr and cost report, attached disk contents, file and nested outputs, scratch removal, all three passing history stages, and exit 3 with failed execution and passing cleanup.
* `late`, `stop`, `crash` and `lie`: `late_stop_rows` and `crash_lie_rows` check statuses 124, 143 and 125, their diagnostics, QEMU termination, retained deadline evidence, removed cancellation scratch, incomplete and cancelled history, a crashed emulator, and disagreement with the volume's status.
* `line` and `arm64`: `command_rows` and `arm_rows` check all original PC and ARM arguments, firmware, accelerator, processor, disk and network settings, the default absence of a network, a distinct ARM cache key and successful job, and an unknown architecture refused. The old count of two cached disks is obsolete: the current cache prunes unused pairs after preparation, so `arm_rows` checks a distinct ARM pair and removal of the unused amd64 pair, as [the cache contract](../../tools/etude/Readme.md#the-tooling-machine) requires.

`agents_credentials_test.py`, all five tests, against `flow_basic.c` and `flow_agents.c`:

* `test_antigravity` and `test_antigravity_scan`: `credentials` and `credential_single` check the token's SHA-256, its absence, clean and leaking scans in both modes, filtered output and exact redaction.
* `test_selected_credentials` and `test_selected_secrets`: `credential_files` and `credential_scans` check each selected agent plus GitHub, both missing-file reports, exact row counts, clean and leaking scans, ignored unused credentials, a selected pair and the full set, in both modes.
* `test_redact_arguments`: `flow_credential_arguments` waits for the real awk descendant, inspects every descendant's arguments for the dummy token, and checks exact redacted output through its input pipe.

### Kernel fixtures and the fake API (#1252)

The controller of these ten rows is `host_scripts_scenario.c`, which runs each kernel fixture as `build/tests/host-suite/scripts/NAME`, built by `mk/host-fixtures.mk`, and the three fake API rows as `build/tests/system/fakeapi_test`, built from `tests/system/fakeapi/native/selftest.c` by `tests/system/fakeapi/native.mk`.

* `tests/host/extract_check.sh` (`chown`, `krffs_open`, `krffs_root_lock`, `krffs_stat`, `pagecache`): every check is in the controller. The script's checks are the fixtures' own, and the make rules compile the same fixture sources over the same extracted functions with the same warnings and, off Windows, the same ASan and UBSan. The script extracted `chown`'s inputs with `fixtures/chown_plan.sh`, which also required `EPERM` and `proc_privileged` to be defined once; `extract.py` takes every matching line, and a second, different definition fails the fixture's compile under `-Werror` as a redefined macro.
* `tests/host/kstack_check.sh` (`kstack`): every check is in the controller. Its inline extractor had the same rule as `extract.py`, one definition of each function and macro.
* `tests/host/endothers_check.sh` (`endothers`): every check is in the controller, as for `kstack`.
* `tests/system/fakeapi/fakeapi_test.py` (`fakeapi`): `selftest.c` already checked an unordered step played after the ordered one, a repeated step reused and not owed, an unexpected request, a reset before the expected WebSocket message, a reset after it, and a held reply. Added to it: an unordered step plays once; an unordered or ordered step that never plays fails the verdict; a repeated step plays before and after the ordered step and is not owed when it never plays; a repeated step that is not unordered is refused by `prepare_steps`; a reset that refuses the upgrade's reply fails the step once and does not play it; and `/fakeapi/release` answers `released`.
* `tests/system/fakeapi/anthropic_test.py` (`anthropic`): `selftest.c` already checked a matching result in text blocks, a failed and a wrong string result, and the stream's start, argument deltas and stop. Issue #975 takes over, in `messages()` of `selftest.c` (its commit c1fdfb718), a duplicated result, a result with no preceding call and a step's `body`. Added: a wrong result in text blocks, with its reason; the stream rebuilt from the wire, whose first and last events, stop reason, text and call arguments must match the step; and the whole response, whose content must equal the step's reply.
* `tests/system/fakeapi/gemini_test.py` (`gemini`): `selftest.c` already checked a signed call with its response, a wrong signature, the signature, finish and usage in a streamed reply, and the JSON type of a whole one. Issue #975 takes over, in `gemini()` of `selftest.c`, a wrong, a failed, a duplicated and an unpaired function response; it makes the unpaired one by renaming the function, where the Python test removed the call, and both reach the same refusal. Added: a request to another model's path, and one without the step's prompt, system instruction or declaration, each refused with its reason; both reply forms rebuilt from the wire, with the text, the signed call, the last response's `STOP` and four streamed events; and an unordered step chosen by its model, before or after the ordered one, and not played twice.

### Images (#1253)

The five checks of `host.host.scripts_images`, whose controllers `host_image_check` runs in the GNU helper machine.

* `tests/system/mkext2_overlay_test.sh`, against `host_image_overlay.c`: every check is in the controller. Both volumes pass `mkext2.sh`, list the same nodes, give the added nodes and `/tmp` their modes, keep the mode of the file a link names, and a failing `mke2fs` or `debugfs` leaves its diagnostic and no image.
* `tests/host/krffs_tools_test.sh`, against `host_image_krffs.c`: every check is in the controller: the nine refusals of the stager, the kept image of a failed replacement, the visualiser's ribbon, the defragmenter on a whole image and on a region, and the two bytes of the editor. The script compiled the tools itself; the controller runs those that preparation builds from `krffs/`.
* `tests/system/install_accounts_test.sh`, against `host_image_accounts.c`: the controller lacked the last row, in which the image's `nobody` and `nogroup` of id 65534, which the machine lacks, are appended with their shadow line. It is added, and judges the three files whole. The other eleven rows were there.
* `tests/system/tooling_image_test.sh`, against `host_image_tooling.c` and `host_products_drive.c`: every check is in the controller, all eight rows under `sh` and `bash`. The script's test for `git`, `tar` and `perl` on the host has no purpose left: `git` is a stand-in in the controller, and `perl` only gave the old driver a process group.
* `collection_tools` has no script. Its command, `etude_platform_hosttest --collection-only`, runs `tests/host/compat/collection_tools_host.c` on `tools/etude/collection_tools.c`, and `host_image_collection.c` does not run it: the controller runs the real `mformat`, `mcopy`, `mtype`, `mke2fs` and `debugfs` on a disk with the same file names, offset, partial output and statuses. The command's checks are not lost, since on a POSIX host `host.host.etude_quiet` runs the whole `etude_platform_hosttest`, its collection part among them, as its `etude_platform` assertion.

### Build (#1254)

* `tests/host/doom_libc_check.sh`, against `host_doom.c`: every check is in the controller, which runs each ABI's program on a guest kernel of that ABI and requires status 0 and the format line. The script's fallback to etude's tooling machine, and its status 2 for a program that ended above 1, served the script alone.
* `tests/system/run_scripts_test.sh`, against `build_programs.c` with the rows of `build_rows.h`: every check is in the controller, all 29 rows.
* `tests/system/commit_msg_test.sh`, against `build_programs.c`: every check is in the controller, all ten messages.
* `tests/system/prereq_test.sh`, against `build_prereq.c`: the controller had every row, but three checks were weaker than the script's, and now match them. A malformed recipe must be refused by a line that begins `prereq: FILE:LINE: `, not only by a line that names the file, which the refusals `cannot read` and `no platform` also give. A command that the tooling machine supplies must be a whole line of `tools/etude/tooling/tools`, since a search for `ip` found it in `gzip`. The tooling machine's Codex version must be the whole line `CODEX_VERSION=VERSION` of `provision.sh`.
* `tests/system/together_test.sh`, against `build_together.c`: the controller's stand-in maker now waits a second before it writes its files, as the script's did; without the wait, a make that ran a maker once for each of its files could find them already written by the first run, and the second run would not happen. The script's option to run under several makes is set aside: the GNU helper has one make, and the row that finds no grouped targets in the makefiles covers the fault of GNU Make 3.81 that the option tested.
* `tests/system/gnu_cmake_jobs_test.sh`, against `build_ports.c`: every check is in the controller. The `CMAKE_RECIPE` override served the script alone.
* `tests/system/gnu_port_resume_test.sh`, against `build_ports.c`: every check is in the controller.
* `tests/system/host_suite_test.sh`, against `build_suite.c`, which tests the native `host` driver in place of `host.sh`: five checks were missing or weaker, and are added. The sanitizer probes must fail by their exit status; the `not run:` line of `signals` must come under its PASS line; the listing must name `unlisted`, and `ghost` must fail because the prepared inventory has no check for it; the stopped run must say `0 of 1 checks finished`; and a selection with a name given twice must give no result line for it, in place of a search for `fine`, which the driver prints only when that check fails. Set aside: the last line of `etude run`, which the native driver does not print, since the controller counts the results, the assertions and the end record; and the kept scratch path ending in the test's file name, since the controller requires the one kept directory to be `keep`'s.
* `tests/system/case_select_test.py`, against `build_runner.c`: every check of the selection and of its refusals is in the controller, through `etude run -N` and `etude prepare collection`. Set aside: the start line's `a selection of N cases` and the call of make that followed it, which `tests/run` gave before it built; the native entry prints that line only after preparation, and the dry selection checks the same cases in the same order.
* `vendor/tcc/check_asm.sh`, against `build_compilers.c`: every check is in the controller. The script's build of both TinyCCs from the tarball is preparation's now, in `vendor/gnu/build_host_products.sh`, with the same six patches in the same order.
* `vendor/tcc/check_ldscript.sh`, against `build_compilers.c`: every check is in the controller, the layout rows read from the link without `-g` and the symbol rows from the link with it.

### Ci, board and release (#1255)

* `tests/system/ci_test.sh`, against `flow_ci.c`: every row is in the controller. The controller lacked two checks, which it now makes: in each row where a bisection reports the first commit that fails, that the same line reports its parent, `good`, as passing; and, for a core run whose i686 result was lost with `default` and `runner` failed, that the first sentence of the issue's body lists both steps and that the body gives the `gh run view 1` command for the log.
* `tests/system/ci_run_test.sh`, against `flow_ci_run.c` and `flow_ci_prepare.c`: every check is in the controller. `flow_ci_prepare.c` adds checks that the script never made: the clean build of a release, the transfer of shared evidence and the record of the report's reader.
* `tests/system/board_test.sh`, against `flow_board.c`: every row is in the controller. A refused `take` must now print the issue's number, as `board: #N: REASON`; the controller matched the reason alone.
* `tests/system/release_resume_test.sh`, against `flow_release.c`: every row is in the controller. It now also checks the order of the calls of board.sh when a record with `closing` is resumed (`phase`, `done`, then `archive`), that the chosen rerun is the fourth line of the record, and that each row that starts a run dispatches `full.yml` once.
* `tests/system/worker_park_test.sh`, against `flow_worker.c`: every check is in the controller.

### The loops' and the testing host's tools (#1256)

* `machine_measure`: `tests/system/machine_measure_test.sh`, against `flow_measure.c`. Every check is in the controller: the eight identity states, the refusals of both drivers before any timing, the five inbound cleanups, the three load windows and the one-second pause between session starts.
* `progress`: `tests/system/progress_test.py`, against `flow_progress.c`. The controller had every row and lacked two checks of the script's fixture, which are added. Its fake `gh` and `board.sh` now answer only the exact requests the report makes: the repository's paths with `per_page=100`, and `members` of the phase that `current` gave. A row that expects a failure now requires the report's own `progress: unknown:` diagnostic, so that a crash of the report does not pass as a refused read.
* `limited`: `tests/system/limited_test.sh`, against `flow_basic.c`. Every check is in the controller, which compares the whole message, newline included.
* `tidy`: `tests/system/tidy_test.sh`, against `flow_cleanup.c`. Every row is in the controller. It checked the store's `cross` link and the scratch link only for existence, and now also requires that each is still a symbolic link, as the script's `-L` did.
* `leftovers`: `tests/system/leftovers_test.sh`, against `flow_cleanup.c`. Every check is in the controller, and the loose link's `-L` is added in the same way. The controller also requires the sweep without the issues' text to fail, which the script only logged.
* `space`: `tests/system/space_test.sh`, against `flow_space.c`. Every check is in the controller but one, set aside: the marker file that the script's wrapper writes after `space.sh` succeeds repeats the check of its exit status.
* `agents_guest`: `tests/system/agents_guest_test.sh`, against `flow_agents.c`, to which `flow_basic.c` dispatches. Every check is in the controller: the six rows' statuses, outputs and empty TMPDIR, and the wait until no process of the group is left.
* `testing_run`: `tests/system/testing_run_test.sh`, against `flow_testing.c`. Every check is in the controller: the eight quoted commands, the synced worktree and the four reports of `wait`.

### Removed (#1251)

* The COMMAND column of `host.list`. A row is `NAME|LABEL`; `tools/etude/host_prepare_scripts.c` refuses a third field, and the detail of an unsupported entry names the row in place of its command. The assertions of the five scripts cases in `tests/cases/host.host.json` say which program judges them in place of the command.
* The 37 scripts that only COMMAND named, each compared above: the 26 shell tests and four Python tests of `tests/system/` (the three of the fake API were gone already), `extract_check.sh`, `kstack_check.sh`, `endothers_check.sh`, `krffs_tools_test.sh` and `doom_libc_check.sh` of `tests/host/`, and `check_asm.sh` and `check_ldscript.sh` of `vendor/tcc/`. With them went the three files that only they used: `tests/system/test_status_test.py`, which `test_status_check.sh` ran, `tests/host/fixtures/chown_plan.sh`, which `extract_check.sh` ran, and `tests/system/fixtures/old_ports.py`, which `etude_launch_test.py` ran; `etude_ports_hosttest.c` has its own stand-in, `tests/host/compat/old_ports_host.c`. No check was set aside here beyond those the comparisons above set aside.
* `tests/cases/host/host.sh`, whole. No case ran it, and its print mode served `tools/verify/coverage.sh` alone, which now takes the compile commands from `etude prepare -H --print-commands`, as `tools/dev/gen_compile_commands.sh` does: the native route first, and the GNU route, with the same compiler, for a test the native route leaves to the GNU helper, so that every host test is listed once. `tests/fuzz/build.sh` no longer reads `HOST_PRINT_COMMANDS`, which only `host.sh` set.
* Kept: `tests/system/tooling.sh`, which `tests/cases/host/style.sh` and `tests/system/tooling_ready.sh` source; `tests/system/case_select.py`, which `tests/run` runs; `tests/host/fixtures/*.c`, which `mk/host-fixtures.mk` and `tests/cases/host/products.mk` build; and `vendor/tcc/asm-test/` and `ld-test/`, which `build_compilers.c` reads. The adapter `etude run host SELECTION`, `tools/etude/posix/run_host.c`, ran only the selections of `host.sh` and has no caller left.

## The fake API's Python

`tests/system/fakeapi/*.py` and `tests/system/agents/*_verify.py` are run by no case of the collection; issue #975 replaces the fake API's services with C.

## The routine entry

`tests/run`, `tools/etude/collection-windows.sh` and the other shell entries of `tools/etude` are the routine and deployment route, which issues #1177 and #1037 own. They are not host drivers of a case.
