#!/bin/sh
# board_test.sh: check group creation, rollback, take's choice of the first
# assignee, and take and release of a closed issue against a local gh fixture.
# --mutate omits rollback, then reverses take's order, then lets a closed
# issue pass for an open one, in a private copy; --texts checks the faults'
# texts.

set -eu
case ${1:-} in
    '') mode=check ;;
    --mutate) mode=mutate ;;
    --texts) mode=texts ;;
    *) echo 'board-test: usage: board_test.sh [--mutate | --texts]' >&2; exit 2 ;;
esac
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/board-test.XXXXXX")
trap 'rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$work/bin" "$work/state"
cp "$repo/tools/flow/board.sh" "$work/board.sh"

# fault: omit rollback from the private copy, refusing missing fault text.
fault()
{
    sed 's/; group_rollback;/; :;/' "$repo/tools/flow/board.sh" > "$work/board.sh"
    ! cmp -s "$repo/tools/flow/board.sh" "$work/board.sh"
}

# take_fault: let the latest assignment keep an issue in the private copy,
# refusing missing fault text.
take_fault()
{
    sed 's/last\[l\] < last\[who\]/last[l] > last[who]/' \
        "$repo/tools/flow/board.sh" > "$work/board.sh"
    ! cmp -s "$repo/tools/flow/board.sh" "$work/board.sh"
}

# closed_fault: let take and release treat a closed issue as open in the
# private copy, refusing missing fault text.
closed_fault()
{
    # shellcheck disable=SC2016 # the dollar is in the text the fault removes
    sed 's/\[ "\$state" = CLOSED \] || return 1/return 1/' \
        "$repo/tools/flow/board.sh" > "$work/board.sh"
    ! cmp -s "$repo/tools/flow/board.sh" "$work/board.sh"
}
if [ "$mode" = texts ]; then
    fault
    take_fault
    closed_fault
    exit 0
fi

cat > "$work/bin/gh" <<'EOF'
#!/bin/sh
set -eu
printf '%s\n' "$*" >> "$BOARD_TEST_WORK/calls"
case "$1 $2" in
    'api user')
        echo "$BOARD_TEST_ME" ;;
    'api --paginate')
        printf '%s\n' "$BOARD_TEST_TIMELINE" | tr , '\n' ;;
    'api graphql')
        case "$*" in
            *'field(name: "Status")'*)
                printf '%s\n' project=project field=field Backlog=backlog \
                    Ready=ready In_progress=progress Done=done ;;
            *'projectItems(first: 10)'*)
                case $BOARD_TEST_FAIL in
                    item) exit 1 ;;
                    project|remove|close) exit 0 ;;
                esac
                echo item ;;
            *) exit 2 ;;
        esac ;;
    'issue view')
        case "$*" in
            *'--json state --jq .state'*)
                echo "$BOARD_TEST_STATE"
                exit 0 ;;
        esac
        if [ -n "$BOARD_TEST_ME" ]; then
            case "$*" in
                *'.assignees[].login'*) printf '%s\n' "$BOARD_TEST_NOW" | tr , '\n' ;;
                *) echo no ;;
            esac
            exit 0
        fi
        [ "$BOARD_TEST_FAIL" != validate ] || exit 1
        if [ -f "$BOARD_TEST_WORK/grouped-2" ]; then
            case $BOARD_TEST_FAIL in
                recheck) echo assigned ;;
                recheck-read) exit 1 ;;
            esac
        fi ;;
    'issue create')
        [ "$BOARD_TEST_FAIL" != create ] || exit 1
        printf '%s\n' "$*" > "$BOARD_TEST_WORK/body"
        echo open > "$BOARD_TEST_WORK/group"
        echo https://example.invalid/issues/99 ;;
    'issue edit')
        case "$*" in
            *'--add-label grouped')
                [ "$BOARD_TEST_FAIL" != "label$3" ] || exit 1
                if [ "$3" = 2 ]; then
                    case $BOARD_TEST_FAIL in
                        term) kill -TERM "$PPID"; exit 1 ;;
                    esac
                fi
                touch "$BOARD_TEST_WORK/grouped-$3"
                [ "$BOARD_TEST_FAIL" != "applied$3" ] ;;
            *'--remove-label grouped')
                [ "$BOARD_TEST_FAIL/$3" != remove/1 ] || exit 1
                rm -f "$BOARD_TEST_WORK/grouped-$3" ;;
            *'--add-assignee '*|*'--remove-assignee '*) ;;
            *) exit 2 ;;
        esac ;;
    'project item-add')
        case $BOARD_TEST_FAIL in
            project|remove|close) exit 1 ;;
            added)
                echo 'GraphQL: Content already exists in this project' >&2
                exit 1 ;;
        esac ;;
    'project item-edit')
        case "$*" in *'--id item '*) ;; *) exit 1 ;; esac
        [ "$BOARD_TEST_FAIL" != status ] || exit 1
        printf '%s\n' "$*" | sed 's/.*--single-select-option-id //' \
            > "$BOARD_TEST_WORK/status" ;;
    'issue close')
        [ "$BOARD_TEST_FAIL" != close ] || exit 1
        echo closed > "$BOARD_TEST_WORK/group" ;;
    *) exit 2 ;;
esac
EOF
chmod +x "$work/bin/gh"
BOARD_TEST_WORK=$work/state
BOARD_TEST_ME=
BOARD_TEST_STATE=OPEN
PATH=$work/bin:$PATH
export BOARD_TEST_WORK BOARD_TEST_ME BOARD_TEST_STATE PATH

# row FAILURE: check the exit status, queue labels and group left by one call.
row()
{
    BOARD_TEST_FAIL=$1
    export BOARD_TEST_FAIL
    rm -f "$BOARD_TEST_WORK/"*
    result=0
    sh "$work/board.sh" group fixtures 1 2 > "$work/out" 2> "$work/err" || result=$?
    case $1 in
        success|added)
            [ "$result" -eq 0 ] && [ "$(cat "$work/out")" = 99 ] &&
                [ ! -s "$work/err" ] &&
                [ "$(cat "$BOARD_TEST_WORK/group")" = open ] &&
                [ "$(cat "$BOARD_TEST_WORK/status")" = ready ] &&
                [ -f "$BOARD_TEST_WORK/grouped-1" ] &&
                [ -f "$BOARD_TEST_WORK/grouped-2" ] &&
                grep -q '^\* #1$' "$BOARD_TEST_WORK/body" &&
                grep -q '^\* #2$' "$BOARD_TEST_WORK/body" &&
                ! grep -q 'issue close\|remove-label' "$BOARD_TEST_WORK/calls" ;;
        validate)
            [ "$result" -ne 0 ] && [ ! -s "$work/out" ] &&
                [ ! -f "$BOARD_TEST_WORK/group" ] &&
                ! grep -q 'issue edit\|issue close\|project item-' "$BOARD_TEST_WORK/calls" ;;
        create|label1|label2|applied2|term|recheck|recheck-read)
            [ "$result" -ne 0 ] && [ ! -s "$work/out" ] &&
                [ ! -f "$BOARD_TEST_WORK/grouped-1" ] &&
                [ ! -f "$BOARD_TEST_WORK/grouped-2" ] &&
                [ ! -f "$BOARD_TEST_WORK/group" ] ;;
        remove)
            [ "$result" -ne 0 ] && [ ! -s "$work/out" ] &&
                [ -f "$BOARD_TEST_WORK/grouped-1" ] &&
                [ "$(cat "$BOARD_TEST_WORK/group")" = open ] &&
                grep -q 'issues/99: could not remove grouped from #1' "$work/err" &&
                grep -q 'issues/99: incomplete group kept open' "$work/err" &&
                ! grep -q 'issue close' "$BOARD_TEST_WORK/calls" ;;
        close)
            [ "$result" -ne 0 ] && [ ! -s "$work/out" ] &&
                [ ! -f "$BOARD_TEST_WORK/grouped-1" ] &&
                [ "$(cat "$BOARD_TEST_WORK/group")" = open ] &&
                grep -q 'issues/99: could not close the incomplete group' "$work/err" ;;
        *)
            [ "$result" -ne 0 ] && [ ! -s "$work/out" ] &&
                [ ! -f "$BOARD_TEST_WORK/grouped-1" ] &&
                [ ! -f "$BOARD_TEST_WORK/grouped-2" ] &&
                [ "$(cat "$BOARD_TEST_WORK/group")" = closed ] ;;
    esac
}

# take_row NAME ME NOW TIMELINE: take issue 7 as ME, with the accounts NOW
# assigned and TIMELINE the assignees of its assignment events in order, and
# check that ME keeps it only when NAME begins `keep`.
take_row()
{
    BOARD_TEST_FAIL=none BOARD_TEST_ME=$2 BOARD_TEST_NOW=$3 BOARD_TEST_TIMELINE=$4
    export BOARD_TEST_FAIL BOARD_TEST_NOW BOARD_TEST_TIMELINE
    rm -f "$BOARD_TEST_WORK/"*
    result=0
    sh "$work/board.sh" take 7 > "$work/out" 2> "$work/err" || result=$?
    BOARD_TEST_ME=
    case $1 in
        keep*)
            [ "$result" -eq 0 ] && [ -f "$BOARD_TEST_WORK/status" ] &&
                ! grep -q 'remove-assignee' "$BOARD_TEST_WORK/calls" ;;
        *)
            [ "$result" -ne 0 ] && [ ! -f "$BOARD_TEST_WORK/status" ] &&
                grep -q "remove-assignee $2\$" "$BOARD_TEST_WORK/calls" &&
                grep -q '^board: #7: taken by zed first$' "$work/err" ;;
    esac
}

# state_row NAME: run `take 7` or `release 7` as alpha, the one account
# assigned, on an issue that is closed or open as NAME says, and check the
# Status and assignees it leaves.
state_row()
{
    BOARD_TEST_FAIL=none BOARD_TEST_ME=alpha BOARD_TEST_NOW=alpha
    BOARD_TEST_TIMELINE=alpha
    case $1 in
        *-closed) BOARD_TEST_STATE=CLOSED ;;
        *) BOARD_TEST_STATE=OPEN ;;
    esac
    export BOARD_TEST_FAIL BOARD_TEST_NOW BOARD_TEST_TIMELINE BOARD_TEST_STATE
    rm -f "$BOARD_TEST_WORK/"*
    result=0
    sh "$work/board.sh" "${1%%-*}" 7 > "$work/out" 2> "$work/err" || result=$?
    BOARD_TEST_ME='' BOARD_TEST_STATE=OPEN
    case $1 in
        take-closed)
            [ "$result" -ne 0 ] && [ "$(cat "$BOARD_TEST_WORK/status")" = 'done' ] &&
                ! grep -q 'issue edit' "$BOARD_TEST_WORK/calls" &&
                grep -q '^board: #7: closed: set Done$' "$work/err" ;;
        release-closed)
            [ "$result" -eq 0 ] && [ "$(cat "$BOARD_TEST_WORK/status")" = 'done' ] &&
                ! grep -q 'issue edit' "$BOARD_TEST_WORK/calls" ;;
        release-open)
            [ "$result" -eq 0 ] && [ "$(cat "$BOARD_TEST_WORK/status")" = ready ] &&
                grep -q 'remove-assignee alpha' "$BOARD_TEST_WORK/calls" ;;
    esac
}

# rows: check successful creation, a group the board added first, each failed
# step and failed cleanup, and which of two accounts assigned in the same
# second keeps an issue, and what take and release do to a closed issue.
rows()
{
    failed=
    for test in success added validate create label1 label2 applied2 recheck recheck-read project item status term remove close; do
        if ! row "$test"; then
            echo "board-test: failed: $test" >&2
            cat "$work/err" >&2
            failed="$failed $test"
        fi
    done
    while read -r test me now timeline; do
        if ! take_row "$test" "$me" "$now" "$timeline"; then
            echo "board-test: failed: $test" >&2
            cat "$work/err" >&2
            failed="$failed $test"
        fi
    done <<'ROWS'
yield-later alpha zed,alpha zed,alpha
keep-earlier zed zed,alpha zed,alpha
yield-again alpha zed,alpha alpha,zed,alpha
ROWS
    for test in take-closed release-closed release-open; do
        if ! state_row "$test"; then
            echo "board-test: failed: $test" >&2
            cat "$work/err" >&2
            failed="$failed $test"
        fi
    done
    [ -z "$failed" ]
}
rows || exit 1
if [ "$mode" = mutate ]; then
    fault
    if rows > "$work/mutated.out" 2>&1; then
        echo 'board-test: fault passed: rollback omitted' >&2
        exit 1
    fi
    grep -q '^board-test: failed: label2$' "$work/mutated.out"
    take_fault
    if rows > "$work/mutated.out" 2>&1; then
        echo 'board-test: fault passed: latest assignment kept the issue' >&2
        exit 1
    fi
    grep -q '^board-test: failed: yield-later$' "$work/mutated.out"
    closed_fault
    if rows > "$work/mutated.out" 2>&1; then
        echo 'board-test: fault passed: a closed issue was taken as open' >&2
        exit 1
    fi
    grep -q '^board-test: failed: take-closed$' "$work/mutated.out"
    grep -q '^board-test: failed: release-closed$' "$work/mutated.out"
    cp "$repo/tools/flow/board.sh" "$work/board.sh"
    rows || exit 1
    echo 'board-test: result: pass: rollback, take and closed faults caught and restored'
fi
echo 'board-test: result: pass: 21 rows'
