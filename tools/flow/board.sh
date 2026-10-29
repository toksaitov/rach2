#!/bin/sh
#
# board.sh: the operations the worker loops, the manager and the release make
# on the board and on the milestones.
#
# Usage: board.sh next
#        board.sh take N
#        board.sh mine N
#        board.sh release N [owner|owner-only]
#        board.sh branch N
#        board.sh ready N...
#        board.sh done
#        board.sh archive M
#        board.sh milestones
#
# A milestone is a GitHub milestone, and its description names its branch on
# a line `Branch: NAME`. A task is an issue in a milestone with no parent
# issue, and a subtask is a sub-issue of a task. Workers take tasks; a subtask
# is closed by the worker of its task and is never taken by itself.
# An issue labelled `owner-only` has work that only the owner's accounts can
# do, such as a step on the testing host through `ssh testing` from the
# owner's computer; `next` and `take` offer it only to the account OWNER below.
# Each subcommand that reads the board fails, with a diagnostic on stderr and
# nothing on stdout, when a page of the answer is lost five times, as `ask`
# below says, so that a caller does not take a part of the board for the
# whole.
# Assignees are added and removed with the REST calls that change only the
# login given. `gh issue edit` writes back the whole list it read, and so puts
# back an account that another `take` removed in the meantime.
#
# `next` prints the tasks a worker may take, one line each with the number, a
# tab and the title. Each is open, Ready, unassigned, in an open milestone
# whose description names a branch, with no parent and no open blocker, not
# labelled `owner`, and labelled `owner-only` only when this account is OWNER.
# They are ordered by milestone number, lowest first, and then in the board's
# order. `gh project item-list` does not return that order, so the query asks
# for the items by position.
#
# `take N` refuses a closed issue, setting it Done and changing nothing else,
# since a close between `next` and `take` would otherwise leave it In
# progress. Before it assigns anything, it also refuses an issue that is not
# on the board, a subtask, an issue in no open milestone or in one that names
# no branch, one with an open blocker, one labelled `owner`, one labelled
# `owner-only` when this account is not OWNER, and one whose Status is
# neither Ready nor In progress. Otherwise it assigns N to this account and
# reads the issue's timeline. Of the accounts assigned now, the one whose
# latest assignment is the earliest keeps the issue. When that is another
# account, `take` unassigns this one and exits 1, so that no issue has two
# writers; otherwise it sets N In progress. A step that fails after the
# assignment unassigns this account again, unless N was this account's
# before, so that a failed take leaves the issue as it found it.
#
# `mine N` prints `yes` while N is open and assigned to this account and `no`
# otherwise; it prints nothing and fails when GitHub cannot be reached, so
# that a caller does not take a lost connection for a closed issue. It makes
# none of the checks of `take`: a task a worker holds is resumed after it
# gains a blocker or the label `owner`, since its work may be only in that
# worker's clone, and the session that reads the issue hands it back with
# `release` when it cannot go on.
#
# `release N` hands N back: it sets it Ready and unassigns this account.
# `release N owner` sets Backlog and adds the label `owner`, which marks a
# decision that AGENTS.md gives the owner. `release N owner-only` sets Ready
# and adds the label `owner-only`, for work that is no decision but that only
# the owner's accounts can do. A step that fails exits 1 with N still
# assigned to this account, so that its worker keeps it and can release it
# again. A closed issue is set Done and nothing else is changed, since its
# commit may have closed it before its worker hands it back.
#
# `branch N` prints the branch that the description of N's milestone names,
# and fails when N has no milestone or the milestone names no branch.
#
# `ready N...` sets each N Ready.
#
# `done` sets Done on every closed issue whose Status is not, and fails when
# the listing of the board fails.
#
# `archive M` archives the cards of the closed issues of milestone M and of
# their closed subtasks, and prints the numbers it archived; release.sh runs
# it once it has closed M. An open issue keeps its card. A card that cannot be
# archived is reported on stderr and the rest go on, and the exit status is
# then 1.
#
# `milestones` prints each open milestone, lowest number first, as a line
# `milestone M: TITLE`, the line `  branch: NAME`, or `  branch: none`, the
# line `  tasks: O open, C closed`, and a line `  #N In progress: LOGINS:
# TITLE` for each task In progress, LOGINS being its assignees or `nobody`.
#
# The repository and the board's owner and number are below. The board's
# owner is the owner's account, the one that may take an issue labelled
# `owner-only`. The ids of the board, its Status field and the field's
# options are looked up by name on each call, so another board needs only
# these three lines changed.

# shellcheck disable=SC2016 # the dollars are GraphQL's and jq's
set -e

REPO=toksaitov/rach2
OWNER=toksaitov
BOARD=6

# The awk function has(L, X) is true when the comma-joined list L has X.
LIB='function has(l, x) { return index("," l ",", "," x ",") > 0 }'

# ask ARGS...: print the answer of the read `gh api graphql ARGS...`. GitHub
# at times answers a query with an error page, for minutes at a time, so a
# read that fails is made again, five tries in all, BOARD_WAIT seconds apart,
# ten unless it is set; fail when the fifth fails.
ask()
{
    ask_try=1
    until ask_out=$(gh api graphql "$@"); do
        [ "$ask_try" -lt 5 ] || return 1
        ask_try=$((ask_try + 1))
        sleep "${BOARD_WAIT:-10}"
    done
    [ -z "$ask_out" ] || printf '%s\n' "$ask_out"
}

# pages CONNECTION FILTER ARGS...: print what the jq FILTER prints for each
# node of the connection at the path CONNECTION, such as
# `.data.repository.issues`, in the answer to the query that ARGS give, every
# page, each read by `ask`, so that one lost page costs a try and not the
# whole read. The query takes `$endCursor` and asks for the connection's
# `pageInfo { hasNextPage endCursor }`.
pages()
{
    pages_jq="($1.nodes[] | $2),
        \"@page \\($1.pageInfo.hasNextPage) \\($1.pageInfo.endCursor)\""
    shift 2
    pages_after=
    while :; do
        pages_got=$(ask ${pages_after:+-f endCursor="$pages_after"} "$@" \
            --jq "$pages_jq") || return 1
        pages_last=$(printf '%s\n' "$pages_got" | tail -n 1)
        printf '%s\n' "$pages_got" | sed '$d'
        case $pages_last in
            '@page true '?*) pages_after=${pages_last#@page true } ;;
            '@page false '*) return 0 ;;
            *) echo "board: a page of the answer has no pageInfo" >&2; return 1 ;;
        esac
    done
}

# ids: print the board's id as `project=ID`, its Status field's as `field=ID`,
# and each option of Status as `NAME=ID`, spaces in NAME written as `_`.
ids()
{
    gh api graphql -F n="$BOARD" -f o="$OWNER" -f query='
        query($o: String!, $n: Int!) {
            user(login: $o) { projectV2(number: $n) { id
                field(name: "Status") { ... on ProjectV2SingleSelectField {
                    id options { id name } } } } } }' \
        --jq '.data.user.projectV2 | "project=\(.id)", "field=\(.field.id)",
            (.field.options[] | "\(.name | gsub(" "; "_"))=\(.id)")'
}

# id_of NAME: print the id `ids` gave NAME.
id_of()
{
    printf '%s\n' "$board_ids" | sed -n "s/^$1=//p"
}

# item N: print the id of issue N's item on the board.
item()
{
    gh api graphql -F n="$1" -f r="${REPO#*/}" -f o="${REPO%/*}" -f query='
        query($o: String!, $r: String!, $n: Int!) {
            repository(owner: $o, name: $r) { issue(number: $n) {
                projectItems(first: 10) { nodes { id project { id } } } } } }' \
        --jq ".data.repository.issue.projectItems.nodes[] |
            select(.project.id == \"$(id_of project)\") | .id"
}

# set_status N NAME: set issue N's Status to the option NAME.
set_status()
{
    gh project item-edit --project-id "$(id_of project)" \
        --field-id "$(id_of field)" --id "$(item "$1")" \
        --single-select-option-id "$(id_of "$(echo "$2" | tr ' ' _)")" > /dev/null
}

# BRANCH is the jq filter that prints the branch that the description of the
# milestone `.milestone` names on its first line `Branch: NAME`, without the
# backquotes and spaces that a person may write around NAME, or nothing.
BRANCH='([.milestone.description // "" | split("\n")[] |
        select(startswith("Branch:")) | ltrimstr("Branch:") |
        gsub("[\\s`]"; "")] | map(select(. != "")) | first // "")'

# FIELDS is the jq filter that prints the twelve fields of `items` from an
# object with the item's id and Status as `id` and `status` and the issue as
# `issue`.
FIELDS='[.id, .status, (.issue.number | tostring), .issue.state,
    ([.issue.labels.nodes[].name] | join(",")),
    ([.issue.assignees.nodes[].login] | join(",")),
    (.issue.issueDependenciesSummary.blockedBy | tostring),
    (.issue.parent.number // "" | tostring),
    (.issue.milestone.number // "" | tostring),
    (.issue.milestone.state // ""),
    (.issue | '"$BRANCH"'),
    (.issue.title | gsub("\t"; " "))] | join("\t")'

# ISSUE is the part of a query that asks for the fields FIELDS reads.
ISSUE='number title state
    assignees(first: 10) { nodes { login } }
    labels(first: 20) { nodes { name } }
    parent { number }
    milestone { number state description }
    issueDependenciesSummary { blockedBy }'

# items [all]: print the issues on the board in the board's order, one a
# line, as twelve fields separated by tabs: the item's id, the Status or
# nothing, the number, the state, the labels joined by commas, the assignees'
# logins joined by commas, the number of open blockers, the parent's number
# or nothing, the milestone's number, its state and the branch it names, each
# or nothing, and the title. GitHub's list of the board's items can lack an
# item that the issue's own `projectItems` gives, so the repository's open
# issues, or with `all` every issue, are read as well, and an issue whose
# item the list lacks follows the others, in the order the issues were
# opened. Fail when any page of either answer is lost.
items()
{
    states=OPEN
    [ "$1" != all ] || states='OPEN, CLOSED'
    listed=$(pages .data.user.projectV2.items '
            select(.content.number != null and (.isArchived | not)) |
            {id, status: (.fieldValueByName.name // ""), issue: .content} |
            '"$FIELDS" -F n="$BOARD" -f o="$OWNER" -f query='
        query($o: String!, $n: Int!, $endCursor: String) {
            user(login: $o) { projectV2(number: $n) {
                items(first: 100, after: $endCursor,
                      orderBy: {field: POSITION, direction: ASC}) {
                    pageInfo { hasNextPage endCursor }
                    nodes { id isArchived
                        fieldValueByName(name: "Status") {
                            ... on ProjectV2ItemFieldSingleSelectValue { name } }
                        content { ... on Issue { '"$ISSUE"' } } } } } } }') ||
        return 1
    found=$(pages .data.repository.issues '. as $issue |
            .projectItems.nodes[] |
            select(.project.number == '"$BOARD"' and
                   .project.owner.login == "'"$OWNER"'") |
            {id, status: (.fieldValueByName.name // ""), issue: $issue} |
            '"$FIELDS" -f o="${REPO%/*}" -f r="${REPO#*/}" -f query='
        query($o: String!, $r: String!, $endCursor: String) {
            repository(owner: $o, name: $r) {
                issues(first: 100, after: $endCursor, states: ['"$states"'],
                       orderBy: {field: CREATED_AT, direction: ASC}) {
                    pageInfo { hasNextPage endCursor }
                    nodes { '"$ISSUE"'
                        projectItems(first: 10, includeArchived: false) {
                            nodes { id project { number
                                    owner { ... on User { login } } }
                                fieldValueByName(name: "Status") {
                                    ... on ProjectV2ItemFieldSingleSelectValue {
                                        name } } } } } } } }') || return 1
    printf '%s\n' "$listed" "$found" | awk -F '\t' 'NF > 0 && !seen[$1]++'
}

# read_board [all]: read the board into `board`, as `items` prints it, or say
# that it cannot and exit 1.
read_board()
{
    board=$(items "$1") || {
        echo "board: cannot read the board" >&2
        exit 1
    }
}

# login: set `me` to this account's login, or say that it cannot and exit 1.
login()
{
    me=$(gh api user --jq .login) && [ -n "$me" ] || {
        echo "board: cannot read this account's login" >&2
        exit 1
    }
}

next()
{
    login
    read_board
    printf '%s\n' "$board" | awk -F '\t' -v me="$me" -v owner="$OWNER" "$LIB"'
        $4 != "OPEN" || $2 != "Ready" || $6 != "" || $7 > 0 || $8 != "" { next }
        $10 != "OPEN" || $11 == "" || has($5, "owner") { next }
        has($5, "owner-only") && me != owner { next }
        { print $9 "\t" NR "\t" $3 "\t" $12 }' |
        sort -n -k 1,1 -k 2,2 | cut -f 3-
}

# finished N: when issue N is closed, set it Done, say so and succeed;
# otherwise fail. A state or Status that cannot be read or set exits 1.
finished()
{
    state=$(gh issue view "$1" --repo "$REPO" --json state --jq .state) || {
        echo "board: #$1: cannot read its state" >&2
        exit 1
    }
    [ "$state" = CLOSED ] || return 1
    set_status "$1" Done || {
        echo "board: #$1: closed, but cannot set it Done" >&2
        exit 1
    }
    echo "board: #$1: closed: set Done" >&2
}

# assign N LOGIN: add LOGIN to N's assignees.
assign()
{
    gh api -X POST "repos/$REPO/issues/$1/assignees" -f "assignees[]=$2" > /dev/null
}

# unassign N LOGIN: remove LOGIN from N's assignees.
unassign()
{
    gh api -X DELETE "repos/$REPO/issues/$1/assignees" -f "assignees[]=$2" > /dev/null
}

# first N: print the account, of those assigned to N now, whose latest
# assignment is the earliest, or the one account assigned when the timeline
# does not show its assignment yet. Assignments are compared by their places
# in the timeline, which lists events in the order they happened; two in the
# same second have equal timestamps but distinct places.
first()
{
    now=$(gh issue view "$1" --repo "$REPO" --json assignees --jq '.assignees[].login') ||
        return 1
    events=$(gh api --paginate "repos/$REPO/issues/$1/timeline" \
        --jq '.[] | select(.event == "assigned") | .assignee.login') ||
        return 1
    printf '%s\n' "$events" | awk -v now="$(printf '%s\n' "$now" | tr '\n' ' ')" '
        BEGIN { n = split(now, a, " "); for (i = 1; i <= n; i++) cur[a[i]] = 1 }
        ($1 in cur) { last[$1] = NR }
        END {
            for (l in last)
                if (who == "" || last[l] < last[who])
                    who = l
            if (who == "" && n == 1)
                who = a[1]
            print who
        }'
}

# refuse N WHY: say WHY on stderr and exit 1, first unassigning this account
# from N when this take assigned it.
refuse()
{
    [ "$added" != yes ] || unassign "$1" "$me" ||
        echo "board: #$1: could not unassign $me; unassign it by hand" >&2
    echo "board: #$1: $2" >&2
    exit 1
}

# kind N: print what keeps N from a worker on the board read into `board`,
# `me` being this account; print nothing when nothing does.
kind()
{
    printf '%s\n' "$board" | awk -F '\t' -v n="$1" -v me="$me" -v owner="$OWNER" "$LIB"'
        $3 != n { next }
        { found = 1 }
        $8 != "" { print "it is a subtask of #" $8 "; take its task"; exit }
        $10 != "OPEN" { print "it is in no open milestone"; exit }
        $11 == "" { print "its milestone names no branch"; exit }
        $7 > 0 { print "it has an open blocker"; exit }
        has($5, "owner") { print "it waits for the owner"; exit }
        has($5, "owner-only") && me != owner {
            print "only the owner'"'"'s accounts can do its work"; exit }
        $2 != "Ready" && $2 != "In progress" {
            print "its Status is " ($2 == "" ? "empty" : $2) ", not Ready"; exit }
        END { if (!found) print "it is not on the board" }'
}

take()
{
    if finished "$1"; then
        exit 1
    fi
    added=no
    login
    board=$(items) || refuse "$1" "cannot read the board"
    reason=$(kind "$1")
    [ -z "$reason" ] || refuse "$1" "$reason"
    had=$(gh issue view "$1" --repo "$REPO" --json assignees \
        --jq 'if any(.assignees[]; .login == "'"$me"'") then "yes" else "no" end') ||
        refuse "$1" "cannot read its assignees"
    assign "$1" "$me" || refuse "$1" "cannot assign it to $me"
    [ "$had" = yes ] || added=yes
    who=$(first "$1") || refuse "$1" "cannot read who was assigned first"
    [ "$who" = "$me" ] || refuse "$1" "taken by $who first"
    set_status "$1" "In progress" || refuse "$1" "cannot set it In progress"
}

mine()
{
    me=$(gh api user --jq .login)
    who=$(gh issue view "$1" --repo "$REPO" --json state,assignees \
        --jq 'if .state == "OPEN" then .assignees[].login else empty end')
    if printf '%s\n' "$who" | grep -qx "$me"; then echo yes; else echo no; fi
}

release()
{
    if finished "$1"; then
        return 0
    fi
    me=$(gh api user --jq .login)
    status=Ready label=
    case $2 in
        owner) status=Backlog label=owner ;;
        owner-only) label=owner-only ;;
    esac
    if ! set_status "$1" "$status"; then
        echo "board: #$1: cannot set it $status; it is still assigned to $me" >&2
        exit 1
    fi
    if [ -n "$label" ] && ! gh issue edit "$1" --repo "$REPO" --add-label "$label" > /dev/null; then
        echo "board: #$1: set $status, but cannot label it $label; release it again" >&2
        exit 1
    fi
    if ! unassign "$1" "$me"; then
        echo "board: #$1: set $status, but cannot unassign $me; release it again" >&2
        exit 1
    fi
}

branch()
{
    name=$(gh api "repos/$REPO/issues/$1" --jq "$BRANCH") || {
        echo "board: #$1: cannot read its milestone" >&2
        exit 1
    }
    if [ -z "$name" ]; then
        echo "board: #$1: it has no milestone, or its milestone names no branch" >&2
        exit 1
    fi
    echo "$name"
}

ready()
{
    for n; do
        set_status "$n" Ready
        printf '%s ' "$n"
    done
    echo
}

done_()
{
    read_board all
    closed=$(printf '%s\n' "$board" | awk -F '\t' '
        $4 == "CLOSED" && $2 != "Done" { print $3 }')
    for n in $closed; do
        set_status "$n" Done
        printf '%s ' "$n"
    done
    echo
}

archive()
{
    read_board all
    cards=$(printf '%s\n' "$board" | awk -F '\t' -v m="$1" '
        { id[NR] = $1; n[NR] = $3; closed[NR] = $4 == "CLOSED"; parent[NR] = $8
          if ($9 == m) task[$3] = 1 }
        END {
            for (i = 1; i <= NR; i++)
                if (closed[i] && (n[i] in task || parent[i] in task))
                    print id[i] " " n[i]
        }')
    status=0
    printf '%s\n' "$cards" | {
        while read -r i n; do
            [ -n "$i" ] || continue
            if gh project item-archive "$BOARD" --owner "$OWNER" --id "$i" > /dev/null; then
                printf '%s ' "$n"
            else
                echo "board: #$n: cannot archive its card" >&2
                status=1
            fi
        done
        echo
        exit "$status"
    }
}

milestones()
{
    open=$(gh api --paginate "repos/$REPO/milestones?state=open&per_page=100" \
        --jq '.[] | [(.number | tostring), ({milestone: .} | '"$BRANCH"'),
            (.title | gsub("\t"; " "))] | join("\t")') || {
        echo "board: cannot read the milestones" >&2
        exit 1
    }
    read_board all
    {
        printf '%s\n' "$open" | sort -n | awk '{ print "M\t" $0 }'
        printf '%s\n' "$board"
    } | awk -F '\t' '
        $1 == "M" { if (NF >= 4) { m[++ms] = $2; branch[$2] = $3; title[$2] = $4 }; next }
        NF < 12 || $8 != "" || !($9 in title) { next }
        $4 == "OPEN" { open[$9]++ }
        $4 == "CLOSED" { closed[$9]++ }
        $4 == "OPEN" && $2 == "In progress" {
            busy[$9] = busy[$9] "  #" $3 " In progress: " ($6 == "" ? "nobody" : $6) ": " $12 "\n" }
        END {
            for (i = 1; i <= ms; i++) {
                k = m[i]
                printf "milestone %s: %s\n  branch: %s\n  tasks: %d open, %d closed\n%s",
                    k, title[k], (branch[k] == "" ? "none" : branch[k]),
                    open[k], closed[k], busy[k]
            }
        }'
}

case "$1" in
    next) next ;;
    take) [ -n "$2" ] || exit 2; board_ids=$(ids); take "$2" ;;
    mine) [ -n "$2" ] || exit 2; mine "$2" ;;
    release)
        [ -n "$2" ] || exit 2
        case $3 in ''|owner|owner-only) ;; *) exit 2 ;; esac
        board_ids=$(ids); release "$2" "$3" ;;
    branch) [ -n "$2" ] || exit 2; branch "$2" ;;
    ready) [ -n "$2" ] || exit 2; board_ids=$(ids); shift; ready "$@" ;;
    done) board_ids=$(ids); done_ ;;
    archive) [ -n "$2" ] || exit 2; archive "$2" ;;
    milestones) milestones ;;
    *) sed -n 's/^# Usage: /usage: /p; s/^#        /       /p' "$0" >&2; exit 2 ;;
esac
