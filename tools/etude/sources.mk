# sources.mk: the sources of etude's programs, which the top-level Makefile
# and version.mk, the build of a cached version on the machine it is for,
# both include, so that one file lists them. Paths are relative to the root
# of the tree.

# casecheck, the validator of manifests, reports, results and runs.
ETUDE_CASECHECK_SRC = tools/etude/casecheck.c tools/etude/cases.c \
                      tools/etude/digest.c tools/etude/judge.c tools/etude/json.c \
                      tools/etude/processor.c tools/etude/qemu_profile.c tools/etude/schema.c \
                      tools/etude/verdict.c tools/etude/history.c \
                      tools/etude/history_json.c utils/sha256.c

# history_check, the checker of a closed history stream.
ETUDE_HISTORY_CHECK_SRC = tools/etude/history_check.c tools/etude/history.c \
                          tools/etude/history_json.c \
                          tools/etude/history_collect.c utils/sha256.c

# history_compare, the comparison of a run's cases with their timing history.
ETUDE_HISTORY_COMPARE_SRC = tools/etude/history_compare.c tools/etude/compare.c \
                            tools/etude/baseline.c tools/etude/compare_report.c \
                            tools/etude/compare_run.c tools/etude/history.c \
                            tools/etude/history_json.c \
                            tools/etude/history_collect.c utils/sha256.c

# history_failures, the attempts of a run that did not pass, which
# tools/ci/report.sh gives in ci's issues.
ETUDE_HISTORY_FAILURES_SRC = tools/etude/history_failures.c tools/etude/failures.c \
                             tools/etude/history.c tools/etude/history_json.c \
                             tools/etude/history_collect.c utils/sha256.c

# history_lines, the compatible formatter, which prints a run's history as the
# lines of tests/run.
ETUDE_HISTORY_LINES_SRC = tools/etude/history_lines.c tools/etude/lines.c \
                          tools/etude/history.c tools/etude/history_json.c \
                          tools/etude/history_collect.c utils/sha256.c

# history_budget, the forecast and the check of a run's budget. The cost of a
# case takes its processor reservation from the schedule, which links the
# ledger.
ETUDE_HISTORY_BUDGET_SRC = tools/etude/history_budget.c tools/etude/budget.c \
                           tools/etude/budget_run.c tools/etude/digest.c \
                           tools/etude/cost.c tools/etude/sched.c tools/etude/ledger.c \
                           tools/etude/posix/process.c tools/etude/compare.c \
                           tools/etude/compare_report.c tools/etude/history.c \
                           tools/etude/history_json.c tools/etude/history_collect.c \
                           tools/etude/json.c tools/etude/schema.c utils/sha256.c

# etude-remote, the responder of remote execution on Linux and macOS.
ETUDE_REMOTE_SRC = tools/etude/posix/etude_remote.c tools/etude/posix/serve.c \
                   tools/etude/posix/exec.c tools/etude/posix/keeper.c \
                   tools/etude/posix/process.c tools/etude/capture.c \
                   tools/etude/guest/wire.c tools/etude/json.c utils/sha256.c

# etude-native, which runs one case on a Rach2 machine that is already up,
# through etude-guest over a transport such as ssh(1).
ETUDE_NATIVE_SRC = tools/etude/posix/etude_native.c tools/etude/posix/remote.c \
                   tools/etude/posix/process.c tools/etude/guest/wire.c \
                   tools/etude/json.c utils/sha256.c

# etude-host, which runs the host checks of `tests/run host` at once on Linux
# and macOS.
ETUDE_HOST_SRC = tools/etude/posix/etude_host.c tools/etude/posix/exec.c \
                 tools/etude/posix/keeper.c tools/etude/posix/process.c \
                 tools/etude/capture.c

# etude-tooling, the local Linux tooling machine, on Linux and macOS.
ETUDE_TOOLING_SRC = tools/etude/etude_tooling.c tools/etude/tooling.c \
                    tools/etude/posix/exec.c tools/etude/posix/keeper.c \
                    tools/etude/posix/process.c tools/etude/capture.c \
                    tools/etude/history.c tools/etude/history_json.c \
                    tools/etude/history_collect.c utils/sha256.c

# etude-volume, the helper that makes and reads the tooling machine's
# volumes for etude-tooling, on every platform etude-tooling runs on.
ETUDE_VOLUME_SRC = tools/etude/volume/etude_volume.c tools/etude/volume/volume.c

# etude-legacy, the preparation adapter of the suites of tests/run and their
# set driver, on Linux and macOS.
ETUDE_LEGACY_SRC = tools/etude/posix/etude_legacy.c tools/etude/posix/legacy.c \
                   tools/etude/posix/legacy_set.c tools/etude/posix/legacy_cases.c \
                   tools/etude/posix/legacy_guest.c tools/etude/serial.c \
                   tools/etude/guest/wire.c tools/etude/posix/store.c \
                   tools/etude/digest.c tools/etude/json.c tools/etude/posix/exec.c \
                   tools/etude/posix/keeper.c tools/etude/posix/process.c \
                   tools/etude/capture.c tools/etude/watchdog.c \
                   tools/etude/supervise.c tools/etude/qemu_profile.c \
                   tools/etude/posix/qemu.c tools/etude/processor.c \
                   tools/etude/ledger.c tools/etude/sched.c \
                   tools/etude/posix/bridge.c tools/etude/posix/status.c \
                   tools/etude/compare.c tools/etude/baseline.c \
                   tools/etude/compare_report.c tools/etude/compare_run.c \
                   tools/etude/history.c tools/etude/history_json.c \
                   tools/etude/history_collect.c utils/sha256.c

# etude-load, the load fixture of shuffled and loaded repetitions on Linux
# and macOS.
ETUDE_LOAD_SRC = tools/etude/posix/etude_load.c tools/etude/posix/load.c

# etude-bridge, the external admission wrapper for old checkouts and new runs.
ETUDE_BRIDGE_SRC = tools/etude/posix/etude_bridge.c tools/etude/posix/bridge.c \
                   tools/etude/ledger.c tools/etude/posix/exec.c \
                   tools/etude/posix/keeper.c tools/etude/posix/process.c \
                   tools/etude/capture.c utils/sha256.c

# etude-hold, which holds a tree's run lock while one command runs, on Linux
# and macOS, and on Windows.
ETUDE_HOLD_SRC = tools/etude/etude_hold.c tools/etude/posix/hold.c
ETUDE_HOLD_WIN_SRC = tools/etude/etude_hold.c tools/etude/win/hold.c tools/etude/win/winfmt.c

# etude-ledger, which lists a ledger's claims and drops one whose processes a
# person has found ended.
ETUDE_LEDGER_SRC = tools/etude/posix/etude_ledger.c tools/etude/ledger.c \
                   tools/etude/posix/process.c utils/sha256.c

# libetude-case, the library of machine scenarios on POSIX systems, with the
# `main` it gives them, which each scenario of tests/cases/machine/ links.
ETUDE_CASE_LIB_SRC = tools/etude/posix/scenario.c tools/etude/posix/case_main.c \
                     tools/etude/serial.c tools/etude/processor.c \
                     tools/etude/posix/qemu.c tools/etude/qemu_profile.c \
                     tools/etude/posix/exec.c tools/etude/posix/keeper.c \
                     tools/etude/posix/process.c tools/etude/posix/store.c \
                     tools/etude/capture.c tools/etude/history_json.c \
                     tools/etude/guest/wire.c utils/sha256.c
