#!/bin/sh
#
# board.sh: the operations the worker loops, the manager and the release make
# on the board and on the milestones.
#
# Usage: board.sh next
#        board.sh take N
#        board.sh mine N
#        board.sh release N [owner|owner-only|trial]
#        board.sh branch N
#        board.sh milestone M
#        board.sh ready N...
#        board.sh done
#        board.sh finish N COMMENT
#        board.sh card M PARENT TITLE
#        board.sh step N progress|pass|fail TEXT
#        board.sh check [HELD...]
#        board.sh archive [M]
#        board.sh milestones
#        board.sh watch [FILE]
#        board.sh move N [AFTER]
#
# A milestone is a GitHub milestone, and its description names its branch on
# its first line that begins `Branch:`, as `Branch: NAME`; backquotes and
# blanks around NAME are not part of it, and a NAME with a character other
# than a letter, a digit, `.`, `_`, `/` or `-` is no branch, so that NAME can
# go into a command line as it is. A task is an issue in a milestone with no
# parent issue, and a subtask is a sub-issue of a task. Workers take tasks; a
# subtask is closed by the worker of its task and is never taken by itself.
# An issue labelled `owner-only` has work that only the owner's accounts can
# do, such as a step on the Lenovo, which only the owner's computer reaches;
# `next` and `take` offer it only to the account OWNER below. An issue
# labelled `linux-host` builds the GNU layer, which builds on an x86-64 Linux
# host only (vendor/gnu/Readme.md); they offer it only to an account on Linux
# or one whose loop names a copy on the testing host in RACH2_TESTING_COPY.
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
# labelled `owner`, labelled `owner-only` only when this account is OWNER,
# and labelled `linux-host` only when this account can build the GNU layer.
# An issue labelled `owner-only` is a step on the Lenovo unless its comment
# says otherwise, and one task works the Lenovo at a time, so `next` leaves
# it out while the Lenovo's lock, `build/flow/lenovo` of the main working
# tree as the machine skill says, is held or cannot be read, and while
# another issue labelled `owner-only` is In progress, whose loop takes the
# lock once its session starts; a loop of the owner's then takes other work
# instead of waiting for the machine. The tasks are in the board's order,
# whatever their milestones, so that the tasks of every open milestone are
# one queue whose order the manager sets with `move`.
# `gh project item-list` does not return that order, so the query asks for
# the items by position.
#
# `take N` refuses a closed issue, setting it Done and changing nothing else,
# since a close between `next` and `take` would otherwise leave it In
# progress. Before it assigns anything, it also refuses an issue that is not
# on the board, a subtask, an issue in no open milestone or in one that names
# no branch, one with an open blocker, one labelled `owner`, one labelled
# `owner-only` when this account is not OWNER, one labelled `linux-host` when
# this account cannot build the GNU layer, and one whose Status is
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
# the owner's accounts can do, and removes `linux-host`, since the step left
# runs on the Mac. `release N trial` does the same but sets Backlog, for a
# step that boots code never run on the Lenovo, which waits there for the
# manager's go and `ready N`. When RACH2_WORKER_STATE names the state
# directory of the loop whose session releases N, and its `issue` file
# holds N, `release` empties that file before it unassigns this account, so
# that a loop of the same account that takes N at once is its only writer.
# A step that fails exits 1 with N still
# assigned to this account, so that its worker keeps it and can release it
# again. A closed issue is set Done and nothing else is changed, since its
# commit may have closed it before its worker hands it back.
#
# `branch N` prints the branch that the description of N's milestone names,
# and fails when N has no milestone or the milestone names no branch.
#
# `milestone M` prints the branch that the description of milestone M names,
# for release.sh. It exits 1 when M cannot be read, and 3 when M is closed or
# names no branch.
#
# `ready N...` sets each N Ready.
#
# `done` sets Done on every closed issue whose Status is not, assigns every
# closed issue that has no assignee to the account that closed it, and fails
# when the listing of the board fails.
#
# `finish N COMMENT` is how a worker closes a subtask or a task: it assigns N
# to this account, closes N with COMMENT and sets it Done. It refuses, naming
# them, while a subtask of N is open, since the manager may add one to a task
# that is being worked.
#
# `move N [AFTER]` puts N's card right after AFTER's on the board, or first
# when AFTER is not given; the board's order is the order in which `next`
# offers tasks, and the manager sets it with this.
#
# `card M PARENT TITLE` files an issue TITLE in milestone M, a sub-issue of
# PARENT unless PARENT is `-`, assigned to OWNER and put on the board in
# Backlog, and prints its number; release.sh files the release's cards so.
#
# `step N progress|pass|fail TEXT` reports a release step on its card N:
# `progress` sets it In progress, `pass` closes it with TEXT and sets it Done,
# and `fail` comments TEXT and leaves it In progress; `progress` and `fail`
# reopen a closed card first, for a step that runs again.
#
# `check [HELD...]` prints a line `board: LINE` for each inconsistency of
# the board it can see: an open task that no loop in HELD holds whose branch
# `task/N` its milestone's branch already contains, an open card In progress
# with no assignee, or one that no worker loop holds when HELD lists the
# issues the loops hold, a card Done or closed whose state or Status
# disagrees, and an open milestone whose cards are all closed while no other
# open milestone with an open card names its branch, since the milestones of
# a stack wait on one branch for the stack's release. status.sh runs it on
# every check.
#
# `archive [M]` archives the cards of the closed issues of milestone M and of
# their closed subtasks, and prints the numbers it archived; release.sh runs
# it once it has closed M. With no M it archives those of every closed
# milestone and of no milestone; status.sh and stop.sh run it so on every
# look, so that a card closed after its own release leaves the board at once.
# An open issue keeps its card. A card that
# cannot be archived is reported on stderr and the rest go on, and the exit
# status is then 1.
#
# `milestones` prints each open milestone, lowest number first, as a line
# `milestone M: TITLE`, the line `  branch: NAME`, or `  branch: none`, the
# line `  tasks: O open, C closed`, and a line `  #N In progress: LOGINS:
# TITLE` for each task In progress, LOGINS being its assignees or `nobody`.
# The closed tasks it counts are those on the board, which keeps them until
# the release archives them, so that it reads no closed issue that the board
# does not hold.
#
# `watch [FILE]` runs until it is stopped and prints a line for each event on
# an issue the board holds, one in an open milestone or labelled `owner`,
# `owner-only` or `bug`, as it reads them: `#N: comment: LOGIN: LINE` for a
# new comment, LINE being its first line that is not blank, and `#N: EVENT:
# LOGIN: TITLE` for EVENT `closed`, `reopened`, `labeled owner`, `unlabeled
# owner`, the same two for `owner-only`, and `handed back`, an account's
# removal of itself from an open issue whose Status it set since it was
# assigned, as `release` does; a `take` that loses its race sets none.
# LOGIN is the account that made the event. It reads the repository's issue
# events and comments every BOARD_POLL seconds, thirty unless it is set and
# never fewer, and starts from the newest of each, so that it prints nothing
# that came before it. FILE, when given, keeps the ids of the last event and
# comment read, so that a watch started again with it goes on where the last
# one stopped; its directory is made when it is missing. A read that fails
# is said on stderr and made again at the next poll, which reads from the
# same place.
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

# host is 1 when this account can build the GNU layer: on Linux, or with a
# copy on the testing host.
case $(uname -s) in
Linux) host=1 ;;
*) host=${RACH2_TESTING_COPY:+1} ;;
esac

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

# set_status N NAME: set issue N's Status to the option NAME, putting the
# issue on the board first if `gh issue create` left it off.
set_status()
{
    it=$(item "$1") || it=
    if [ -z "$it" ]; then
        gh project item-add "$BOARD" --owner "$OWNER" \
            --url "https://github.com/$REPO/issues/$1" > /dev/null && { it=$(item "$1") || it=; }
    fi
    if [ -z "$it" ]; then
        echo "board: #$1: it is not on the board" >&2
        return 1
    fi
    gh project item-edit --project-id "$(id_of project)" \
        --field-id "$(id_of field)" --id "$it" \
        --single-select-option-id "$(id_of "$(echo "$2" | tr ' ' _)")" > /dev/null
}

# BRANCH is the jq filter that prints the branch that the description of the
# milestone `.milestone` names, as the header says, or nothing.
BRANCH='([.milestone.description // "" | split("\n")[] |
        select(startswith("Branch:"))] | first // "" | ltrimstr("Branch:") |
        gsub("^[\\s`]+|[\\s`]+$"; "") |
        if test("^[A-Za-z0-9._/-]+$") then . else "" end)'

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

# lenovo_held: succeed when the Lenovo's lock is held, as a directory
# `build/flow/lenovo` in the main working tree of this clone, or when that
# tree cannot be found, so that a doubt offers no step on the Lenovo.
lenovo_held()
{
    held_common=$(git rev-parse --path-format=absolute --git-common-dir 2> /dev/null) ||
        return 0
    [ -d "$held_common/../build/flow/lenovo" ]
}

next()
{
    login
    read_board
    busy=0
    if [ "$me" = "$OWNER" ]; then
        if lenovo_held || printf '%s\n' "$board" | awk -F '\t' "$LIB"'
                $4 == "OPEN" && $2 == "In progress" && has($5, "owner-only") { f = 1 }
                END { exit !f }'; then
            busy=1
        fi
    fi
    printf '%s\n' "$board" | awk -F '\t' -v me="$me" -v owner="$OWNER" -v host="$host" -v busy="$busy" "$LIB"'
        $4 != "OPEN" || $2 != "Ready" || $6 != "" || $7 > 0 || $8 != "" { next }
        $10 != "OPEN" || $11 == "" || has($5, "owner") { next }
        has($5, "owner-only") && (me != owner || busy == 1) { next }
        has($5, "linux-host") && host != 1 { next }
        { print $3 "\t" $12 }'
}

# move N [AFTER]: put N's card right after AFTER's, or first.
move()
{
    [ "$1" != "$2" ] || { echo "board: #$1: a card cannot follow itself" >&2; exit 2; }
    move_item=$(item "$1") || { echo "board: #$1: cannot read its card" >&2; exit 1; }
    [ -n "$move_item" ] || { echo "board: #$1: it is not on the board" >&2; exit 1; }
    move_after=
    if [ -n "$2" ]; then
        move_after=$(item "$2") || { echo "board: #$2: cannot read its card" >&2; exit 1; }
        [ -n "$move_after" ] || { echo "board: #$2: it is not on the board" >&2; exit 1; }
    fi
    gh api graphql -f p="$(id_of project)" -f i="$move_item" \
        ${move_after:+-f a="$move_after"} -f query='
        mutation($p: ID!, $i: ID!, $a: ID) {
            updateProjectV2ItemPosition(input: {projectId: $p, itemId: $i,
                afterId: $a}) { clientMutationId } }' > /dev/null || {
        echo "board: #$1: cannot move its card" >&2
        exit 1
    }
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
    printf '%s\n' "$board" | awk -F '\t' -v n="$1" -v me="$me" -v owner="$OWNER" -v host="$host" "$LIB"'
        $3 != n { next }
        { found = 1 }
        $8 != "" { print "it is a subtask of #" $8 "; take its task"; exit }
        $10 != "OPEN" { print "it is in no open milestone"; exit }
        $11 == "" { print "its milestone names no branch"; exit }
        $7 > 0 { print "it has an open blocker"; exit }
        has($5, "owner") { print "it waits for the owner"; exit }
        has($5, "owner-only") && me != owner {
            print "only the owner'"'"'s accounts can do its work"; exit }
        has($5, "linux-host") && host != 1 {
            print "it builds the GNU layer, which needs a Linux host"; exit }
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
    me=$(gh api user --jq .login) && [ -n "$me" ] || exit 1
    who=$(gh issue view "$1" --repo "$REPO" --json state,assignees \
        --jq 'if .state == "OPEN" then .assignees[].login else empty end') || exit 1
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
        trial) status=Backlog label=owner-only ;;
    esac
    if ! set_status "$1" "$status"; then
        echo "board: #$1: cannot set it $status; it is still assigned to $me" >&2
        exit 1
    fi
    if [ -n "$label" ] && ! gh issue edit "$1" --repo "$REPO" --add-label "$label" > /dev/null; then
        echo "board: #$1: set $status, but cannot label it $label; release it again" >&2
        exit 1
    fi
    if [ "$label" = owner-only ] && ! gh issue edit "$1" --repo "$REPO" --remove-label linux-host > /dev/null 2>&1 &&
        gh issue view "$1" --repo "$REPO" --json labels --jq '.labels[].name' | grep -qx linux-host; then
        echo "board: #$1: set $status, but cannot remove linux-host; release it again" >&2
        exit 1
    fi
    claimed=
    if [ -n "$RACH2_WORKER_STATE" ] && [ "$(cat "$RACH2_WORKER_STATE/issue" 2> /dev/null)" = "$1" ]; then
        claimed=$RACH2_WORKER_STATE/issue
        : > "$claimed"
    fi
    if ! unassign "$1" "$me"; then
        [ -z "$claimed" ] || echo "$1" > "$claimed"
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

milestone()
{
    name=$(gh api "repos/$REPO/milestones/$1" \
        --jq 'if .state == "open" then {milestone: .} | '"$BRANCH"' else "" end') || {
        echo "board: milestone $1: cannot read it" >&2
        exit 1
    }
    if [ -z "$name" ]; then
        echo "board: milestone $1: it is closed or names no branch" >&2
        exit 3
    fi
    echo "$name"
}

ready()
{
    rc=0
    for n; do
        if set_status "$n" Ready; then
            printf '%s ' "$n"
        else
            echo "board: #$n: cannot set it Ready" >&2
            rc=1
        fi
    done
    echo
    return $rc
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
    alone=$(printf '%s\n' "$board" | awk -F '\t' '$4 == "CLOSED" && $6 == "" { print $3 }')
    for n in $alone; do
        who=$(gh api "repos/$REPO/issues/$n" --jq '.closed_by.login // ""') || continue
        [ -z "$who" ] || assign "$n" "$who" || echo "board: #$n: cannot assign it to $who" >&2
    done
    echo
}

finish()
{
    login
    read_board
    open=$(printf '%s\n' "$board" | awk -F '\t' -v p="$1" '
        $8 == p && $4 == "OPEN" { printf "%s#%s", sep, $3; sep = " " }')
    if [ -n "$open" ]; then
        echo "board: #$1: subtasks $open are open; finish them first" >&2
        exit 1
    fi
    if ! assign "$1" "$me" ||
        ! gh issue close "$1" --repo "$REPO" --comment "$2" > /dev/null ||
        ! set_status "$1" Done; then
        echo "board: #$1: cannot finish it" >&2
        exit 1
    fi
}

card()
{
    n=$(gh api "repos/$REPO/issues" -f title="$3" -F milestone="$1" -f "assignees[]=$OWNER" \
        -f body="A step of the release of milestone $1, kept by release.sh." --jq .number) || {
        echo "board: cannot file \"$3\"" >&2
        exit 1
    }
    if [ "$2" != - ]; then
        id=$(gh api "repos/$REPO/issues/$n" --jq .id) &&
            gh api -X POST "repos/$REPO/issues/$2/sub_issues" -F sub_issue_id="$id" > /dev/null ||
            echo "board: #$n: cannot put it under #$2" >&2
    fi
    url=$(gh api "repos/$REPO/issues/$n" --jq .html_url) &&
        gh project item-add "$BOARD" --owner "$OWNER" --url "$url" > /dev/null &&
        set_status "$n" Backlog || echo "board: #$n: cannot put it on the board" >&2
    echo "$n"
}

# reopen N: reopen issue N if it is closed.
reopen()
{
    [ "$(gh issue view "$1" --repo "$REPO" --json state --jq .state)" != CLOSED ] ||
        gh issue reopen "$1" --repo "$REPO" > /dev/null
}

step()
{
    case $2 in
        progress)
            reopen "$1" && set_status "$1" "In progress" ;;
        pass)
            gh issue close "$1" --repo "$REPO" --comment "$3" > /dev/null &&
                set_status "$1" Done ;;
        fail)
            reopen "$1" && gh issue comment "$1" --repo "$REPO" --body "$3" > /dev/null &&
                set_status "$1" "In progress" ;;
        *) exit 2 ;;
    esac || {
        echo "board: #$1: cannot report the step" >&2
        exit 1
    }
}

check()
{
    read_board all
    printf '%s\n' "$board" | awk -F '\t' -v held=" $* " '
        NF < 12 { next }
        $4 == "OPEN" && $2 == "Done" { print "board: #" $3 " is open but Done" }
        $4 == "CLOSED" && $2 != "Done" { print "board: #" $3 " is closed but " ($2 == "" ? "has no Status" : $2) }
        $4 == "OPEN" && $2 == "In progress" && $6 == "" { print "board: #" $3 " is In progress with no assignee" }
        $4 == "OPEN" && $2 == "In progress" && held != "  " && $8 == "" && index(held, " " $3 " ") == 0 &&
            $12 !~ /^Release milestone / { print "board: #" $3 " is In progress but no loop holds it" }
        $8 == "" && $9 != "" && $10 == "OPEN" {
            cards[$9]++; branch[$9] = $11
            if ($4 == "OPEN") { open[$9]++; shared[$11]++ }
        }
        END {
            for (m in cards)
                if (!open[m] && !shared[branch[m]])
                    print "board: milestone " m " is open but all its tasks are closed"
        }'
    printf '%s\n' "$board" | awk -F '\t' '$4 == "OPEN" && $8 == "" && $11 != "" { print $3 " " $11 }' |
        while read -r n b; do
            # A loop that holds the task may have pushed part of it already.
            case " $* " in *" $n "*) continue ;; esac
            s=$(gh api "repos/$REPO/compare/$b...task/$n" --jq .status 2> /dev/null) || continue
            case $s in identical|behind) echo "board: #$n is open but $b contains task/$n" ;; esac
        done
}

archive()
{
    read_board all
    cards=$(printf '%s\n' "$board" | awk -F '\t' -v m="$1" '
        { id[NR] = $1; n[NR] = $3; closed[NR] = $4 == "CLOSED"; parent[NR] = $8
          if (m != "" ? $9 == m : $9 == "" || $10 == "CLOSED") task[$3] = 1 }
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
    read_board
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

# US is the ASCII unit separator, which parts the fields of `news`; a tab
# would not do, since `read` joins tabs around an empty field.
US=$(printf '\037')

# BOARDED is the jq filter that is true when the issue `.` is on the board: not
# a pull request, and in an open milestone or labelled `owner`, `owner-only`
# or `bug`.
BOARDED='(.pull_request == null and ((.milestone.state // "") == "open" or
    any(.labels[]; .name == "owner" or .name == "owner-only" or .name == "bug")))'

# newest: print the ids of the repository's newest issue event and newest
# issue comment, or fail.
newest()
{
    newest_e=$(gh api "repos/$REPO/issues/events?per_page=1" --jq '.[0].id // 0') &&
        newest_c=$(gh api "repos/$REPO/issues/comments?sort=created&direction=desc&per_page=1" \
            --jq '.[0].id // 0') &&
        [ -n "$newest_e" ] && [ -n "$newest_c" ] &&
        echo "$newest_e $newest_c"
}

# news E C: print, oldest first, each issue event with an id above E and each
# issue comment with an id above C, as fields parted by US: the time in
# seconds, `e` or `c`, the id, the event, the actor, the issue's number, a
# flag that is 1 when the issue is on the board (always 1 for a comment, which
# carries no issue), the issue's state, the label or the assignee the event
# names, and the issue's title or the comment's first line. Both lists come
# newest first, so pages are read until one holds an id at or below the mark,
# ten at most. Fail when a page cannot be read.
news()
{
    news_e=$1 news_c=$2 news_p=1 news_out=
    while [ "$news_p" -le 10 ]; do
        news_got=$(gh api "repos/$REPO/issues/events?per_page=100&page=$news_p" --jq '
            .[] | select(.id > '"$news_e"') |
            [(.created_at | fromdateiso8601 | tostring), "e", (.id | tostring),
             .event, (.actor.login // "ghost"), (.issue.number | tostring),
             (if (.issue | '"$BOARDED"') or
                 ((.label.name // "") | test("^owner(-only)?$")) then "1" else "0" end), .issue.state,
             (.label.name // .assignee.login // ""), .issue.title] |
            map(gsub("[\u001f\n\r]"; " ")) | join("\u001f")') || return 1
        news_out=$news_out${news_got:+$news_got
}
        [ "$(printf '%s\n' "$news_got" | grep -c .)" -eq 100 ] || break
        news_p=$((news_p + 1))
    done
    news_p=1
    while [ "$news_p" -le 10 ]; do
        news_got=$(gh api "repos/$REPO/issues/comments?sort=created&direction=desc&per_page=100&page=$news_p" --jq '
            .[] | select(.id > '"$news_c"') |
            [(.created_at | fromdateiso8601 | tostring), "c", (.id | tostring),
             "comment", (.user.login // "ghost"),
             (.issue_url | split("/") | last), "1", "", "",
             ((.body // "") | gsub("\r"; "") | split("\n") | map(select(test("\\S"))) |
                 first // "")] |
            map(gsub("[\u001f\n\r]"; " ")) | join("\u001f")') || return 1
        news_out=$news_out${news_got:+$news_got
}
        [ "$(printf '%s\n' "$news_got" | grep -c .)" -eq 100 ] || break
        news_p=$((news_p + 1))
    done
    printf '%s' "$news_out" | sort -t "$US" -k 1,1n -k 3,3n
}

# handed N LOGIN ID: succeed when LOGIN, between its latest assignment to N
# before the event ID and that event, set N's Status, as `release` does
# before it unassigns and `take` does once it has won N. A `take` that loses
# the race for N unassigns its account without setting a Status. A timeline
# that cannot be read counts as a hand-back, so that none is missed.
handed()
{
    handed_ev=$(gh api --paginate "repos/$REPO/issues/$1/events?per_page=100" --jq '
        .[] | select(.actor.login == "'"$2"'" and .id < '"$3"') |
        if .event == "assigned" and .assignee.login == "'"$2"'" then "a \(.id)"
        elif .event == "project_v2_item_status_changed" then "s \(.id)"
        else empty end') || return 0
    printf '%s\n' "$handed_ev" | awk '
        $1 == "a" && $2 > a { a = $2 }
        $1 == "s" && $2 > s { s = $2 }
        END { exit !(s > a) }'
}

# say N EVENT LOGIN TEXT: print the line `#N: EVENT: LOGIN: TEXT`.
say()
{
    printf '#%s: %s: %s: %s\n' "$1" "$2" "$3" "$4"
}

watch()
{
    poll=${BOARD_POLL:-30}
    [ "$poll" -ge 30 ] 2> /dev/null || poll=30
    marks=
    [ -z "$1" ] || mkdir -p "$(dirname "$1")"
    [ -z "$1" ] || [ ! -s "$1" ] || marks=$(cat "$1")
    until [ -n "$marks" ] || marks=$(newest); do
        echo "board: watch: cannot read the newest events; trying again" >&2
        sleep "$poll"
    done
    [ -z "$1" ] || echo "$marks" > "$1"
    while :; do
        sleep "$poll"
        # shellcheck disable=SC2086 # marks is two words, the newest ids
        set -- "$1" $marks
        if ! got=$(news "$2" "$3"); then
            echo "board: watch: cannot read the events; trying again" >&2
            continue
        fi
        e=$2 c=$3
        while IFS=$US read -r _ kind id event who n boarded state what text; do
            [ -n "$id" ] || continue
            case $kind in e) e=$id ;; c) c=$id ;; esac
            [ "$kind" = e ] && [ "$boarded" != 1 ] && continue
            [ "$kind" = c ] && { gh api "repos/$REPO/issues/$n" --jq "$BOARDED" 2> /dev/null |
                grep -qx false; } && continue
            case $event in
                comment|closed|reopened) say "$n" "$event" "$who" "$text" ;;
                labeled|unlabeled)
                    case $what in owner|owner-only) say "$n" "$event $what" "$who" "$text" ;; esac ;;
                unassigned)
                    [ "$who" = "$what" ] && [ "$state" = open ] && handed "$n" "$who" "$id" &&
                        say "$n" "handed back" "$who" "$text" ;;
            esac
        done <<END
$got
END
        marks="$e $c"
        [ -z "$1" ] || echo "$marks" > "$1"
    done
}

case "$1" in
    next) next ;;
    take) [ -n "$2" ] || exit 2; board_ids=$(ids); take "$2" ;;
    mine) [ -n "$2" ] || exit 2; mine "$2" ;;
    release)
        [ -n "$2" ] || exit 2
        case $3 in ''|owner|owner-only|trial) ;; *) exit 2 ;; esac
        board_ids=$(ids); release "$2" "$3" ;;
    branch) [ -n "$2" ] || exit 2; branch "$2" ;;
    milestone) [ -n "$2" ] || exit 2; milestone "$2" ;;
    ready) [ -n "$2" ] || exit 2; board_ids=$(ids); shift; ready "$@" ;;
    done) board_ids=$(ids); done_ ;;
    finish) [ -n "$3" ] || exit 2; board_ids=$(ids); finish "$2" "$3" ;;
    card) [ -n "$4" ] || exit 2; board_ids=$(ids); card "$2" "$3" "$4" ;;
    step) [ -n "$3" ] || exit 2; board_ids=$(ids); step "$2" "$3" "$4" ;;
    check) shift; check "$@" ;;
    archive) archive "$2" ;;
    milestones) milestones ;;
    watch) watch "$2" ;;
    move)
        case $2$3 in ''|*[!0-9]*) exit 2 ;; esac
        [ -z "$4" ] || exit 2
        board_ids=$(ids); move "$2" "$3" ;;
    *) sed -n 's/^# Usage: /usage: /p; s/^#        /       /p' "$0" >&2; exit 2 ;;
esac
