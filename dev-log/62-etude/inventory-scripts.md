# The scripts left in the collection

This is the inventory that issue #977 starts from: every shell, Python or Perl script under `tests/` that the native collection still runs, and where each runs, at `master` 04153d8e8. Every case of every manifest in `tests/cases/` names a C program as its `execution.program`; the scripts below are run by those programs, by preparation, or by nothing in the collection.

Four groups are allowed by [the recovery's acceptance](recovery.md#acceptance): a product run in its own language when its behavior is what a case checks, a command run inside a Rach2 machine that is the system under test, a step of preparation, which may keep its declared build tools, and a host test that preparation places in the GNU helper machine. Three are the work left: the legacy commands of `host.list`, the fake API's Python, and the routine entry.

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
* `tests/cases/host/agentapi.sh`: replaced by `machine/agentapi_scenario.c`. The prerequisite check that read its `CODEX_VERSION` reads `VERSION` of `vendor/gnu/agents/packages/codex.sh`, the Codex that the cases run.

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
* Build helpers: `tests/musl/build.sh`, `tests/fuzz/build.sh`, `tests/system/fakeapi/build_openssl.sh` and `fetch_muse.sh`, `tests/host/mkexfat_external.sh` and `tests/system/tooling_ready.sh`.
* Source checks: `tests/cases/host/style.sh`, which `tools/etude/host_prepare_style.c` runs to record the evidence that `host/style.c` reads, and the tests it runs, `tests/system/*_test.sh` of the style list in [`tests/system/Readme.md`](../../tests/system/Readme.md).
* Python in preparation, which issue #1176 removes: `tests/host/fixtures/extract.py`, `tests/native/prepare.py`, `tests/cases/machine/prepare_codex_digest.py` and `tools/setup/prepare_clang.py`.

## Host tests in the GNU helper machine

`tools/etude/host_prepare.c` gives a host test of `tests/host` to the GNU helper machine when its sources call an interpreter, and names seven more that need its POSIX environment: `ext2`, `fat32`, `etude_volume`, `etude_compare`, `etude_step`, `etude_native` and `doom_platform`. On Windows every host test but the native platform checks goes there. These run the tree's own C under test with a shell or a product script as a fixture; they are host tests placed in a machine, not old drivers moved into one.

## The legacy host.list commands

`tests/cases/host/host.list` gives each of its 45 checks a COMMAND, the script that `tests/cases/host/host.sh` runs. The collection does not run these: `host_scripts_scenario.c` and `host.c` run each row's prepared C controller (`host_build_check`, `host_image_check`, `host_doom_check` and `host-flow-check` in the GNU helper, the kernel fixtures and `fakeapi_test` natively). `host.sh` is still run by `tools/verify/coverage.sh`, with `HOST_PRINT_COMMANDS`, and is the subject of `tests/system/host_suite_test.sh`, itself one of the commands.

The commands are 26 shell tests under `tests/system/`, seven Python tests (`progress_test.py`, `case_select_test.py`, `etude_launch_test.py`, `agents_credentials_test.py` and the three of `tests/system/fakeapi/`), seven kernel fixture scripts under `tests/host/` (`extract_check.sh` five times, `kstack_check.sh`, `endothers_check.sh`), `tests/host/krffs_tools_test.sh`, `tests/host/doom_libc_check.sh`, the two TinyCC checks of `vendor/tcc/`, and `etude_platform_hosttest --collection-only`, a prepared program. Their removal waits for a comparison of each with its controller, so that an assertion the controller lacks is added or recorded as redundant before the script goes. The comparisons are issues #1252 (the kernel fixtures and the fake API), #1253 (images), #1254 (build), #1255 (ci, board and release), #1256 (the loops' and the testing host's tools) and #1257 (etude's deployment, versions and tooling machine); each records its result below. Issue #1251 then removes the column and the scripts.

## The fake API's Python

`tests/system/fakeapi/*.py` and `tests/system/agents/*_verify.py` are run by no case of the collection, apart from the three `host.list` commands above; issue #975 replaces the fake API's services with C.

## The routine entry

`tests/run`, `tools/etude/collection-windows.sh` and the other shell entries of `tools/etude` are the routine and deployment route, which issues #1177 and #1037 own. They are not host drivers of a case.
