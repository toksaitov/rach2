#!/bin/sh
#
# board.sh: the operations the worker and manager loops make on the board.
#
# Usage: board.sh next
#        board.sh take N
#        board.sh mine N
#        board.sh release N [STATUS]
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
# `take N` assigns N to this account and reads the issue's timeline. Of the
# accounts assigned now, the one whose latest assignment is the earliest keeps
# the issue. When that is another account, `take` unassigns this one and exits
# 1, so that no issue has two writers; otherwise it sets N In progress. A step
# that fails after the assignment unassigns this account again, so that a
# failed take leaves the issue as it found it.
#
# `mine N` prints `yes` while N is open and assigned to this account and `no`
# otherwise; it prints nothing and fails when GitHub cannot be reached, so
# that a caller does not take a lost connection for a closed issue.
#
# `release N [STATUS]` hands N back: it unassigns this account, removes the
# label `grouped`, and sets STATUS, Ready unless given, so that N returns to
# the queue as an issue of its own.
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
# member gets the label `grouped`, which keeps it out of `next`, and the group
# is set Ready, for `ready` or `order` to place. It prints the group's number.
#
# `done` sets Done on every closed issue whose Status is not, and fails when
# the listing of the board fails.
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

# unassign N LOGIN: remove LOGIN from N's assignees, or say that it could not.
unassign()
{
    gh issue edit "$1" --repo "$REPO" --remove-assignee "$2" > /dev/null ||
        echo "board: #$1: could not unassign $2; unassign it by hand" >&2
}

# first N: print the account, of those assigned to N now, whose latest
# assignment is the earliest, the lesser login on a tie, or the one account
# assigned when the timeline does not show its assignment yet.
first()
{
    now=$(gh issue view "$1" --repo "$REPO" --json assignees --jq '.assignees[].login') ||
        return 1
    events=$(gh api --paginate "repos/$REPO/issues/$1/timeline" \
        --jq '.[] | select(.event == "assigned") | "\(.created_at) \(.assignee.login)"') ||
        return 1
    printf '%s\n' "$events" | awk -v now="$(printf '%s\n' "$now" | tr '\n' ' ')" '
        BEGIN { n = split(now, a, " "); for (i = 1; i <= n; i++) cur[a[i]] = 1 }
        ($2 in cur) { last[$2] = $1 }
        END {
            for (l in last)
                if (who == "" || last[l] < last[who] ||
                    (last[l] == last[who] && l < who))
                    who = l
            if (who == "" && n == 1)
                who = a[1]
            print who
        }'
}

take()
{
    me=$(gh api user --jq .login)
    gh issue edit "$1" --repo "$REPO" --add-assignee "$me" > /dev/null
    if ! who=$(first "$1"); then
        unassign "$1" "$me"
        echo "board: #$1: cannot read who was assigned first" >&2
        exit 1
    fi
    if [ "$who" != "$me" ]; then
        unassign "$1" "$me"
        echo "board: #$1: taken by $who first" >&2
        exit 1
    fi
    if ! set_status "$1" "In progress"; then
        unassign "$1" "$me"
        echo "board: #$1: cannot set it In progress" >&2
        exit 1
    fi
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
    me=$(gh api user --jq .login)
    gh issue edit "$1" --repo "$REPO" --remove-assignee "$me" --remove-label grouped > /dev/null
    set_status "$1" "$2"
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

group()
{
    title=$1
    shift
    if [ $# -gt 8 ]; then
        echo "board: a group has at most 8 members, not $#" >&2
        exit 2
    fi
    body="Members, each fixed in a commit of its own, as the Work section of AGENTS.md says:
"
    for n; do
        why=$(gh issue view "$n" --repo "$REPO" --json state,assignees,labels,title --jq '
            if .state != "OPEN" then "closed"
            elif (.assignees | length) > 0 then "assigned"
            elif any(.labels[]; .name == "grouped") then "in a group"
            elif (.title | startswith("ci:")) then "filed by ci"
            else empty end')
        if [ -n "$why" ]; then
            echo "board: #$n is $why, so it cannot join a group" >&2
            exit 1
        fi
        body="$body
* #$n"
    done
    url=$(gh issue create --repo "$REPO" --title "group: $title" --label group --body "$body")
    for n; do
        gh issue edit "$n" --repo "$REPO" --add-label grouped > /dev/null
    done
    gh project item-add "$BOARD" --owner "$OWNER" --url "$url" > /dev/null
    set_status "${url##*/}" Ready
    echo "${url##*/}"
}

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
    take|mine) [ -n "$2" ] || exit 2; board_ids=$(ids); "$1" "$2" ;;
    release) [ -n "$2" ] || exit 2; board_ids=$(ids); release "$2" "${3:-Ready}" ;;
    ready) [ -n "$3" ] || exit 2; board_ids=$(ids); shift; ready "$@" ;;
    order) [ -n "$2" ] || exit 2; board_ids=$(ids); shift; order "$@" ;;
    group) [ -n "$3" ] || exit 2; board_ids=$(ids); shift; group "$@" ;;
    done) board_ids=$(ids); done_ ;;
    *) sed -n 's/^# Usage: /usage: /p; s/^#        /       /p' "$0" >&2; exit 2 ;;
esac
