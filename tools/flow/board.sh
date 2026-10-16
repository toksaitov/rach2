#!/bin/sh
#
# board.sh: the operations the worker, manager and reviewer loops make on the
# board and on the phase in progress.
#
# Usage: board.sh next
#        board.sh take N [GROUP]
#        board.sh mine N
#        board.sh release N [STATUS|owner]
#        board.sh ready AFTER N...
#        board.sh order N...
#        board.sh group TITLE N...
#        board.sh done
#        board.sh current
#        board.sh members P [open]
#        board.sh link N...
#        board.sh start [P]
#        board.sh archive P
#        board.sh phase
#
# A phase is an issue with the label `phase`, and at most one phase has the
# Status In progress. The members of phase P are its sub-issues and the
# issues whose body says `Part of #P` with no digit after P, since GitHub links
# at most a hundred sub-issues to one parent. Workers take only the issues
# labelled `ci`, which ci files, and the members of the phase In progress.
# Each subcommand that reads the board or the issues fails, with a diagnostic
# on stderr and nothing on stdout, when any page of the answer is lost, so
# that a caller does not take a part of the board for the whole.
#
# `next` prints the issues a worker may take, one line each with the number,
# a tab and the title. Each is open, unassigned, with no open blocker and
# without the labels `grouped`, `owner` and `phase`. First come the issues
# labelled `ci` and not `intermittent`, whatever their Status; then the
# members of the phase In progress whose Status is Ready; then, while that
# phase has an open member not labelled `intermittent`, the issues labelled
# both `ci` and `intermittent`. Each tier is in the board's order. `gh project
# item-list` does not return that order, so the query asks for the items by
# position. With no phase In progress, `next` lists only the first tier.
#
# `take N [GROUP]` refuses a closed issue, setting it Done and changing
# nothing else, since a close between `next` and `take` would otherwise leave
# it In progress. Before it assigns anything, it also refuses a phase, an
# issue with an open blocker, one labelled `owner` and, without GROUP, an
# issue that is neither labelled `ci` nor a member of the phase In progress,
# and a member whose Status is neither Ready nor In progress.
# Otherwise it assigns N to this account and reads the issue's timeline.
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
# that a caller does not take a lost connection for a closed issue. It makes
# none of the checks of `take`: an issue a worker holds is resumed after it
# gains a blocker or the label `owner`, since its work may be committed only
# in that worker's clone, and the session that reads the issue hands it back
# with `release` when it cannot go on.
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
# group, and labelled `ci` or a member of the phase In progress. The group is
# an issue titled `group: TITLE` with the label `group`, and `ci` as well when
# a member has it, whose body lists the members one a line as `* #N`. While a
# phase P is In progress, the body ends with the line `Part of #P.` and the
# group is linked to P as a sub-issue; a link that GitHub refuses is no
# failure, since the line makes the group a member. Each member gets the
# label `grouped`, which keeps it out of `next` and `take`, and the group is
# set Ready, for `ready` or `order` to place. It prints the group's number.
# `group` labels the members before it reads their assignees again, as `take`
# assigns before it reads the label, so that of a take and a grouping that
# overlap, at least one sees the other and fails; when a member was taken,
# `group` removes the labels and makes no group. The board adds each new
# issue itself, and `gh project item-add` fails when the board was first;
# `group` then reads the board and fails only when the group is not on it.
# A failure removes the labels attempted by this call and closes any group
# it created. If cleanup fails, stderr gives the steps left to do.
#
# `done` sets Done on every closed issue whose Status is not, and fails when
# the listing of the board fails. The manager runs it at each check, for an
# issue that closes while a `take` or a `release` of it runs.
#
# `current` prints the number of the phase In progress, prints nothing when
# there is none, and fails when two or more are.
#
# `members P [open]` prints the members of P, or only the open ones, sorted by
# number, one a line: the number, the state, the time it closed or `-`, the
# labels joined by commas, and the title, separated by tabs. It reads the
# repository's issues through GraphQL, every page, and not through the search
# API, whose index lags behind the issues.
#
# `link N...` makes each N a sub-issue of the phase In progress P. When GitHub
# refuses, as it does past a hundred sub-issues, it adds the line `Part of
# #P.` to N's body instead. A member of P is left as it is. It fails when no
# phase is In progress or when an N got neither the link nor the line.
#
# `start [P]` starts a phase. With none In progress, it sets P In progress, or
# without P the top open phase whose Status is Ready, and prints nothing and
# succeeds when no phase is Ready. With a phase In progress, it completes that
# phase's start, and refuses any other P. For the phase In progress, it
# comments `phase: started at SHA`, SHA being master's head, unless a comment
# of that form is there already; it links, as `link` does, each open issue
# labelled `triaged` that has no parent, whose body says no `Part of #N`, and
# that has none of the labels `phase`, `group`, `grouped`, `ci`, `owner` and
# `intermittent`; and it sets Ready each open member whose Status is Backlog
# or empty and that is not labelled `owner`, `grouped` or `phase`. It prints
# the phase's number. Each step skips what an earlier run did, so a second
# run changes nothing, and a run that failed part way is completed by the
# next.
#
# `archive P` archives the card of every closed issue on the board, except a
# closed member of an open phase other than P, which stays on the board until
# that phase is archived. P's own card is archived once P is closed. It prints
# the numbers it archived. A card that cannot be archived is reported on
# stderr and the rest go on, and the exit status is then 1.
#
# `phase` reports on the phase In progress, for the manager and the reviewer,
# in ten lines, in this order:
#
#    phase: #P TITLE      or `phase: none`
#    base: SHA            from P's first comment `phase: started at SHA`
#    head: SHA            master's head
#    members: O open, C closed
#    work: #N...          open members and open issues labelled `ci`
#    intermittent: #N...  those of them labelled `intermittent`
#    hold: #N...          open issues labelled `hold`
#    review: BASE..HEAD   from P's newest line `review: BASE..HEAD complete`
#    core: STATE          the commit status `ci/core` on master's head
#    ready: yes HEAD      or `ready: no: REASON`
#
# `work` leaves out the issues labelled `intermittent`. An empty list, a
# review not found and a missing status are `none`, and a base not found is
# `unknown`. The reason is the first of these that applies: `no phase is In
# progress`; `work: #N...`; `hold #N...`; `review failed`, when P has the label
# `review-failed`; `review`, when the newest review did not end at head; and
# `core STATE`. When something cannot be read, the last line is `ready: no:
# unreadable: WHAT` and the exit status is 1; otherwise it is 0.
#
# The repository and the board's owner and number are below. The ids of the
# board, its Status field and the field's options are looked up by name on
# each call, so another board needs only these three lines changed.

# shellcheck disable=SC2016 # the dollars are GraphQL's and jq's
set -e

REPO=toksaitov/rach2
OWNER=toksaitov
BOARD=6

# The awk functions that read the lists below: has(L, X) is true when the
# comma-joined list L has X, and in_phase(N, PARENT, PARTOF, P) is true when
# issue N is a member of phase P, which is not empty, PARENT being N's parent
# and PARTOF the numbers M for which N's body says `Part of #M`.
LIB='function has(l, x) { return index("," l ",", "," x ",") > 0 }
    function in_phase(n, parent, partof, p) {
        return p != "" && n != p && (parent == p || has(partof, p)) }'

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

# items: print the issues on the board in the board's order, one a line, as
# ten fields separated by tabs: the item's id, the Status or nothing, the
# number, the state, the labels joined by commas, the number of assignees,
# the number of open blockers, the parent's number or nothing, the numbers
# that `Part of #N` gives in the body joined by commas, and the title. Fail
# when any page of the answer is lost.
items()
{
    gh api graphql --paginate -F n="$BOARD" -f o="$OWNER" -f query='
        query($o: String!, $n: Int!, $endCursor: String) {
            user(login: $o) { projectV2(number: $n) {
                items(first: 100, after: $endCursor,
                      orderBy: {field: POSITION, direction: ASC}) {
                    pageInfo { hasNextPage endCursor }
                    nodes { id isArchived
                        fieldValueByName(name: "Status") {
                            ... on ProjectV2ItemFieldSingleSelectValue { name } }
                        content { ... on Issue { number title state body
                            assignees(first: 1) { totalCount }
                            labels(first: 20) { nodes { name } }
                            parent { number }
                            issueDependenciesSummary { blockedBy } } } } } } } }' \
        --jq '.data.user.projectV2.items.nodes[] |
            select(.content.number != null and (.isArchived | not)) |
            [.id, (.fieldValueByName.name // ""),
             (.content.number | tostring), .content.state,
             ([.content.labels.nodes[].name] | join(",")),
             (.content.assignees.totalCount | tostring),
             (.content.issueDependenciesSummary.blockedBy | tostring),
             (.content.parent.number // "" | tostring),
             ([.content.body // "" | scan("Part of #([0-9]+)") | .[0]] |
              unique | join(",")),
             (.content.title | gsub("\t"; " "))] | join("\t")'
}

# issues [open]: print the repository's issues, or only the open ones, one a
# line, as seven fields separated by tabs: the number, the state, the time it
# closed or `-`, the labels joined by commas, the parent's number or nothing,
# the numbers that `Part of #N` gives in the body joined by commas, and the
# title. Fail when any page of the answer is lost.
issues()
{
    states='OPEN, CLOSED'
    [ "$1" != open ] || states=OPEN
    gh api graphql --paginate -f o="${REPO%/*}" -f r="${REPO#*/}" -f query='
        query($o: String!, $r: String!, $endCursor: String) {
            repository(owner: $o, name: $r) {
                issues(first: 100, after: $endCursor, states: ['"$states"']) {
                    pageInfo { hasNextPage endCursor }
                    nodes { number state closedAt title body
                        labels(first: 20) { nodes { name } }
                        parent { number } } } } }' \
        --jq '.data.repository.issues.nodes[] |
            [(.number | tostring), .state, (.closedAt // "-"),
             ([.labels.nodes[].name] | join(",")),
             (.parent.number // "" | tostring),
             ([.body // "" | scan("Part of #([0-9]+)") | .[0]] |
              unique | join(",")),
             (.title | gsub("\t"; " "))] | join("\t")'
}

# read_board: read the board into `board`, or say that it cannot and exit 1.
read_board()
{
    board=$(items) || {
        echo "board: cannot read the board" >&2
        exit 1
    }
}

# in_progress: print the number of the open phase whose Status is In progress
# on the board read into `board`, or nothing when there is none; say so and
# fail when there are two or more.
in_progress()
{
    ps=$(printf '%s\n' "$board" | awk -F '\t' "$LIB"'
        $4 == "OPEN" && $2 == "In progress" && has($5, "phase") {
            s = s (s == "" ? "" : " #") $3 }
        END { print s }')
    case $ps in
        *' '*)
            echo "board: #$ps are In progress, and at most one phase may be" >&2
            return 1 ;;
    esac
    [ -z "$ps" ] || echo "$ps"
}

next()
{
    read_board
    p=$(in_progress) || exit 1
    printf '%s\n' "$board" | awk -F '\t' -v p="$p" "$LIB"'
        { member = in_phase($3, $8, $9, p) }
        $4 == "OPEN" && member && !has($5, "intermittent") { work = 1 }
        $4 != "OPEN" || $6 > 0 || $7 > 0 { next }
        has($5, "grouped") || has($5, "owner") || has($5, "phase") { next }
        has($5, "ci") && !has($5, "intermittent") { ci = ci $3 "\t" $10 "\n"; next }
        has($5, "ci") { flaky = flaky $3 "\t" $10 "\n"; next }
        member && $2 == "Ready" { ready = ready $3 "\t" $10 "\n" }
        END { printf "%s%s%s", ci, ready, (work ? flaky : "") }'
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

# kind N [GROUP]: print what keeps N from a worker on the board read into
# `board`, `p` being the phase In progress and GROUP, when given, the group N
# is taken for; print nothing when nothing does.
kind()
{
    printf '%s\n' "$board" | awk -F '\t' -v n="$1" -v p="$p" -v g="$2" "$LIB"'
        $3 != n { next }
        { found = 1 }
        has($5, "phase") { print "it is a phase"; exit }
        $7 > 0 { print "it has an open blocker"; exit }
        has($5, "owner") { print "it waits for the owner"; exit }
        g != "" || has($5, "ci") { exit }
        p == "" { print "it is not labelled ci, and no phase is In progress"; exit }
        !in_phase($3, $8, $9, p) {
            print "it is neither labelled ci nor a member of #" p; exit }
        $2 != "Ready" && $2 != "In progress" {
            print "its Status is " ($2 == "" ? "empty" : $2) ", not Ready"; exit }
        END { if (!found) print "it is not on the board" }'
}

take()
{
    if finished "$1"; then
        exit 1
    fi
    me=$(gh api user --jq .login) added=no
    board=$(items) || refuse "$1" "cannot read the board"
    p=$(in_progress) || exit 1
    reason=$(kind "$1" "$2")
    [ -z "$reason" ] || refuse "$1" "$reason"
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
    gh issue view "$1" --repo "$REPO" --json state,assignees,labels --jq '
        if .state != "OPEN" then "closed"
        elif (.assignees | length) > 0 then "assigned"
        elif "'"$grouping"'" == "" and any(.labels[]; .name == "grouped") then
            "in a group"
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
    read_board
    p=$(in_progress) || exit 1
    body="Members, each fixed in a commit of its own, as the Work section of AGENTS.md says:
"
    labels=group
    grouping=
    for n; do
        reason=$(why "$n")
        if [ -n "$reason" ]; then
            echo "board: #$n is $reason, so it cannot join a group" >&2
            exit 1
        fi
        case $(printf '%s\n' "$board" | awk -F '\t' -v n="$n" -v p="$p" "$LIB"'
                $3 == n && has($5, "ci") { print "ci"; exit }
                $3 == n && in_phase($3, $8, $9, p) { print "member" }') in
            ci) labels=group,ci ;;
            member) ;;
            *)
                echo "board: #$n is neither labelled ci nor a member of the phase In progress, so it cannot join a group" >&2
                exit 1 ;;
        esac
        body="$body
* #$n"
    done
    [ -z "$p" ] || body="$body

Part of #$p."
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
    if ! group_url=$(gh issue create --repo "$REPO" --title "group: $title" --label "$labels" --body "$body"); then
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
    if [ -n "$p" ]; then
        sub_issue "${group_url##*/}" "$p" > /dev/null 2>&1 || :
    fi
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

current()
{
    read_board
    in_progress || exit 1
}

members()
{
    list=$(issues "$2") || {
        echo "board: cannot read the issues" >&2
        exit 1
    }
    printf '%s\n' "$list" | awk -F '\t' -v p="$1" "$LIB"'
        in_phase($1, $5, $6, p) {
            print $1 "\t" $2 "\t" $3 "\t" $4 "\t" $7 }' | sort -n
}

# sub_issue N P: make N a sub-issue of P; fail, with GitHub's answer on
# stderr, when GitHub refuses.
sub_issue()
{
    id=$(gh api "repos/$REPO/issues/$1" --jq .id) || return 1
    gh api -X POST "repos/$REPO/issues/$2/sub_issues" -F sub_issue_id="$id" > /dev/null
}

# attach N P: make N a sub-issue of P or, when GitHub refuses, add the line
# `Part of #P.` to N's body; say which on stderr when it is the line. Fail
# when neither is done.
attach()
{
    if refused=$(sub_issue "$1" "$2" 2>&1); then
        return 0
    fi
    if text=$(gh issue view "$1" --repo "$REPO" --json body --jq .body) &&
        gh issue edit "$1" --repo "$REPO" --body "${text:+$text

}Part of #$2." > /dev/null; then
        echo "board: #$1: not linked to #$2: its body says Part of #$2" >&2
        return 0
    fi
    printf '%s\n' "$refused" >&2
    echo "board: #$1: cannot link it to #$2 or add Part of #$2 to its body" >&2
    return 1
}

link()
{
    read_board
    p=$(in_progress) || exit 1
    if [ -z "$p" ]; then
        echo "board: no phase is In progress" >&2
        exit 1
    fi
    status=0
    for n; do
        linked=$(printf '%s\n' "$board" | awk -F '\t' -v n="$n" -v p="$p" "$LIB"'
            $3 == n && in_phase($3, $8, $9, p) { print "yes" }')
        [ -n "$linked" ] || attach "$n" "$p" || status=1
    done
    return "$status"
}

# master: print master's head, 40 hex digits; fail when it cannot be read.
master()
{
    sha=$(gh api "repos/$REPO/commits/master" --jq .sha) || return 1
    printf '%s\n' "$sha" | grep -Ex '[0-9a-f]{40}'
}

# comments N: print the bodies of N's comments, oldest first.
comments()
{
    gh api --paginate "repos/$REPO/issues/$1/comments" --jq '.[].body'
}

start()
{
    read_board
    p=$(in_progress) || exit 1
    if [ -n "$p" ]; then
        if [ -n "$1" ] && [ "$1" != "$p" ]; then
            echo "board: #$p is In progress, so #$1 cannot start" >&2
            exit 1
        fi
    else
        p=$(printf '%s\n' "$board" | awk -F '\t' -v p="$1" "$LIB"'
            $4 != "OPEN" || !has($5, "phase") { next }
            p == "" && $2 == "Ready" || $3 == p { print $3; exit }')
        if [ -z "$p" ]; then
            [ -z "$1" ] || {
                echo "board: #$1 is not an open phase on the board" >&2
                exit 1
            }
            return 0
        fi
        set_status "$p" "In progress" || {
            echo "board: #$p: cannot set it In progress" >&2
            exit 1
        }
    fi
    said=$(comments "$p") || {
        echo "board: #$p: cannot read its comments" >&2
        exit 1
    }
    if ! printf '%s\n' "$said" | grep -Eqx 'phase: started at [0-9a-f]{40}'; then
        head=$(master) || {
            echo "board: cannot read master's head" >&2
            exit 1
        }
        gh issue comment "$p" --repo "$REPO" --body "phase: started at $head" > /dev/null || {
            echo "board: #$p: cannot comment on it" >&2
            exit 1
        }
    fi
    orphans=$(printf '%s\n' "$board" | awk -F '\t' "$LIB"'
        $4 == "OPEN" && has($5, "triaged") && $8 == "" && $9 == "" &&
        !has($5, "phase") && !has($5, "group") && !has($5, "grouped") &&
        !has($5, "ci") && !has($5, "owner") && !has($5, "intermittent") {
            print $3 }')
    status=0 attached=
    for n in $orphans; do
        if attach "$n" "$p"; then
            attached="$attached,$n"
        else
            status=1
        fi
    done
    promote=$(printf '%s\n' "$board" | awk -F '\t' -v p="$p" -v o="$attached" "$LIB"'
        $4 != "OPEN" || $3 == p { next }
        !in_phase($3, $8, $9, p) && !has(o, $3) { next }
        $2 != "" && $2 != "Backlog" { next }
        !has($5, "owner") && !has($5, "grouped") && !has($5, "phase") { print $3 }')
    for n in $promote; do
        set_status "$n" Ready || {
            echo "board: #$n: cannot set it Ready" >&2
            status=1
        }
    done
    [ "$status" -eq 0 ] || exit 1
    echo "$p"
}

archive()
{
    read_board
    cards=$(printf '%s\n' "$board" | awk -F '\t' -v p="$1" "$LIB"'
        { line[NR] = $0 }
        $4 == "OPEN" && has($5, "phase") && $3 != p { open[$3] = 1 }
        END {
            for (i = 1; i <= NR; i++) {
                split(line[i], f, "\t")
                if (f[4] != "CLOSED" || f[8] in open)
                    continue
                kept = 0
                m = split(f[9], q, ",")
                for (j = 1; j <= m; j++)
                    if (q[j] in open)
                        kept = 1
                if (!kept)
                    print f[1] " " f[3]
            }
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

# unreadable WHAT: end the report of `phase` with the line that says that
# WHAT cannot be read, and exit 1.
unreadable()
{
    echo "board: phase: cannot read $1" >&2
    echo "ready: no: unreadable: $1"
    exit 1
}

phase()
{
    board=$(items) || unreadable board
    p=$(in_progress) || unreadable 'the phase In progress'
    base='' review=''
    if [ -n "$p" ]; then
        echo "phase: #$p $(printf '%s\n' "$board" |
            awk -F '\t' -v p="$p" '$3 == p { print $10; exit }')"
        said=$(comments "$p") || unreadable "the comments on #$p"
        base=$(printf '%s\n' "$said" |
            sed -n 's/^phase: started at \([0-9a-f]\{40\}\)$/\1/p' | head -n 1)
        review=$(printf '%s\n' "$said" |
            sed -n 's/^review: \([0-9a-f]\{40\}\.\.[0-9a-f]\{40\}\) complete$/\1/p' |
            tail -n 1)
    else
        echo 'phase: none'
    fi
    echo "base: ${base:-unknown}"
    head=$(master) || unreadable "master's head"
    echo "head: $head"
    list=$(issues) || unreadable issues
    lists=$(printf '%s\n' "$list" | sort -n | awk -F '\t' -v p="$p" "$LIB"'
        { member = in_phase($1, $5, $6, p) }
        member && $2 == "OPEN" { open++ }
        member && $2 == "CLOSED" { closed++ }
        $1 == p && has($4, "review-failed") { failed = "yes" }
        $2 != "OPEN" { next }
        (member || has($4, "ci")) && !has($4, "intermittent") { work = work " #" $1 }
        (member || has($4, "ci")) && has($4, "intermittent") { flaky = flaky " #" $1 }
        has($4, "hold") { hold = hold " #" $1 }
        END {
            printf "members: %d open, %d closed\n", open, closed
            printf "work:%s\n", (work == "" ? " none" : work)
            printf "intermittent:%s\n", (flaky == "" ? " none" : flaky)
            printf "hold:%s\n", (hold == "" ? " none" : hold)
            printf "failed: %s\n", (failed == "" ? "no" : failed)
        }')
    printf '%s\n' "$lists" | grep -v '^failed: '
    echo "review: ${review:-none}"
    core=$(gh api "repos/$REPO/commits/$head/status" --jq '
        [.statuses[] | select(.context == "ci/core") | .state][0] // "none"') ||
        unreadable "the state of ci/core"
    echo "core: $core"
    work=$(printf '%s\n' "$lists" | sed -n 's/^work: //p')
    hold=$(printf '%s\n' "$lists" | sed -n 's/^hold: //p')
    if [ -z "$p" ]; then
        echo 'ready: no: no phase is In progress'
    elif [ "$work" != none ]; then
        echo "ready: no: work: $work"
    elif [ "$hold" != none ]; then
        echo "ready: no: hold $hold"
    elif printf '%s\n' "$lists" | grep -qx 'failed: yes'; then
        echo 'ready: no: review failed'
    elif [ -z "$review" ] || [ "${review#*..}" != "$head" ]; then
        echo 'ready: no: review'
    elif [ "$core" != success ]; then
        echo "ready: no: core $core"
    else
        echo "ready: yes $head"
    fi
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
    current) current ;;
    members)
        [ -n "$2" ] || exit 2
        [ -z "$3" ] || [ "$3" = open ] || exit 2
        members "$2" "$3" ;;
    link) [ -n "$2" ] || exit 2; shift; link "$@" ;;
    start) board_ids=$(ids); start "$2" ;;
    archive) [ -n "$2" ] || exit 2; archive "$2" ;;
    phase) phase ;;
    *) sed -n 's/^# Usage: /usage: /p; s/^#        /       /p' "$0" >&2; exit 2 ;;
esac
