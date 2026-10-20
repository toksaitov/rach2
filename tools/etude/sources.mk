# sources.mk: the sources of etude's programs, which the top-level Makefile
# and version.mk, the build of a cached version on the machine it is for,
# both include, so that one file lists them. Paths are relative to the root
# of the tree.

# casecheck, the validator of manifests, reports and results.
ETUDE_CASECHECK_SRC = tools/etude/casecheck.c tools/etude/cases.c \
                      tools/etude/judge.c tools/etude/json.c \
                      tools/etude/schema.c utils/sha256.c

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

# etude-remote, the responder of remote execution on Linux and macOS.
ETUDE_REMOTE_SRC = tools/etude/posix/etude_remote.c tools/etude/posix/serve.c \
                   tools/etude/posix/exec.c tools/etude/posix/keeper.c \
                   tools/etude/posix/process.c tools/etude/capture.c \
                   tools/etude/guest/wire.c tools/etude/json.c utils/sha256.c
