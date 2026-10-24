# sources.mk: the sources of etude and its helpers, which the top-level Makefile
# and version.mk, the build of a cached version on the machine it is for,
# both include, so that one file lists them. Paths are relative to the root
# of the tree.

# The sources of etude, the one command, for each platform. ETUDE_SRC is
# that of Linux and macOS, which has every verb; ETUDE_WIN_SRC that of
# Windows, ETUDE_RACH2_SRC for Rach2, with check and local native runs, and
# ETUDE_LOCK_SRC that of the build of `lock` alone, with ETUDE_LOCK_ONLY
# defined, which the first process of tests/run makes for itself. etude.c
# chooses the verbs by the same platform.

# `etude check`, the validator of manifests, reports, results, runs and
# histories, and the parts every platform shares.
ETUDE_CHECK_SRC = tools/etude/etude.c tools/etude/check.c tools/etude/cases.c \
                  tools/etude/digest.c tools/etude/judge.c tools/etude/json.c \
                  tools/etude/processor.c tools/etude/qemu_profile.c tools/etude/schema.c \
                  tools/etude/verdict.c tools/etude/verdict_budget.c tools/etude/history.c tools/etude/history_json.c \
                  tools/etude/history_collect.c utils/sha256.c
# The local Rach2 collection shares its framed transport with the desktop
# single-attempt command; its public entry remains etude.
NATIVE_COLLECTION_DRIVER_SRC = tests/native/collection.c tests/native/inventory.c \
    tests/native/attempt.c tests/native/assets.c tests/native/proof.c tests/native/cut_transport.c \
    tests/native/history_plan.c tests/native/history_result.c tests/native/history_io.c tests/native/history_totals.c tests/native/bounds.c
NATIVE_COLLECTION_TRANSPORT_SRC = tools/etude/posix/run_native.c tools/etude/posix/remote.c \
    tools/etude/posix/process.c tools/etude/guest/wire.c
ETUDE_RACH2_SRC = $(ETUDE_CHECK_SRC) $(NATIVE_COLLECTION_DRIVER_SRC) \
                   $(NATIVE_COLLECTION_TRANSPORT_SRC) tools/etude/watchdog.c \
                   tools/etude/compare.c tools/etude/compare_load.c tools/etude/observe_import.c

# `etude lock`: the holder of a tree's run lock, on Linux and macOS, and the
# holder of the host lock and a block of host ports, whose winfmt.c gives it
# a Windows command line.
ETUDE_LOCK_SRC = tools/etude/etude.c tools/etude/lock_tree.c tools/etude/posix/hold.c \
                 tools/etude/lock_ports.c tools/etude/portlock.c tools/etude/win/winfmt.c
ETUDE_LOCK_WIN_SRC = tools/etude/etude.c tools/etude/lock_tree.c tools/etude/win/hold.c \
                     tools/etude/lock_ports.c tools/etude/portlock.c tools/etude/win/winfmt.c

# `etude run suites`, `etude prepare` and `etude check record`: the
# preparation adapter of the suites of tests/run and their set driver, over
# the supervisor, the ledger, the old runner's locks and the history, beside
# `etude show lines`, the compatible formatter, and `etude admit`, the
# external admission wrapper for old checkouts and new runs. These are the
# parts with Windows sources as well.
ETUDE_OBSERVE_SRC = tools/etude/observe_sequence.c tools/etude/posix/observe_sequence.c \
                    tools/etude/observe_history.c tools/etude/observe.c tools/etude/observe_import.c \
                    tools/etude/observe_main.c tools/etude/observe_scope.c \
                    tools/etude/observe_system.c

ETUDE_RUN_SRC = tools/etude/repeat.c tools/etude/collection_repeat.c tools/etude/collection_load.c \
                tools/etude/posix/load.c $(ETUDE_OBSERVE_SRC) tools/etude/measurement_context.c \
                tools/etude/boot_identity.c tools/etude/collection.c \
                tools/etude/posix/run_suites.c tools/etude/posix/legacy.c \
                tools/etude/preparation_record.c \
                tools/etude/posix/legacy_set.c tools/etude/posix/legacy_cases.c \
                tools/etude/posix/legacy_guest.c tools/etude/serial.c \
                tools/etude/guest/wire.c tools/etude/posix/store.c \
                tools/etude/watchdog.c tools/etude/supervise.c tools/etude/ledger.c \
                tools/etude/sched.c tools/etude/portlock.c tools/etude/posix/status.c \
                tools/etude/compare.c tools/etude/compare_load.c tools/etude/baseline.c tools/etude/compare_report.c \
                tools/etude/compare_run.c tools/etude/lines.c tools/etude/posix/admit.c \
                tools/etude/posix/run_host.c tools/etude/lock_tree.c tools/etude/lock_ports.c \
                tools/etude/provision_tooling.c tools/etude/tooling.c tools/etude/tooling_cache.c \
                tools/etude/budget_run.c tools/etude/budget.c tools/etude/budget_prepare.c tools/etude/cost.c

# The verbs of Linux and macOS alone: `etude run native`, `etude show
# failures`, `etude status` and `etude release`, `etude
# serve` and `etude build`, with the host executor, which
# ETUDE_WIN_EXEC_SRC replaces on Windows.
ETUDE_POSIX_SRC = tools/etude/posix/run_native.c tools/etude/posix/remote.c \
                  tools/etude/failures.c tools/etude/posix/claims.c tools/etude/posix/responder.c \
                  tools/etude/posix/serve.c tools/etude/step/step.c \
                  tools/etude/posix/exec.c tools/etude/posix/keeper.c \
                  tools/etude/posix/process.c tools/etude/posix/qemu.c \
                  tools/etude/posix/bridge.c tools/etude/posix/hold.c \
                  tools/etude/capture.c tools/etude/win/winfmt.c

ETUDE_SRC = $(ETUDE_CHECK_SRC) $(ETUDE_RUN_SRC) $(ETUDE_POSIX_SRC)

# etude-volume, the helper that makes and reads the tooling machine's
# volumes for `etude provision tooling`, on every platform that has it.
ETUDE_VOLUME_SRC = tools/etude/volume/etude_volume.c tools/etude/volume/volume.c

# libetude-case, the library of machine scenarios on POSIX systems, with the
# `main` it gives them, which each scenario of tests/cases/machine/ links.
ETUDE_CASE_LIB_SRC = tools/etude/posix/scenario.c tools/etude/posix/case_main.c \
                     tools/etude/serial.c tools/etude/processor.c \
                     tools/etude/posix/qemu.c tools/etude/qemu_profile.c \
                     tools/etude/posix/exec.c tools/etude/posix/keeper.c \
                     tools/etude/posix/process.c tools/etude/posix/store.c \
                     tools/etude/capture.c tools/etude/history_json.c \
                     tools/etude/guest/wire.c tools/etude/volume/volume.c utils/sha256.c

# The native Windows executor, which etude and the scenarios link there.
ETUDE_WIN_EXEC_SRC = tools/etude/win/exec.c tools/etude/win/winexec.c \
                     tools/etude/win/winsys.c tools/etude/win/winfmt.c \
                     tools/etude/win/files.c tools/etude/capture.c
ETUDE_WIN_SRC = $(ETUDE_CHECK_SRC) $(ETUDE_RUN_SRC) $(ETUDE_WIN_EXEC_SRC) \
                tools/etude/posix/qemu.c tools/etude/win/bridge.c tools/etude/win/hold.c
ETUDE_CASE_LIB_WIN_SRC = tools/etude/posix/scenario.c tools/etude/posix/case_main.c \
                         tools/etude/serial.c tools/etude/processor.c \
                         tools/etude/posix/qemu.c tools/etude/qemu_profile.c \
                         $(ETUDE_WIN_EXEC_SRC) tools/etude/posix/store.c \
                         tools/etude/history_json.c tools/etude/guest/wire.c \
                         tools/etude/volume/volume.c utils/sha256.c

# The native driver of prepared host checks, linked without the command UI.
ETUDE_HOSTCASE_SRC = tests/cases/host/host.c tools/etude/json.c utils/sha256.c \
                     tools/etude/capture.c tools/etude/posix/exec.c \
                     tools/etude/posix/keeper.c tools/etude/posix/process.c
ETUDE_HOSTCASE_WIN_SRC = tests/cases/host/host.c tools/etude/json.c utils/sha256.c \
                         $(ETUDE_WIN_EXEC_SRC)

# Native Windows behavior fixtures and the programs they launch.
ETUDE_WIN_FIXTURE_SRC = tools/etude/win/winfixture.c tools/etude/win/winexecfix.c \
                        tools/etude/win/winledgerfix.c tools/etude/win/winrunfix.c \
                        tools/etude/win/wintoolfix.c tools/etude/win/winholdfix.c \
                        tools/etude/win/winportsfix.c tools/etude/win/wincollectionfix.c \
                        tools/etude/ledger.c tools/etude/watchdog.c tools/etude/supervise.c \
                        tools/etude/history_json.c tools/etude/posix/qemu.c \
                        tools/etude/posix/store.c tools/etude/qemu_profile.c \
                        tools/etude/processor.c $(ETUDE_WIN_EXEC_SRC) utils/sha256.c \
                        tests/host/compat/output_sinks.c
