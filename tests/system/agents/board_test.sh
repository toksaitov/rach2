# shellcheck shell=bash
# board_test: tests/system/board_test.sh, the test of the loops' board.sh
# against a stand-in for gh, with --mutate, which runs its rows with each of
# its faults in a private copy of board.sh and then with the copy restored.
# The bound is about twice the 2984 s it took under TCG on the testing host.
# bound: 6000

row() {
    tree || return
    (cd tree && sh tests/system/board_test.sh --mutate) || fail "board_test.sh --mutate: status $?"
}
