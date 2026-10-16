#!/bin/sh
# board_test.sh: check board.sh against a local gh stand-in: group creation
# and rollback, take's choice of the first assignee, take and release of a
# closed issue, and the phase subcommands, which are current, members, next,
# start, the checks take and group make of the phase, archive and phase.
# --mutate plants each fault below in a private copy, requires a row of its
# own to fail, and then requires every row to pass with the copy restored;
# --texts requires only that each fault changes the copy, so that a fault
# whose text moved is refused.

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

# The faults of the phase rows, each a sed script for the private copy:
# membership ignores `Part of`; `current` keeps one of two phases; `start`
# looks for a comment it never wrote, and so comments again, or sets the phase
# In progress before it comments; `next` lists Ready issues of no phase; `take`
# skips the phase check, or takes a member that is not Ready; `group` accepts
# an issue of no phase; `archive` ignores the other open phase; and `phase`
# ignores the review.
# shellcheck disable=SC2016 # the dollars are in the texts the faults change
{
    members_fault='s/(parent == p || has(partof, p))/(parent == p)/'
    current_fault='s/s = s (s == "" ? "" : " #") \$3 }/s = $3 }/'
    start_fault="s/grep -Eqx 'phase: started at/grep -Eqx 'phase: begun at/"
    start_order_fault='s/^    said=\$(comments/    [ -n "$started" ] || set_status "$p" "In progress" || exit 1; &/'
    next_fault='s/member && \$2 == "Ready" {/$2 == "Ready" {/'
    take_phase_fault='s/g != "" || has(\$5, "ci") { exit }/{ exit }/'
    take_status_fault='s/\$2 != "Ready" \&\& \$2 != "In progress" {/0 {/'
    group_fault='s/^            member) ;;$/            *) ;;/'
    archive_fault='s/ || f\[8\] in open)/)/'
    phase_fault='s/\[ "\${review#\*..}" != "\$head" \]/false/'
}

# plant SCRIPT: write the private copy with the sed SCRIPT applied, refusing
# a script that changes nothing.
plant()
{
    sed "$1" "$repo/tools/flow/board.sh" > "$work/board.sh"
    ! cmp -s "$repo/tools/flow/board.sh" "$work/board.sh"
}
if [ "$mode" = texts ]; then
    fault
    take_fault
    closed_fault
    for f in "$members_fault" "$current_fault" "$start_fault" \
        "$start_order_fault" "$next_fault" "$take_phase_fault" \
        "$take_status_fault" "$group_fault" "$archive_fault" "$phase_fault"; do
        plant "$f"
    done
    exit 0
fi

# The stand-in keeps its issues in BOARD_TEST_DATA, one a line, as
# `N|STATE|STATUS|LABELS|ASSIGNEES|PARENT|BLOCKED|BODY|TITLE|COMMENTS|ARCHIVED`,
# BLOCKED being the number of open blockers, with `-` for an empty field, `_`
# for a space in STATUS, `;` between comments and `\n` for a newline in BODY.
# It answers each query with the lines that the query's --jq filter in
# board.sh prints, paginated two lines a page.
cat > "$work/bin/gh" <<'EOF'
#!/bin/sh
set -eu
printf '%s\n' "$*" >> "$BOARD_TEST_WORK/calls"
data=$BOARD_TEST_DATA
query='' body='' id='' item='' n='' prev=''
for a; do
    case $prev in
        --body) body=$a ;;
        --id) item=$a ;;
        -f|-F)
            case $a in
                query=*) query=${a#query=} ;;
                sub_issue_id=*) id=${a#sub_issue_id=} ;;
                n=*) n=${a#n=} ;;
            esac ;;
    esac
    prev=$a
done

# lines FORM: print the issues as board.sh's items, for FORM `board`, or
# issues, for `all` and `open`, read them from GitHub.
lines()
{
    awk -F '|' -v form="$1" '
        function named(b,   out) {
            out = ""
            while (match(b, /Part of #[0-9]+/)) {
                out = out (out == "" ? "" : ",") substr(b, RSTART + 9, RLENGTH - 9)
                b = substr(b, RSTART + RLENGTH)
            }
            return out
        }
        {
            for (i = 1; i <= 11; i++)
                f[i] = $i == "-" ? "" : $i
            status = f[3]
            gsub("_", " ", status)
        }
        form == "board" && f[11] != "archived" {
            print "I" f[1] "\t" status "\t" f[1] "\t" f[2] "\t" f[4] "\t" \
                (f[5] == "" ? 0 : split(f[5], a, ",")) "\t" f[7] "\t" f[6] "\t" \
                named(f[8]) "\t" f[9]
        }
        form == "all" || form == "open" && f[2] == "OPEN" {
            print f[1] "\t" f[2] "\t" \
                (f[2] == "CLOSED" ? "2026-10-01T00:00:00Z" : "-") "\t" f[4] "\t" \
                f[6] "\t" named(f[8]) "\t" f[9]
        }' "$data"
}

# pages NAME: print standard input as gh --paginate prints an answer two
# lines a page, failing after the first page when BOARD_TEST_LOST is NAME.
pages()
{
    awk -v lost="$BOARD_TEST_LOST" -v name="$1" '
        NR > 2 && lost == name { exit 1 }
        { print }'
}

# field N K: print field K of issue N.
field()
{
    awk -F '|' -v n="$1" -v k="$2" '$1 == n { print $k }' "$data"
}

# change N K VALUE: set field K of issue N to VALUE.
change()
{
    V=$3 awk -F '|' -v OFS='|' -v n="$1" -v k="$2" '
        $1 == n { $k = ENVIRON["V"] }
        { print }' "$data" > "$data.new"
    mv "$data.new" "$data"
}

case "$1 $2" in
    'api user')
        echo "$BOARD_TEST_ME" ;;
    'api --paginate')
        case $3 in
            */timeline) printf '%s\n' "$BOARD_TEST_TIMELINE" | tr , '\n' ;;
            */comments)
                p=${3%/comments}
                field "${p##*/}" 10 | tr ';' '\n' | grep -v '^-\{0,1\}$' || : ;;
            *) exit 2 ;;
        esac ;;
    'api graphql')
        case $query in
            *'field(name: "Status")'*)
                printf '%s\n' project=project field=field Backlog=backlog \
                    Ready=ready In_progress=progress Done=done ;;
            *'projectItems(first: 10)'*)
                case $BOARD_TEST_FAIL in
                    item) exit 1 ;;
                    project|remove|close) exit 0 ;;
                esac
                echo "I$n" ;;
            *'items(first: 100'*) lines board | pages board ;;
            *'states: [OPEN]'*) lines open | pages issues ;;
            *'issues(first: 100'*) lines all | pages issues ;;
            *) exit 2 ;;
        esac ;;
    'api -X')
        if [ "$BOARD_TEST_FULL" = yes ]; then
            echo 'gh: the parent has the most sub-issues it may have (HTTP 422)' >&2
            exit 1
        fi
        p=${4%/sub_issues}
        change "$((id - 1000))" 6 "${p##*/}" ;;
    'api repos/'*)
        case $2 in
            */commits/master) echo "$BOARD_TEST_HEAD" ;;
            */status) echo "$BOARD_TEST_CORE" ;;
            */issues/*) echo "$((1000 + ${2##*/}))" ;;
            *) exit 2 ;;
        esac ;;
    'issue view')
        case "$*" in
            *'--json state --jq .state'*)
                echo "$BOARD_TEST_STATE"
                exit 0 ;;
            *'--json body --jq .body'*)
                field "$3" 8 | sed 's/^-$//' | awk '{ gsub(/\\n/, "\n"); print }'
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
            *' --body '*)
                change "$3" 8 "$(printf '%s\n' "$body" |
                    awk 'NR > 1 { printf "\\n" } { printf "%s", $0 }')" ;;
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
    'issue comment')
        said=$(field "$3" 10)
        [ "$said" != - ] || said=''
        change "$3" 10 "${said:+$said;}$body" ;;
    'project item-add')
        case $BOARD_TEST_FAIL in
            project|remove|close) exit 1 ;;
            added)
                echo 'GraphQL: Content already exists in this project' >&2
                exit 1 ;;
        esac ;;
    'project item-edit')
        case "$*" in *'--id I'*) ;; *) exit 1 ;; esac
        [ "$BOARD_TEST_FAIL" != status ] || exit 1
        option=$(printf '%s\n' "$*" | sed 's/.*--single-select-option-id //')
        printf '%s\n' "$option" > "$BOARD_TEST_WORK/status"
        case $option in
            backlog) option=Backlog ;;
            ready) option=Ready ;;
            progress) option=In_progress ;;
            *) option=Done ;;
        esac
        change "${item#I}" 3 "$option" ;;
    'project item-archive')
        [ "$BOARD_TEST_FAIL" != "archive${item#I}" ] || exit 1
        change "${item#I}" 11 archived ;;
    'issue close')
        [ "$BOARD_TEST_FAIL" != close ] || exit 1
        echo closed > "$BOARD_TEST_WORK/group" ;;
    *) exit 2 ;;
esac
EOF
chmod +x "$work/bin/gh"
BOARD_TEST_WORK=$work/state
BOARD_TEST_DATA=$work/data
BOARD_TEST_ME=''
BOARD_TEST_NOW=''
BOARD_TEST_TIMELINE=''
BOARD_TEST_STATE=OPEN
BOARD_TEST_FAIL=none
BOARD_TEST_LOST=''
BOARD_TEST_FULL=no
BOARD_TEST_CORE=success
B=1111111111111111111111111111111111111111
H=2222222222222222222222222222222222222222
C=3333333333333333333333333333333333333333
BOARD_TEST_HEAD=$H
PATH=$work/bin:$PATH
export BOARD_TEST_WORK BOARD_TEST_DATA BOARD_TEST_ME BOARD_TEST_NOW \
    BOARD_TEST_TIMELINE BOARD_TEST_STATE BOARD_TEST_FAIL BOARD_TEST_LOST \
    BOARD_TEST_FULL BOARD_TEST_CORE BOARD_TEST_HEAD PATH
tab=$(printf '\t')

# The issues of the group, take and release rows, each filed by ci.
old='1|OPEN|Ready|ci|-|-|0|-|ci: one
2|OPEN|Ready|ci|-|-|0|-|ci: two
7|OPEN|Ready|ci|-|-|0|-|ci: seven'

# A board with phase 7 In progress and phase 8 Ready. Issue 12 is no member
# of 7, since its body says `Part of #73`.
main='7|OPEN|In_progress|phase|-|-|0|-|phase: seven
8|OPEN|Ready|phase|-|-|0|-|phase: eight
10|OPEN|Ready|bug|-|7|0|-|linked member
11|OPEN|Ready|bug|-|-|0|Found in review. Part of #7.|named member
12|OPEN|Ready|bug|-|-|0|Part of #73.|member of 73
13|OPEN|Ready|bug|-|8|0|-|member of eight
14|OPEN|Backlog|ci|-|-|0|-|ci: core fails
15|OPEN|Ready|ci,intermittent|-|-|0|-|ci: night fails once
16|OPEN|Ready|bug,owner|-|7|0|-|owner member
17|OPEN|Ready|bug|-|7|1|-|blocked member
18|OPEN|Ready|bug|alpha|7|0|-|assigned member
19|CLOSED|Done|bug|-|7|0|-|closed member
20|CLOSED|Done|bug|-|8|0|-|closed member of eight
21|CLOSED|Done|bug|-|-|0|-|closed issue of no phase
22|OPEN|Ready|ci|-|-|0|-|ci: style fails
23|OPEN|Ready|bug,grouped|-|7|0|-|grouped member
24|OPEN|Ready|phase|-|7|0|-|phase under seven'

# Phase 7 with no open member but an intermittent ci issue.
quiet='7|OPEN|In_progress|phase|-|-|0|-|phase: seven
14|OPEN|Backlog|ci|-|-|0|-|ci: core fails
15|OPEN|Ready|ci,intermittent|-|-|0|-|ci: night fails once
19|CLOSED|Done|bug|-|7|0|-|closed member'

# A board before `start`, with phases 5 and 6 Ready.
begin='5|OPEN|Ready|phase|-|-|0|-|phase: five
6|OPEN|Ready|phase|-|-|0|-|phase: six
30|OPEN|Backlog|bug|-|5|0|-|backlog member
31|OPEN|-|bug|-|-|0|Part of #5.|member with no Status
32|OPEN|In_progress|bug|alpha|5|0|-|member in progress
33|OPEN|Backlog|bug,owner|-|5|0|-|owner member
34|OPEN|Backlog|bug,triaged|-|-|0|Seen in a log.|triaged orphan
35|OPEN|Backlog|ci,triaged|-|-|0|-|ci: triaged
36|OPEN|Backlog|bug|-|-|0|-|untriaged orphan
37|OPEN|Backlog|bug,triaged|-|6|0|-|member of six
38|OPEN|Backlog|bug,grouped|-|5|0|-|grouped member
39|OPEN|Ready|bug|-|5|0|-|ready member'

# Phase 7 with its members closed, reviewed at master's head H.
review="7|OPEN|In_progress|phase|-|-|0|-|phase: seven|phase: started at $B;Looks right.;review: $B..$H complete;phase: started at $C
10|CLOSED|Done|bug|-|7|0|-|done member
11|CLOSED|Done|bug|-|-|0|Part of #7.|named done member
12|CLOSED|Done|ci|-|-|0|-|ci: fixed
13|OPEN|Ready|bug|-|-|0|-|issue of no phase"

# use ISSUES [SCRIPT]: give the stand-in ISSUES, edited by the sed SCRIPT
# when given, and the default settings.
use()
{
    printf '%s\n' "$1" | sed "${2:-}" > "$BOARD_TEST_DATA"
    BOARD_TEST_FAIL=none BOARD_TEST_LOST='' BOARD_TEST_FULL=no
    BOARD_TEST_CORE=success BOARD_TEST_ME='' BOARD_TEST_NOW=''
    BOARD_TEST_TIMELINE='' BOARD_TEST_STATE=OPEN
}

# run ARGS...: run the private copy with ARGS, keeping its output in out, its
# errors in err and its exit status in `result`.
run()
{
    rm -f "$BOARD_TEST_WORK/"*
    result=0
    sh "$work/board.sh" "$@" > "$work/out" 2> "$work/err" || result=$?
}

# words: print the first field of each line of out, joined by single spaces.
words()
{
    cut -f 1 "$work/out" | tr '\n' ' ' | sed 's/  */ /g; s/^ //; s/ $//'
}

# said LINE: succeed when out has the line LINE.
said()
{
    grep -qxF "$1" "$work/out"
}

# statuses: print each issue's number and Status as `N:STATUS`.
statuses()
{
    awk -F '|' '{ printf "%s%s:%s", s, $1, $3; s = " " } END { print "" }' \
        "$BOARD_TEST_DATA"
}

# field N K: print field K of issue N.
field()
{
    awk -F '|' -v n="$1" -v k="$2" '$1 == n { print $k }' "$BOARD_TEST_DATA"
}

# row FAILURE: check the exit status, queue labels and group left by one call.
row()
{
    use "$old"
    BOARD_TEST_FAIL=$1
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
    use "$old"
    BOARD_TEST_FAIL=none BOARD_TEST_ME=$2 BOARD_TEST_NOW=$3 BOARD_TEST_TIMELINE=$4
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
    use "$old"
    BOARD_TEST_FAIL=none BOARD_TEST_ME=alpha BOARD_TEST_NOW=alpha
    BOARD_TEST_TIMELINE=alpha
    case $1 in
        *-closed) BOARD_TEST_STATE=CLOSED ;;
        *) BOARD_TEST_STATE=OPEN ;;
    esac
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

# phase_row NAME: run the row NAME of the phase subcommands and check what
# it prints and what it leaves in the stand-in's issues.
phase_row()
{
    case $1 in
        current-none)
            use "$main" 's/^7|OPEN|In_progress|/7|OPEN|Ready|/'
            run current
            [ "$result" -eq 0 ] && [ ! -s "$work/out" ] ;;
        current-one)
            use "$main"
            run current
            [ "$result" -eq 0 ] && [ "$(cat "$work/out")" = 7 ] ;;
        current-two)
            use "$main" 's/^8|OPEN|Ready|/8|OPEN|In_progress|/'
            run current
            [ "$result" -ne 0 ] && [ ! -s "$work/out" ] &&
                grep -q '#7 #8 are In progress' "$work/err" ;;
        members-all)
            use "$main"
            run members 7
            [ "$result" -eq 0 ] && [ "$(words)" = '10 11 16 17 18 19 23 24' ] &&
                said "11${tab}OPEN${tab}-${tab}bug${tab}named member" &&
                said "19${tab}CLOSED${tab}2026-10-01T00:00:00Z${tab}bug${tab}closed member" ;;
        members-open)
            use "$main"
            run members 7 open
            [ "$result" -eq 0 ] && [ "$(words)" = '10 11 16 17 18 23 24' ] ;;
        members-lost)
            use "$main"
            BOARD_TEST_LOST=issues
            run members 7
            [ "$result" -ne 0 ] && [ ! -s "$work/out" ] && [ -s "$work/err" ] ;;
        next-tiers)
            use "$main"
            run next
            [ "$result" -eq 0 ] && [ "$(words)" = '14 22 10 11 15' ] &&
                said "14${tab}ci: core fails" ;;
        next-idle)
            use "$main" 's/^7|OPEN|In_progress|/7|OPEN|Ready|/'
            run next
            [ "$result" -eq 0 ] && [ "$(words)" = '14 22' ] ;;
        next-done)
            use "$quiet"
            run next
            [ "$result" -eq 0 ] && [ "$(words)" = 14 ] ;;
        next-lost)
            use "$main"
            BOARD_TEST_LOST=board
            run next
            [ "$result" -ne 0 ] && [ ! -s "$work/out" ] && [ -s "$work/err" ] ;;
        take-member|take-ci)
            use "$main"
            BOARD_TEST_ME=alpha BOARD_TEST_NOW=alpha BOARD_TEST_TIMELINE=alpha
            case $1 in take-member) n=10 ;; *) n=14 ;; esac
            run take "$n"
            [ "$result" -eq 0 ] && [ "$(field "$n" 3)" = In_progress ] ;;
        take-stranger|take-blocked|take-owner|take-phase|take-backlog)
            use "$main" 's/^10|OPEN|Ready|/10|OPEN|Backlog|/'
            BOARD_TEST_ME=alpha BOARD_TEST_NOW=alpha BOARD_TEST_TIMELINE=alpha
            case $1 in
                take-stranger) n=13 why='neither labelled ci nor a member of #7' ;;
                take-blocked) n=17 why='it has an open blocker' ;;
                take-owner) n=16 why='it waits for the owner' ;;
                take-backlog) n=10 why='its Status is Backlog, not Ready' ;;
                *) n=8 why='it is a phase' ;;
            esac
            run take "$n"
            [ "$result" -ne 0 ] && grep -q "^board: #$n: .*$why" "$work/err" &&
                ! grep -q 'issue edit\|item-edit' "$BOARD_TEST_WORK/calls" ;;
        group-stranger)
            use "$main"
            run group fixtures 13
            [ "$result" -ne 0 ] && [ ! -s "$work/out" ] &&
                ! grep -q 'issue edit\|issue create' "$BOARD_TEST_WORK/calls" ;;
        group-ci)
            use "$main"
            run group fixtures 14 10
            [ "$result" -eq 0 ] && [ "$(cat "$work/out")" = 99 ] &&
                grep -q -- '--label group,ci ' "$BOARD_TEST_WORK/body" &&
                grep -qx '\* #14' "$BOARD_TEST_WORK/body" &&
                grep -qx '\* #10' "$BOARD_TEST_WORK/body" &&
                grep -qx 'Part of #7\.' "$BOARD_TEST_WORK/body" &&
                grep -q 'issues/7/sub_issues -F sub_issue_id=1099$' "$BOARD_TEST_WORK/calls" ;;
        archive-other)
            use "$main"
            run archive 7
            [ "$result" -eq 0 ] && [ "$(words)" = '19 21' ] &&
                [ "$(field 19 11)" = archived ] && [ "$(field 21 11)" = archived ] &&
                [ -z "$(field 20 11)" ] ;;
        archive-closed)
            use "$main" 's/^7|OPEN|In_progress|/7|CLOSED|Done|/'
            run archive 7
            [ "$result" -eq 0 ] && [ "$(words)" = '7 19 21' ] ;;
        archive-fail)
            use "$main"
            BOARD_TEST_FAIL=archive19
            run archive 7
            [ "$result" -ne 0 ] && [ "$(words)" = 21 ] &&
                grep -q '^board: #19: cannot archive its card$' "$work/err" ;;
        start-first)
            use "$begin"
            run start
            [ "$result" -eq 0 ] && [ "$(cat "$work/out")" = 5 ] &&
                [ "$(statuses)" = '5:In_progress 6:Ready 30:Ready 31:Ready 32:In_progress 33:Backlog 34:Ready 35:Backlog 36:Backlog 37:Backlog 38:Backlog 39:Ready' ] &&
                [ "$(field 34 6)" = 5 ] && [ "$(field 5 10)" = "phase: started at $H" ] ;;
        start-again)
            use "$begin"
            run start
            cp "$BOARD_TEST_DATA" "$work/first"
            run start
            [ "$result" -eq 0 ] && [ "$(cat "$work/out")" = 5 ] &&
                cmp -s "$BOARD_TEST_DATA" "$work/first" &&
                ! grep -q 'item-edit\|issue comment\|-X POST' "$BOARD_TEST_WORK/calls" ;;
        start-retry)
            use "$begin"
            BOARD_TEST_FAIL=status
            run start
            first=$result
            BOARD_TEST_FAIL=none BOARD_TEST_HEAD=$C
            run start
            BOARD_TEST_HEAD=$H
            [ "$first" -ne 0 ] && [ "$result" -eq 0 ] &&
                [ "$(field 5 3)" = In_progress ] &&
                [ "$(field 5 10)" = "phase: started at $H" ] ;;
        start-full)
            use "$begin"
            BOARD_TEST_FULL=yes
            run start
            [ "$result" -eq 0 ] && [ "$(field 34 3)" = Ready ] &&
                [ "$(field 34 6)" = - ] &&
                [ "$(field 34 8)" = 'Seen in a log.\n\nPart of #5.' ] &&
                grep -q '^board: #34: not linked to #5' "$work/err" ;;
        start-other)
            use "$begin" 's/^5|OPEN|Ready|/5|OPEN|In_progress|/'
            run start 6
            [ "$result" -ne 0 ] && [ ! -s "$work/out" ] &&
                grep -q '#5 is In progress' "$work/err" &&
                ! grep -q 'item-edit\|issue comment' "$BOARD_TEST_WORK/calls" ;;
        start-idle)
            use "$begin" 's/^\([56]\)|OPEN|Ready|/\1|OPEN|Backlog|/'
            run start
            [ "$result" -eq 0 ] && [ ! -s "$work/out" ] &&
                ! grep -q 'item-edit' "$BOARD_TEST_WORK/calls" ;;
        phase-ready)
            use "$review"
            run phase
            [ "$result" -eq 0 ] && [ "$(cat "$work/out")" = "phase: #7 phase: seven
base: $B
head: $H
members: 0 open, 2 closed
work: none
intermittent: none
hold: none
review: $B..$H
core: success
ready: yes $H" ] ;;
        phase-none)
            use "$review" 's/^7|OPEN|In_progress|/7|OPEN|Ready|/'
            run phase
            [ "$result" -eq 0 ] && said 'phase: none' && said 'base: unknown' &&
                [ "$(tail -n 1 "$work/out")" = 'ready: no: no phase is In progress' ] ;;
        phase-work)
            use "$review
14|OPEN|Ready|bug|-|7|0|-|open member
15|OPEN|-|ci|-|-|0|-|ci: core fails"
            run phase
            [ "$result" -eq 0 ] && said 'members: 1 open, 2 closed' &&
                said 'work: #14 #15' &&
                [ "$(tail -n 1 "$work/out")" = 'ready: no: work: #14 #15' ] ;;
        phase-flaky)
            use "$review
16|OPEN|-|ci,intermittent|-|-|0|-|ci: night fails once"
            run phase
            [ "$result" -eq 0 ] && said 'intermittent: #16' &&
                [ "$(tail -n 1 "$work/out")" = "ready: yes $H" ] ;;
        phase-hold)
            use "$review
17|OPEN|-|hold|-|-|0|-|hold: core fails"
            run phase
            [ "$result" -eq 0 ] && said 'hold: #17' &&
                [ "$(tail -n 1 "$work/out")" = 'ready: no: hold #17' ] ;;
        phase-failed)
            use "$review" 's/^7|OPEN|In_progress|phase|/7|OPEN|In_progress|phase,review-failed|/'
            run phase
            [ "$result" -eq 0 ] &&
                [ "$(tail -n 1 "$work/out")" = 'ready: no: review failed' ] ;;
        phase-review)
            use "$review" "s/review: $B..$H complete/&;review: $B..$C complete/"
            run phase
            [ "$result" -eq 0 ] && said "review: $B..$C" &&
                [ "$(tail -n 1 "$work/out")" = 'ready: no: review' ] ;;
        phase-core)
            use "$review"
            BOARD_TEST_CORE=failure
            run phase
            [ "$result" -eq 0 ] && said 'core: failure' &&
                [ "$(tail -n 1 "$work/out")" = 'ready: no: core failure' ] ;;
        phase-lost)
            use "$review"
            BOARD_TEST_LOST=issues
            run phase
            [ "$result" -eq 1 ] &&
                [ "$(tail -n 1 "$work/out")" = 'ready: no: unreadable: issues' ] ;;
        *) return 1 ;;
    esac
}

# rows: check successful creation, a group the board added first, each failed
# step and failed cleanup, and which of two accounts assigned in the same
# second keeps an issue, what take and release do to a closed issue, and the
# rows of the phase subcommands.
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
    for test in current-none current-one current-two \
        members-all members-open members-lost \
        next-tiers next-idle next-done next-lost \
        take-member take-ci take-stranger take-blocked take-owner take-phase \
        take-backlog \
        group-stranger group-ci archive-other archive-closed archive-fail \
        start-first start-again start-retry start-full start-other \
        start-idle \
        phase-ready phase-none phase-work phase-flaky phase-hold \
        phase-failed phase-review phase-core phase-lost; do
        if ! phase_row "$test"; then
            echo "board-test: failed: $test" >&2
            cat "$work/err" >&2
            failed="$failed $test"
        fi
    done
    [ -z "$failed" ]
}

# caught SCRIPT ROW WHAT: plant the fault SCRIPT and require the rows to fail,
# ROW among them; WHAT says what the fault does.
caught()
{
    plant "$1"
    if rows > "$work/mutated.out" 2>&1; then
        echo "board-test: fault passed: $3" >&2
        exit 1
    fi
    grep -q "^board-test: failed: $2\$" "$work/mutated.out"
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
    caught "$members_fault" members-all 'membership ignored Part of'
    caught "$current_fault" current-two 'two phases In progress passed'
    caught "$start_fault" start-again 'a second start commented again'
    caught "$start_order_fault" start-retry 'start set In progress before it commented'
    caught "$next_fault" next-tiers 'next listed issues of no phase'
    caught "$take_phase_fault" take-stranger 'take skipped the phase check'
    caught "$take_status_fault" take-backlog 'take took a Backlog member'
    caught "$group_fault" group-stranger 'group took an issue of no phase'
    caught "$archive_fault" archive-other 'archive ignored the other phase'
    caught "$phase_fault" phase-review 'phase ignored the review'
    cp "$repo/tools/flow/board.sh" "$work/board.sh"
    rows || exit 1
    echo 'board-test: result: pass: 13 faults caught and restored'
fi
echo 'board-test: result: pass: 58 rows'
