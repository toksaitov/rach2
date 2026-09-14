# deadline.sh: a suite's completion bounds under the factor the run was given.
#
# Nothing is measured. The runner measured the emulator's rate and refused a
# set past an envelope until 2026-09-15; a host busy with something else was
# then refused a run rather than given a slower one, so the measurement went.
# What is left is a factor a person asks for: `RACH2_TIMEOUT_FACTOR=F`, from
# 1 through 64, which the runner passes on as `RACH2_DEADLINES=scaled` with
# `RACH2_DEADLINE_FACTOR=F`, and which its start line records. A suite sources
# this file and writes each bound that limits how long a step may take as
# `$(scaled N)`, N the seconds it takes on an idle host; the result is N
# times F rounded up, or N when the run asked for no factor. The runner's own
# bound on a silent suite is never scaled by any of this. A number that
# asserts a rate, an interval a row holds a load for, or a deadline a row
# means to run out is written as it is. The factor is kept in tenths, since
# the shell's arithmetic is whole.
deadline_tenths=10
if [ "${RACH2_DEADLINES:-}" = scaled ] && [ -n "${RACH2_DEADLINE_FACTOR:-}" ]; then
    case "$RACH2_DEADLINE_FACTOR" in
        *.*) deadline_tenths=$(( ${RACH2_DEADLINE_FACTOR%%.*} * 10 + $(printf '%s' "${RACH2_DEADLINE_FACTOR#*.}" | cut -c1) )) ;;
        *) deadline_tenths=$(( RACH2_DEADLINE_FACTOR * 10 )) ;;
    esac
    [ "$deadline_tenths" -ge 10 ] || deadline_tenths=10
    [ "$deadline_tenths" -le 640 ] || deadline_tenths=640
fi

# scaled N: N seconds under the factor, rounded up.
scaled() {
    echo $(( ($1 * deadline_tenths + 9) / 10 ))
}
