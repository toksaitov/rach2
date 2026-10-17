# shellcheck shell=bash
# tidy_test: tests/system/tidy_test.sh, the test of what the loops remove
# after finished work, with --mutate, which runs its rows with each of its
# faults in private copies of lib.sh and scratch.pl and then with the copies
# restored. Its rows of busy scratch directories run only where
# /proc/self/fd is a directory, which the row requires first, and it reads
# the scratch names of the tree's tests and tools with git grep.
# bound: 2000

row() {
    same "/proc/self/fd" "$(test -d /proc/self/fd && echo directory)" "directory"
    tree || return
    (cd tree && sh tests/system/tidy_test.sh --mutate) || fail "tidy_test.sh --mutate: status $?"
}
