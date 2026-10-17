# shellcheck shell=bash
# limited_test: tests/system/limited_test.sh, the test of the loops' usage
# limit check in lib.sh, with --mutate, which runs its rows with each of its
# faults in a private copy of lib.sh and then with the copy restored.
# bound: 120

row() {
    tree || return
    (cd tree && sh tests/system/limited_test.sh --mutate) || fail "limited_test.sh --mutate: status $?"
}
