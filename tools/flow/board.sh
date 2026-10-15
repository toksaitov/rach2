#!/bin/sh
#
# board.sh: the operations the worker and manager loops make on the board.
#
# Usage: board.sh next
#        board.sh take N
#        board.sh mine N
#        board.sh ready AFTER N...
#        board.sh done
#
# `next` prints the issues a worker may take, in the order AGENTS.md gives:
# the open issues whose titles begin `ci:`, then the Ready issues in the
# board's order, each open, unassigned and with no open blocker, one line each
# with the number, a tab and the title. `gh project item-list` does not return
# the board's order, so the query asks for the items by position. When any
# page of the answer is lost, `next` prints nothing and fails, so that a caller
# does not take a part of the board for the whole.
#
# `take N` assigns N to this account and sets it In progress, then reads the
# issue's timeline. When another account is assigned too and was assigned
# first, it unassigns this account and exits 1, so that no issue has two
# writers.
#
# `mine N` prints `yes` while N is open and assigned to this account and `no`
# otherwise; it prints nothing and fails when GitHub cannot be reached, so
# that a caller does not take a lost connection for a closed issue.
#
# `ready AFTER N...` sets each N Ready and moves it after AFTER on the board,
# the first N after AFTER and each later one after the one before. `done` sets
# Done on every closed issue whose Status is not.
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
                            blockedBy(first: 20) { nodes { state } } } } } } } } }' \
        --jq '.data.user.projectV2.items.nodes[] |
            select(.content.state == "OPEN" and
                   .content.assignees.totalCount == 0 and
                   ([.content.blockedBy.nodes[] | select(.state == "OPEN")]
                    | length) == 0) |
            "\(.fieldValueByName.name // "-")\t\(.content.number)\t\(.content.title)"')
    printf '%s\n' "$answer" |
        awk -F '\t' '$3 ~ /^ci:/ { print $2 "\t" $3; next }
            $1 == "Ready" { r = r $2 "\t" $3 "\n" } END { printf "%s", r }'
}

take()
{
    me=$(gh api user --jq .login)
    gh issue edit "$1" --repo "$REPO" --add-assignee "$me" > /dev/null
    set_status "$1" "In progress"
    now=$(gh issue view "$1" --repo "$REPO" --json assignees --jq '.assignees[].login' | tr '\n' ' ')
    first=$(gh api --paginate "repos/$REPO/issues/$1/timeline" \
        --jq '.[] | select(.event == "assigned") | .assignee.login' |
        awk -v now=" $now" 'index(now, " " $0 " ") { print; exit }')
    if [ "$first" != "$me" ]; then
        gh issue edit "$1" --repo "$REPO" --remove-assignee "$me" > /dev/null
        echo "board: #$1: taken by $first first" >&2
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

ready()
{
    prev=$(item "$1")
    shift
    for n; do
        i=$(item "$n")
        set_status "$n" Ready
        gh api graphql -f p="$(id_of project)" -f i="$i" -f a="$prev" -f query='
            mutation($p: ID!, $i: ID!, $a: ID!) {
                updateProjectV2ItemPosition(input: {projectId: $p, itemId: $i,
                    afterId: $a}) { clientMutationId } }' > /dev/null
        prev=$i
        printf '%s ' "$n"
    done
    echo
}

done_()
{
    gh api graphql --paginate -F n="$BOARD" -f o="$OWNER" -f query='
        query($o: String!, $n: Int!, $endCursor: String) {
            user(login: $o) { projectV2(number: $n) {
                items(first: 100, after: $endCursor) {
                    pageInfo { hasNextPage endCursor }
                    nodes { fieldValueByName(name: "Status") {
                            ... on ProjectV2ItemFieldSingleSelectValue { name } }
                        content { ... on Issue { number state } } } } } } }' \
        --jq '.data.user.projectV2.items.nodes[] |
            select(.content.state == "CLOSED" and
                   (.fieldValueByName.name // "") != "Done") | .content.number' |
    while read -r n; do
        set_status "$n" Done
        printf '%s ' "$n"
    done
    echo
}

case "$1" in
    next) next ;;
    take|mine) [ -n "$2" ] || exit 2; board_ids=$(ids); "$1" "$2" ;;
    ready) [ -n "$3" ] || exit 2; board_ids=$(ids); shift; ready "$@" ;;
    done) board_ids=$(ids); done_ ;;
    *) sed -n 's/^# Usage: /usage: /p; s/^#        /       /p' "$0" >&2; exit 2 ;;
esac
