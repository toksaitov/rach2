#!/bin/sh
#
# tools/ci/site.sh: read the site of the release tests, tools/ci/site, and
# print it as a workflow's outputs.
#
#   tools/ci/site.sh [FILE]
#
# FILE, or standard input without one, has one line `runner LABEL` and one
# line `accelerator NAME`; empty lines and lines that begin with `#` are
# comments. LABEL is the label of the self-hosted GitHub Actions runners of
# the machine that runs the release tests, lowercase letters, digits and
# hyphens; full.yml runs the tests on a runner that has it. NAME is the
# accelerator, lowercase letters, which full.yml puts in RACH2_QEMU_ACCEL so
# that the run asks QEMU for it alone; tests/run takes kvm, hvf or tcg, and
# refuses a run under any other. The script prints
#
#   runner=LABEL
#   labels=["self-hosted","LABEL"]
#   accelerator=NAME
#
# and exits 0, or says on stderr why it refuses the file and exits 1: a line of
# another form, or a field missing or given twice.
#
set -u

awk '
    /^#/ || /^$/ { next }
    $1 == "runner" && NF == 2 && $2 ~ /^[a-z0-9][a-z0-9-]*$/ { runner[++r] = $2; next }
    $1 == "accelerator" && NF == 2 && $2 ~ /^[a-z]+$/ { accel[++a] = $2; next }
    {
        printf("ci: site: refused: line %d is not `runner LABEL` or `accelerator NAME`: %s\n", NR, $0) > "/dev/stderr"
        bad = 1
    }
    END {
        if (bad)
            exit 1
        if (r != 1 || a != 1) {
            printf("ci: site: refused: %d runner lines and %d accelerator lines, not one of each\n", r, a) > "/dev/stderr"
            exit 1
        }
        printf "runner=%s\nlabels=[\"self-hosted\",\"%s\"]\naccelerator=%s\n", runner[1], runner[1], accel[1]
    }' "$@"
