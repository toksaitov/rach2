#!/bin/sh
#
# board.sh: the operations the worker and manager loops make on the board.
#
# Usage: board.sh next
#        board.sh take N [GROUP]
#        board.sh mine N
#        board.sh release N [STATUS|owner]
#        board.sh ready AFTER N...
#        board.sh order N...
#        board.sh group TITLE N...
#        board.sh done
#
# `next` prints the issues a worker may take, in the order AGENTS.md gives:
# the open issues whose titles begin `ci:`, then the Ready issues in the
# board's order, each open, unassigned, with no open blocker and without the
# label `grouped`, one line each with the number, a tab and the title.
# `gh project item-list` does not return the board's order, so the query asks
# for the items by position. When any page of the answer is lost, `next`
# prints nothing and fails, so that a caller does not take a part of the board
# for the whole.
#
# `take N [GROUP]` refuses a closed issue, setting it Done and changing
# nothing else, since a close between `next` and `take` would otherwise leave
# it In progress. Otherwise it assigns N to this account and reads the
# issue's timeline.
# Of the accounts assigned now, the one whose latest assignment is the
# earliest keeps the issue. When that is another account, `take` unassigns
# this one and exits 1, so that no issue has two writers; otherwise it sets N
# In progress. A step that fails after the assignment unassigns this account
# again, unless N was this account's before, so that a failed take leaves the
# issue as it found it. Without GROUP, `take` refuses a member of a group,
# before the assignment and again after it, since `group` may label N while
# a loop holds a listing of `next` that has N in it. With GROUP, it takes N
# only as a member of that group: N has the label `grouped` and is listed in
# GROUP's body, and GROUP is open and assigned to this account. A member that
# is this account's already is taken again, for a session that resumes the
# group; no other path assigns a member, so it is this group worker's.
#
# `mine N` prints `yes` while N is open and assigned to this account and `no`
# otherwise; it prints nothing and fails when GitHub cannot be reached, so
# that a caller does not take a lost connection for a closed issue.
#
# `release N [STATUS]` hands N back: it sets STATUS, Ready unless given, then
# unassigns this account and removes the label `grouped`, so that N returns to
# the queue as an issue of its own. `release N owner` sets Backlog and adds
# the label `owner`, which marks a decision that AGENTS.md gives the owner and
# that the manager sends to the owner's phone. A step that fails exits 1 with
# N still assigned to this account, so that its worker keeps it and can
# release it again. A closed issue is set Done and nothing else is changed,
# since its commit may have closed it before its worker hands it back.
#
# `ready AFTER N...` moves each N after AFTER on the board, the first N after
# AFTER and each later one after the one before, and then sets it Ready, so
# that no worker finds it Ready in its old place. `order N...` does the same
# with the first N moved to the top of the board, ahead of every issue it does
# not name.
#
# `group TITLE N...` makes a group, one issue that a worker takes in place of
# its members N..., at most eight of them, each open, unassigned, in no other
# group and not filed by ci. The group is an issue titled `group: TITLE` with
# the label `group`, whose body lists the members one a line as `* #N`. Each
# member gets the label `grouped`, which keeps it out of `next` and `take`,
# and the group is set Ready, for `ready` or `order` to place. It prints the
# group's number. `group` labels the members before it reads their assignees
# again, as `take` assigns before it reads the label, so that of a take and a
# grouping that overlap, at least one sees the other and fails; when a member
# was taken, `group` removes the labels and makes no group. The board adds
# each new issue itself, and `gh project item-add` fails when the board was
# first; `group` then reads the board and fails only when the group is not on
# it.
# A failure removes the labels attempted by this call and closes any group
# it created. If cleanup fails, stderr gives the steps left to do.
#
# `done` sets Done on every closed issue whose Status is not, and fails when
# the listing of the board fails. The manager runs it at each check, for an
# issue that closes while a `take` or a `release` of it runs.
#
# The repository and the board's owner and number are below. The ids of the
# board, its Status field and the field's options are looked up by name on
# each call, so another board needs only these three lines changed.

# shellcheck disable=SC2016 # the dollars are GraphQL's and jq's
set -e

REPO=toksaitov/rach2
OWNER=toksaitov
BOARD=6

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

# move ITEM [AFTER]: put the board's item ITEM after the item AFTER, or at the
# top of the board when AFTER is empty.
move()
{
    if [ -n "$2" ]; then
        gh api graphql -f p="$(id_of project)" -f i="$1" -f a="$2" -f query='
            mutation($p: ID!, $i: ID!, $a: ID!) {
                updateProjectV2ItemPosition(input: {projectId: $p, itemId: $i,
                    afterId: $a}) { clientMutationId } }' > /dev/null
    else
        gh api graphql -f p="$(id_of project)" -f i="$1" -f query='
            mutation($p: ID!, $i: ID!) {
                updateProjectV2ItemPosition(input: {projectId: $p,
                    itemId: $i}) { clientMutationId } }' > /dev/null
    fi
}

next()
{
    answer=$(gh api graphql --paginate -F n="$BOARD" -f o="$OWNER" -f query='
        query($o: String!, $n: Int!, $endCursor: String) {
            user(login: $o) { projectV2(number: $n) {
                items(first: 100, after: $endCursor,
                      orderBy: {field: POSITION, direction: ASC}) {
                    pageInfo { hasNextPage endCursor }
                    nodes {
                        fieldValueByName(name: "Status") {
                            ... on ProjectV2ItemFieldSingleSelectValue { name } }
                        content { ... on Issue { number title state
                            assignees(first: 1) { totalCount }
                            labels(first: 20) { nodes { name } }
                            issueDependenciesSummary { blockedBy } } } } } } } }' \
        --jq '.data.user.projectV2.items.nodes[] |
            select(.content.state == "OPEN" and
                   .content.assignees.totalCount == 0 and
                   ([.content.labels.nodes[] | select(.name == "grouped")]
                    | length) == 0 and
                   .content.issueDependenciesSummary.blockedBy == 0) |
            "\(.fieldValueByName.name // "-")\t\(.content.number)\t\(.content.title)"')
    printf '%s\n' "$answer" |
        awk -F '\t' '$3 ~ /^ci:/ { print $2 "\t" $3; next }
            $1 == "Ready" { r = r $2 "\t" $3 "\n" } END { printf "%s", r }'
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

# unassign N LOGIN: remove LOGIN from N's assignees, or say that it could not.
unassign()
{
    gh issue edit "$1" --repo "$REPO" --remove-assignee "$2" > /dev/null ||
        echo "board: #$1: could not unassign $2; unassign it by hand" >&2
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

# member N G: print why N cannot be taken as a member of group G, or nothing
# when it can.
member()
{
    gh issue view "$2" --repo "$REPO" --json state,labels,assignees,body --jq '
        if .state != "OPEN" then "#'"$2"' is closed"
        elif any(.labels[]; .name == "group") | not then "#'"$2"' is not a group"
        elif any(.assignees[]; .login == "'"$me"'") | not then
            "#'"$2"' is not this account'"'"'s"
        elif any(.body | split("\n")[]; . == "* #'"$1"'") | not then
            "#'"$2"' does not list it"
        else empty end'
}

# labelled N: print `yes` when N has the label `grouped` and `no` otherwise.
labelled()
{
    gh issue view "$1" --repo "$REPO" --json labels \
        --jq 'if any(.labels[]; .name == "grouped") then "yes" else "no" end'
}

# refuse N WHY: say WHY on stderr and exit 1, first unassigning this account
# from N when this take assigned it.
refuse()
{
    [ "$added" != yes ] || unassign "$1" "$me"
    echo "board: #$1: $2" >&2
    exit 1
}

take()
{
    if finished "$1"; then
        exit 1
    fi
    me=$(gh api user --jq .login) added=no
    had=$(gh issue view "$1" --repo "$REPO" --json assignees \
        --jq 'if any(.assignees[]; .login == "'"$me"'") then "yes" else "no" end')
    grouped=$(labelled "$1")
    if [ -z "$2" ] && [ "$grouped" = yes ]; then
        refuse "$1" "it is a member of a group; take it with board.sh take $1 GROUP"
    fi
    if [ -n "$2" ]; then
        [ "$grouped" = yes ] || refuse "$1" "it is in no group"
        reason=$(member "$1" "$2")
        [ -z "$reason" ] || refuse "$1" "$reason"
    fi
    gh issue edit "$1" --repo "$REPO" --add-assignee "$me" > /dev/null
    [ "$had" = yes ] || added=yes
    who=$(first "$1") || refuse "$1" "cannot read who was assigned first"
    [ "$who" = "$me" ] || refuse "$1" "taken by $who first"
    if [ -z "$2" ]; then
        grouped=$(labelled "$1") || refuse "$1" "cannot read its labels"
        [ "$grouped" = no ] || refuse "$1" "it joined a group while it was taken"
    fi
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
    status=$2 label=
    if [ "$status" = owner ]; then
        status=Backlog label=owner
    fi
    if ! set_status "$1" "$status"; then
        echo "board: #$1: cannot set it $status; it is still assigned to $me" >&2
        exit 1
    fi
    if ! gh issue edit "$1" --repo "$REPO" --remove-assignee "$me" \
        --remove-label grouped ${label:+--add-label "$label"} > /dev/null; then
        echo "board: #$1: set $status, but cannot unassign $me; release it again" >&2
        exit 1
    fi
}

ready()
{
    prev=$(item "$1")
    shift
    for n; do
        i=$(item "$n")
        move "$i" "$prev"
        set_status "$n" Ready
        prev=$i
        printf '%s ' "$n"
    done
    echo
}

order()
{
    prev=
    for n; do
        i=$(item "$n")
        move "$i" "$prev"
        set_status "$n" Ready
        prev=$i
        printf '%s ' "$n"
    done
    echo
}

# why N: print why N cannot join a group, or nothing when it can. An issue
# labelled `grouped` is in another group, unless `grouping` is set, when the
# label is this grouping's own.
why()
{
    gh issue view "$1" --repo "$REPO" --json state,assignees,labels,title --jq '
        if .state != "OPEN" then "closed"
        elif (.assignees | length) > 0 then "assigned"
        elif "'"$grouping"'" == "" and any(.labels[]; .name == "grouped") then
            "in a group"
        elif (.title | startswith("ci:")) then "filed by ci"
        else empty end'
}

# group_rollback: undo an incomplete group, keeping its issue open when a
# member's label could not be removed, and report any cleanup left to do.
group_rollback()
{
    [ "$group_done" = no ] || return 0
    group_ref=${group_url:-group: $title}
    clean=yes
    for n in $group_members; do
        if ! gh issue edit "$n" --repo "$REPO" --remove-label grouped > /dev/null; then
            echo "board: $group_ref: could not remove grouped from #$n; remove it by hand" >&2
            clean=no
        fi
    done
    [ -n "$group_url" ] || return 0
    if [ "$clean" = yes ]; then
        gh issue close "$group_url" --repo "$REPO" --reason 'not planned' > /dev/null ||
            echo "board: $group_url: could not close the incomplete group; close it by hand" >&2
    else
        echo "board: $group_url: incomplete group kept open; remove its members' grouped labels and close it" >&2
    fi
}

group()
(
    title=$1
    shift
    if [ $# -gt 8 ]; then
        echo "board: a group has at most 8 members, not $#" >&2
        exit 2
    fi
    body="Members, each fixed in a commit of its own, as the Work section of AGENTS.md says:
"
    grouping=
    for n; do
        reason=$(why "$n")
        if [ -n "$reason" ]; then
            echo "board: #$n is $reason, so it cannot join a group" >&2
            exit 1
        fi
        body="$body
* #$n"
    done
    group_url=
    group_members=
    group_done=no
    group_status=0
    trap 'group_status=$?; trap - EXIT; group_rollback; exit "$group_status"' EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
    for n; do
        group_members="$group_members $n"
        if ! gh issue edit "$n" --repo "$REPO" --add-label grouped > /dev/null; then
            echo "board: #$n: cannot label it grouped" >&2
            exit 1
        fi
    done
    grouping=yes
    for n; do
        if ! reason=$(why "$n") || [ -n "$reason" ]; then
            echo "board: #$n is ${reason:-unreadable} now, so no group was made" >&2
            exit 1
        fi
    done
    if ! group_url=$(gh issue create --repo "$REPO" --title "group: $title" --label group --body "$body"); then
        echo "board: cannot create the group" >&2
        exit 1
    fi
    if ! added=$(gh project item-add "$BOARD" --owner "$OWNER" --url "$group_url" 2>&1 > /dev/null) &&
        [ -z "$(item "${group_url##*/}")" ]; then
        printf '%s\n' "$added" >&2
        echo "board: $group_url: cannot add it to the board" >&2
        exit 1
    fi
    set_status "${group_url##*/}" Ready
    group_done=yes
    echo "${group_url##*/}"
)

done_()
{
    closed=$(gh api graphql --paginate -F n="$BOARD" -f o="$OWNER" -f query='
        query($o: String!, $n: Int!, $endCursor: String) {
            user(login: $o) { projectV2(number: $n) {
                items(first: 100, after: $endCursor) {
                    pageInfo { hasNextPage endCursor }
                    nodes { fieldValueByName(name: "Status") {
                            ... on ProjectV2ItemFieldSingleSelectValue { name } }
                        content { ... on Issue { number state } } } } } } }' \
        --jq '.data.user.projectV2.items.nodes[] |
            select(.content.state == "CLOSED" and
                   (.fieldValueByName.name // "") != "Done") | .content.number')
    for n in $closed; do
        set_status "$n" Done
        printf '%s ' "$n"
    done
    echo
}

case "$1" in
    next) next ;;
    take) [ -n "$2" ] || exit 2; board_ids=$(ids); take "$2" "$3" ;;
    mine) [ -n "$2" ] || exit 2; board_ids=$(ids); mine "$2" ;;
    release) [ -n "$2" ] || exit 2; board_ids=$(ids); release "$2" "${3:-Ready}" ;;
    ready) [ -n "$3" ] || exit 2; board_ids=$(ids); shift; ready "$@" ;;
    order) [ -n "$2" ] || exit 2; board_ids=$(ids); shift; order "$@" ;;
    group) [ -n "$3" ] || exit 2; board_ids=$(ids); shift; group "$@" ;;
    done) board_ids=$(ids); done_ ;;
    *) sed -n 's/^# Usage: /usage: /p; s/^#        /       /p' "$0" >&2; exit 2 ;;
esac
