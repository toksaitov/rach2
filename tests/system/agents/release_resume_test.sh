# shellcheck shell=bash
# release_resume_test: tests/system/release_resume_test.sh, the test of how
# the loops' release.sh resumes a release from its record, against stand-ins
# for gh, ssh and the machine, with --mutate, which runs its rows with each
# of its faults in a private copy of release.sh and then with the copy
# restored.
# bound: 2500

row() {
    tree || return
    (cd tree && sh tests/system/release_resume_test.sh --mutate) || fail "release_resume_test.sh --mutate: status $?"
}
