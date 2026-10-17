# shellcheck shell=bash
# worker_park_test: tests/system/worker_park_test.sh, the test of how the
# worker loop parks a committed issue while a hold is open, against
# stand-ins for gh, board.sh and the agent, with --mutate, which runs its
# rows with each of its faults in a private copy of tools/flow and then with
# the copy restored.
# bound: 1800

row() {
    tree || return
    (cd tree && sh tests/system/worker_park_test.sh --mutate) || fail "worker_park_test.sh --mutate: status $?"
}
