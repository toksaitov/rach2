# shellcheck shell=bash
# procfd: /proc/self/fd is a directory on the machine, as the loops'
# tools/flow/scratch.pl needs where it reads /proc/PID/fd to find the scratch
# a process uses, and as tests/system/tidy_test.sh needs to run its rows of
# busy scratch.
# bound: 60

row() {
    same "/proc/self/fd" "$(test -d /proc/self/fd && echo directory)" "directory"
}
