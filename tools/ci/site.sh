#!/bin/sh
#
# tools/ci/site.sh: read the site of routine checks, tools/ci/site, and
# print it as a workflow's outputs.
#
#   tools/ci/site.sh [FILE]
#
# FILE, or standard input without one, has one line `runner LABEL` and one
# line `accelerator NAME`; empty lines and lines that begin with `#` are
# comments. LABEL is the label of the self-hosted GitHub Actions runners of
# the machine that runs the checks, lowercase letters, digits and
# hyphens; the core and release workflows select a runner that has it. NAME
# is the accelerator, lowercase letters, which the workflow puts in
# RACH2_QEMU_ACCEL. The native runner checks it. The script prints
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
