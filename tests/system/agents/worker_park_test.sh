# shellcheck shell=bash
# worker_park_test: tests/system/worker_park_test.sh, the test of how the
# worker loop parks a committed issue while a hold is open and passes by an
# intermittent issue handed back with nothing new from ci, against
# stand-ins for gh, board.sh and the agent, with --mutate, which runs its
# rows with each of its faults in a private copy of tools/flow and then with
# the copy restored.
# The bound is about twice the 1666 s it took under TCG on the testing host.
# bound: 3600

row() {
    tree || return
    (cd tree && sh tests/system/worker_park_test.sh --mutate) || fail "worker_park_test.sh --mutate: status $?"
}
