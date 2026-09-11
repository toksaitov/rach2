# deadline.sh: a suite's completion bounds under the set's measured rate.
#
# A set's runner measures the emulator's rate under the load the set makes
# and tells every suite `RACH2_DEADLINES=scaled` with
# `RACH2_DEADLINE_FACTOR=F`, F a decimal with one digit. A suite sources
# this file and writes each bound that limits how long a step may take as
# `$(scaled N)`, N the seconds it takes on an idle host; the result is N
# times F rounded up, or N when the suite was told nothing, or told the
# mode without a factor. A number that asserts a rate, an interval a row
# holds a load for, or a deadline a row means to run out is written as it
# is. The factor is kept in tenths, since the shell's arithmetic is whole.
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
