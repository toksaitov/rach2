#!/bin/sh
#
# reviewer.sh: review each phase of the board once its issues have closed,
# one review at a time, until STATE/stop exists.
#
# Usage: reviewer.sh [-a codex] [-s STATE]
#
# A phase is an issue with the label `phase`. Its issues are its sub-issues
# and the issues whose body says `Part of #P`, since GitHub links at most a
# hundred sub-issues. The loop reviews a phase once: when it has issues, all
# of them are closed, the last closed after the loop first started, which
# STATE/since records, and it has no label `reviewed` yet. A number written to
# STATE/queue, one a line, is reviewed next, finished or not.
#
# A review is one run of a reviewer's command-line program, which AGENT
# names, through the adapter for it in `review`. For `codex`, the default and
# so far the only one, it is `codex exec` with gpt-6-astra at xhigh effort as
# the lead, which decides how to review and may start at most three subagents
# at a time, gpt-6.1-sol at high effort, in a read-only sandbox in a worktree
# of this clone at master's newest commit, for at most three hours. Its
# prompt is reviewer.txt with the phase filled in, and its answer is JSON in
# the form reviewer.json gives. Each finding becomes a `bug` issue that says
# `Part of #P`, linked as a sub-issue while the phase has room, for the
# manager to triage; a comment on the phase gives the summary and the issues
# filed, and the phase gets the label `reviewed`. A review that fails or
# reaches its limit says so on the phase, files nothing and is not repeated
# unless queued; one that ends on the account's usage limit is repeated after
# fifteen minutes. Nothing waits for a review.
#
# STATE, `~/flow/reviewer` unless given, holds the log `loop.log`, `since`,
# `queue`, `failed`, the worktree `wt`, and each review's output as
# `r-P-TIME.jsonl`, `.err` and `.json`. `touch STATE/stop` ends the loop
# after the review that is running.

# shellcheck disable=SC2016 # the dollars are GraphQL's, jq's and perl's
REPO=toksaitov/rach2
LEAD=gpt-6-astra
LEAD_EFFORT=xhigh
SUB=gpt-6.1-sol
SUB_EFFORT=high
SUBAGENTS=3
DEADLINE=10800
PERIOD=1200

# candidates: print the phases not yet reviewed whose linked sub-issues are
# all closed, oldest first.
candidates()
{
    gh api graphql -f o="${REPO%/*}" -f r="${REPO#*/}" -f query='
        query($o: String!, $r: String!) { repository(owner: $o, name: $r) {
            issues(labels: ["phase"], states: [OPEN, CLOSED], first: 100,
                   orderBy: {field: CREATED_AT, direction: ASC}) {
                nodes { number labels(first: 20) { nodes { name } }
                    subIssuesSummary { total completed } } } } }' \
        --jq '.data.repository.issues.nodes[] |
            select(([.labels.nodes[].name] | index("reviewed")) == null and
                   .subIssuesSummary.total > 0 and
                   .subIssuesSummary.total == .subIssuesSummary.completed) |
            .number'
}

# members P: print phase P's issues, one a line, as number, state, the time it
# closed or `-`, and title, separated by tabs.
members()
{
    linked=$(gh api graphql -F n="$1" -f o="${REPO%/*}" -f r="${REPO#*/}" -f query='
        query($o: String!, $r: String!, $n: Int!) {
            repository(owner: $o, name: $r) { issue(number: $n) {
                subIssues(first: 100) { nodes { number state closedAt title } } } } }' \
        --jq '.data.repository.issue.subIssues.nodes[] |
            "\(.number)\t\(.state)\t\(.closedAt // "-")\t\(.title)"') || return 1
    named=$(gh issue list --repo "$REPO" --state all --limit 1000 \
        --search "\"Part of #$1\" in:body" --json number,state,closedAt,title,body \
        --jq ".[] | select(.body | test(\"Part of #$1([^0-9]|\$)\")) |
            \"\(.number)\t\(.state)\t\(.closedAt // \"-\")\t\(.title)\"") || return 1
    printf '%s\n%s\n' "$linked" "$named" | grep . | sort -u -n
}

# pick: print the phase to review next, or nothing; fail when GitHub cannot be
# reached.
pick()
{
    q=$(sed -n '/^[0-9][0-9]*$/{p;q;}' "$state/queue" 2> /dev/null)
    if [ -n "$q" ]; then
        echo "$q"
        return 0
    fi
    phases=$(candidates) || return 1
    for p in $phases; do
        ! grep -qx "$p" "$state/failed" 2> /dev/null || continue
        m=$(members "$p") || return 1
        printf '%s\n' "$m" | awk -F '\t' '$2 != "CLOSED" { open = 1 } END { exit open }' ||
            continue
        last=$(printf '%s\n' "$m" | awk -F '\t' '$3 > l { l = $3 } END { print l }')
        if awk -v a="$last" -v b="$(cat "$state/since")" 'BEGIN { exit !(a > b) }'; then
            echo "$p"
            return 0
        fi
    done
}

# drop P: remove P from the queue.
drop()
{
    [ -f "$state/queue" ] || return 0
    grep -vx "$1" "$state/queue" > "$state/queue.new"
    mv "$state/queue.new" "$state/queue"
}

# findings P COMMIT ANSWER: file each finding of the JSON file ANSWER as a
# `bug` issue of phase P, and print the numbers filed.
findings()
{
    answer=$3
    n=$(jq '.findings | length' "$answer")
    i=0
    while [ "$i" -lt "$n" ]; do
        body="Found by the review of phase #$1 at $2, which \`tools/flow/reviewer.sh\` ran with $agent.

Severity: $(get severity). In \`$(get file):$(get line)\`.

$(get claim)

Evidence: $(get evidence)

Fix: $(get fix)

Part of #$1."
        if url=$(gh issue create --repo "$REPO" --title "$(get title)" --label bug --body "$body"); then
            id=$(gh api "repos/$REPO/issues/${url##*/}" --jq .id) &&
                gh api -X POST "repos/$REPO/issues/$1/sub_issues" -F sub_issue_id="$id" \
                    > /dev/null 2>&1
            printf '#%s ' "${url##*/}"
        else
            log "#$1: could not file finding $i: $(get title)"
        fi
        i=$((i + 1))
    done
}

# get FIELD: print FIELD of finding i of the answer that `findings` reads.
get()
{
    jq -r ".findings[$i].$1" "$answer"
}

# review P: review phase P with the agent's adapter, file what it finds, and
# return 0, 75 when the account's usage limit ended it, or 1 when it failed.
review()
{
    p=$1
    base=$state/r-$p-$(date -u '+%m%d-%H%M%S')
    git worktree remove --force "$state/wt" 2> /dev/null
    rm -rf "$state/wt"
    git worktree prune
    if ! git worktree add -q --detach "$state/wt" origin/master; then
        log "#$p: no worktree"
        return 1
    fi
    commit=$(git -C "$state/wt" rev-parse --short HEAD)
    title=$(gh issue view "$p" --repo "$REPO" --json title --jq .title) &&
        issues=$(members "$p" | awk -F '\t' '{ print "* #" $1 " " $4 }') || return 75
    prompt=$(P=$p TITLE=$title COMMIT=$commit ISSUES=$issues perl -0pe '
        s/\@PHASE\@/$ENV{P}/g; s/\@TITLE\@/$ENV{TITLE}/g;
        s/\@COMMIT\@/$ENV{COMMIT}/g; s/\@ISSUES\@/$ENV{ISSUES}/g' "$state/run/reviewer.txt")
    started=$(date +%s)
    case $agent in
        codex)
            run_session "$base.jsonl" "$base.err" "$DEADLINE" \
                codex exec -m "$LEAD" -c model_reasoning_effort="\"$LEAD_EFFORT\"" \
                -c agents.max_threads=$((SUBAGENTS + 1)) -c agents.max_depth=1 \
                -c agents.default_subagent_model="\"$SUB\"" \
                -c agents.default_subagent_reasoning_effort="\"$SUB_EFFORT\"" \
                -s read-only --output-schema "$state/run/reviewer.json" \
                -o "$base.json" --json -C "$state/wt" "$prompt" ;;
    esac
    status=$?
    took=$(($(date +%s) - started))
    if why=$(limited "$base.jsonl" "$base.err"); then
        log "#$p: limit: $why"
        return 75
    fi
    if [ "$status" -ne 0 ] || ! jq -e '.summary and .findings' "$base.json" > /dev/null 2>&1; then
        case $status in
            124) why="it reached its limit of $DEADLINE seconds" ;;
            *) why="it ended with status $status and no answer in the form reviewer.json gives" ;;
        esac
        gh issue comment "$p" --repo "$REPO" --body "The review of this phase at $commit, by \`tools/flow/reviewer.sh\` with $agent, is incomplete: $why, after $took seconds. Nothing was filed. Its output is \`$base.*\` on the reviewer's machine; write $p to the loop's \`queue\` to run it again." > /dev/null
        return 1
    fi
    filed=$(findings "$p" "$commit" "$base.json")
    gh issue comment "$p" --repo "$REPO" --body "Review of this phase at $commit by \`tools/flow/reviewer.sh\` with $agent ($LEAD at $LEAD_EFFORT effort, at most $SUBAGENTS subagents at a time of $SUB at $SUB_EFFORT), in $took seconds.

$(jq -r .summary "$base.json")

Filed: ${filed:-none}" > /dev/null
    gh issue edit "$p" --repo "$REPO" --add-label reviewed > /dev/null
    return 0
}

main()
{
    agent=codex state=$HOME/flow/reviewer repo=
    while getopts a:s:r: o; do
        case $o in
            a) agent=$OPTARG ;;
            s) state=$OPTARG ;;
            r) repo=$OPTARG ;;
            *) echo "usage: reviewer.sh [-a codex] [-s STATE]" >&2; exit 2 ;;
        esac
    done
    case $agent in
        codex) ;;
        *) echo "reviewer: no adapter for $agent" >&2; exit 2 ;;
    esac
    [ -n "$repo" ] || repo=$(cd "$(dirname "$0")/../.." && pwd)
    mkdir -p "$state"
    state=$(cd "$state" && pwd)
    cd "$repo" || exit 1
    echo "$repo" > "$state/repo"
    [ -s "$state/since" ] || date -u '+%Y-%m-%dT%H:%M:%SZ' > "$state/since"
    if [ "$0" != "$state/run/reviewer.sh" ]; then
        refresh
        update reviewer.sh
        log "loop starts: $agent in $repo"
        exec sh "$state/run/reviewer.sh" -a "$agent" -s "$state" -r "$repo"
    fi
    while [ ! -e "$state/stop" ]; do
        if ! refresh; then
            log "fetch failed"
            sleep 60
            continue
        fi
        if update reviewer.sh; then
            log "restarting as master's loop"
            exec sh "$state/run/reviewer.sh" -a "$agent" -s "$state" -r "$repo"
        fi
        if ! p=$(pick); then
            log "board unreachable"
            sleep 300
            continue
        fi
        if [ -z "$p" ]; then
            sleep "$PERIOD"
            continue
        fi
        log "review of #$p starts"
        review "$p"
        status=$?
        log "review of #$p ended $status"
        case $status in
            0) drop "$p" ;;
            75) sleep 900 ;;
            *) drop "$p"; echo "$p" >> "$state/failed" ;;
        esac
    done
    log stopped
    exit 0
}

. "$(dirname "$0")/lib.sh"
main "$@"
