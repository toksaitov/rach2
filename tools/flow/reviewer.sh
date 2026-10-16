#!/bin/sh
#
# reviewer.sh: review the phase In progress whenever its work is done and
# master has commits the last review did not read, one review at a time,
# until STATE/stop exists.
#
# Usage: reviewer.sh [-a codex] [-s STATE]
#
# The loop asks `board.sh phase` every five minutes. When it answers
# `ready: no: review`, every member of the phase In progress and every `ci`
# issue has closed, none but those labelled `intermittent` excepted, and the
# commits from BASE to master's head HEAD have not been reviewed: BASE is the
# HEAD of the phase's last complete review, or the commit the phase started
# at, which `board.sh start` commented on it. The loop reviews that range. The
# first review of a phase reads all its work; a later one, a repair review,
# reads the fixes since, those of the earlier review's findings among them.
#
# A review is one run of a reviewer's command-line program, which AGENT names,
# through the adapter for it in `review`. For `codex`, the default and so far
# the only one, it is `codex exec` with gpt-6-astra at xhigh effort as the lead,
# which decides how to review and may start at most three subagents at a time,
# gpt-6.1-sol at high effort, in a read-only sandbox in a worktree of this clone
# at HEAD, for at most three hours. Its prompt is reviewer.txt with the range
# filled in, and the text of the phase and its issues in a file beside the
# output, since the sandbox has no network; its answer is JSON in the form
# reviewer.json gives. Each finding becomes a `bug` issue that says
# `Part of #P`, linked as a sub-issue while the phase has room, and the
# summary becomes a comment on the phase. A review that read the whole range
# ends that comment with the line `review: BASE..HEAD complete`, which
# `board.sh phase` reads as the review's record; one that did not says what it
# left unread and gives the phase the label `review-failed`. When one of these
# steps fails, the loop starts no other review and tries the steps not yet
# done again every five minutes, so that no finding is lost. A finding whose
# issue GitHub created but whose answer was lost is filed again, and the
# manager's triage closes the duplicate.
#
# A review that ends without an answer in that form is run again at once, up
# to three runs for one range; one that reaches its three hours is not, since
# a second would most likely reach them too. Either way the loop writes the
# failure to STATE/failure, and starts no review until the phase has the
# label `review-failed` and a comment that says where the output is; the
# manager diagnoses it; removing the label lets the loop review the range again
# from its first run. A range too large for three hours is divided by
# writing a commit inside it, by its full hash, to STATE/until: the loop then
# reviews up to that commit first, and the rest in the next review. A review
# that ends on the account's usage limit is run again after fifteen minutes
# and is not counted. No push and no issue waits for a review; the release of
# the phase does.
#
# STATE, `~/flow/reviewer` unless given, holds the log `loop.log`, the runs of
# the range being reviewed in `tries` as `P BASE HEAD N`, `until`, the
# worktree `wt`, the process group of the review that is running in
# `session`, the review whose findings are being filed in `pending`, the
# failure not yet on the phase in `failure`, and each review's issue texts
# and output as `r-P-TIME.issues`, `.jsonl`, `.err` and `.json`, with the
# findings filed in `.filed`, one `I N` a line for finding I filed as issue
# N, and `.commented` once the summary is on the phase.
# Removing `pending` abandons the filing. `touch STATE/stop` ends the loop
# after the review that is running, and a loop that starts while a review of
# the loop before it still runs waits for that review to end.

# shellcheck disable=SC2016 # the dollars are perl's
REPO=toksaitov/rach2
LEAD=gpt-6-astra
LEAD_EFFORT=xhigh
SUB=gpt-6.1-sol
SUB_EFFORT=high
SUBAGENTS=3
DEADLINE=10800
PERIOD=300
RUNS=3

# pick: print `P BASE HEAD` for the range to review next, or nothing; fail when
# the board cannot be read.
pick()
{
    report=$(sh "$state/run/board.sh" phase) || return 1
    printf '%s\n' "$report" | grep -qx 'ready: no: review' || return 0
    p=$(printf '%s\n' "$report" | sed -n 's/^phase: #\([0-9][0-9]*\) .*/\1/p')
    base=$(printf '%s\n' "$report" | sed -n 's/^review: [0-9a-f]*\.\.\([0-9a-f]\{40\}\)$/\1/p')
    [ -n "$base" ] || base=$(printf '%s\n' "$report" | sed -n 's/^base: \([0-9a-f]\{40\}\)$/\1/p')
    head=$(printf '%s\n' "$report" | sed -n 's/^head: \([0-9a-f]\{40\}\)$/\1/p')
    if [ -z "$p" ] || [ -z "$base" ] || [ -z "$head" ]; then
        log "board.sh phase gave no phase, base or head to review"
        return 0
    fi
    until=$(cat "$state/until" 2> /dev/null)
    if [ -n "$until" ] && [ "$until" != "$base" ] &&
        git merge-base --is-ancestor "$base" "$until" 2> /dev/null &&
        git merge-base --is-ancestor "$until" "$head" 2> /dev/null; then
        head=$until
    fi
    echo "$p $base $head"
}

# findings P COMMIT BASE: file each finding of BASE.json that BASE.filed does
# not list as a `bug` issue of phase P, and add it to BASE.filed. Fail when a
# finding could not be filed.
findings()
{
    answer=$3.json
    n=$(jq '.findings | length' "$answer") || return 1
    i=0 lost=0
    while [ "$i" -lt "$n" ]; do
        if grep -q "^$i " "$3.filed" 2> /dev/null; then
            i=$((i + 1))
            continue
        fi
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
            echo "$i ${url##*/}" >> "$3.filed"
        else
            log "#$1: could not file finding $i: $(get title)"
            lost=1
        fi
        i=$((i + 1))
    done
    return "$lost"
}

# get FIELD: print FIELD of finding i of the answer that `findings` reads.
get()
{
    jq -r ".findings[$i].$1" "$answer"
}

# finish: file the findings of the review that STATE/pending names and
# comment its summary on the phase, ending with the record of a complete
# review, or labelling the phase `review-failed` when the review says it left
# part of the range unread; then remove STATE/pending. Fail, and keep
# STATE/pending for the next try, when a step fails.
finish()
{
    read -r p from to took base < "$state/pending" || return 1
    findings "$p" "$(echo "$to" | cut -c 1-8)" "$base" || return 1
    complete=$(jq -r '.complete' "$base.json")
    if [ ! -e "$base.commented" ]; then
        filed=$(awk '{ printf "%s#%s", s, $2; s = " " }' "$base.filed" 2> /dev/null)
        if [ "$complete" = true ]; then
            record="review: $from..$to complete"
        else
            record="The review is incomplete; it did not read: $(jq -r .unreviewed "$base.json")"
        fi
        gh issue comment "$p" --repo "$REPO" --body "Review of the commits $from..$to of this phase by \`tools/flow/reviewer.sh\` with $agent ($LEAD at $LEAD_EFFORT effort, at most $SUBAGENTS subagents at a time of $SUB at $SUB_EFFORT), in $took seconds.

$(jq -r .summary "$base.json")

Filed: ${filed:-none}

$record" > /dev/null || return 1
        : > "$base.commented"
    fi
    if [ "$complete" != true ]; then
        gh issue edit "$p" --repo "$REPO" --add-label review-failed > /dev/null || return 1
    fi
    rm -f "$state/pending"
}

# give_up P WHY: record in STATE/failure that the review of phase P failed
# for WHY, and publish it.
give_up()
{
    echo "$1 $base $2" > "$state/failure"
    publish || log "#$1: could not label it review-failed; tried again before the next review"
}

# publish: say on the phase that STATE/failure names why its review failed,
# for the manager to diagnose, and give it the label `review-failed`; then
# remove STATE/failure. Fail, keeping it and what was done, when GitHub
# cannot be reached.
publish()
{
    read -r p out why < "$state/failure" || return 1
    if [ ! -e "$state/failure.commented" ]; then
        gh issue comment "$p" --repo "$REPO" --body "The review of this phase failed: $why. Its output is \`$out.*\` on the reviewer's machine. Remove the label \`review-failed\` to review the range again." > /dev/null ||
            return 1
        : > "$state/failure.commented"
    fi
    gh issue edit "$p" --repo "$REPO" --add-label review-failed > /dev/null || return 1
    rm -f "$state/failure" "$state/failure.commented"
}

# review P FROM TO: review the commits FROM..TO of phase P with the agent's
# adapter, file what it finds through STATE/pending and `finish`, and return
# 0; 75 when the account's usage limit ended it or GitHub could not be read;
# 124 when it reached its time limit; or 1 when it ended without an answer.
review()
{
    p=$1 from=$2 to=$3
    base=$state/r-$p-$(date -u '+%m%d-%H%M%S')
    git worktree remove --force "$state/wt" 2> /dev/null
    rm -rf "$state/wt"
    git worktree prune
    if ! git worktree add -q --detach "$state/wt" "$to" 2> /dev/null &&
        ! { git fetch -q origin && git worktree add -q --detach "$state/wt" "$to"; }; then
        log "#$p: no worktree at $to"
        return 75
    fi
    title=$(gh issue view "$p" --repo "$REPO" --json title --jq .title) &&
        m=$(sh "$state/run/board.sh" members "$p") &&
        started_at=$(gh api "repos/$REPO/issues/$p/comments" --paginate --jq '.[].body' |
            sed -n 's/^phase: started at \([0-9a-f]\{40\}\)$/\1/p' | head -n 1) || return 75
    if [ "$from" = "$started_at" ]; then
        kind="They are the phase's work since it started, and this is its first review."
    else
        kind="An earlier review read the phase up to $from; these commits are what landed since, the fixes of that review's findings among them, so check each fix and what it may have broken as well."
    fi
    issues=$(printf '%s\n' "$m" | awk -F '\t' 'NF { print "* #" $1 " " $5 }')
    for issue in "$p" $(printf '%s\n' "$m" | cut -f 1); do
        gh issue view "$issue" --repo "$REPO" --json number,title,body \
            --jq '"## #\(.number) \(.title)\n\n\(.body)\n"' || return 75
    done > "$base.issues"
    prompt=$(P=$p TITLE=$title FROM=$from TO=$to KIND=$kind ISSUES=$issues TEXTS=$base.issues perl -0pe '
        s/\@PHASE\@/$ENV{P}/g; s/\@TITLE\@/$ENV{TITLE}/g; s/\@BASE\@/$ENV{FROM}/g;
        s/\@HEAD\@/$ENV{TO}/g; s/\@KIND\@/$ENV{KIND}/g; s/\@ISSUES\@/$ENV{ISSUES}/g;
        s/\@TEXTS\@/$ENV{TEXTS}/g' "$state/run/reviewer.txt")
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
    [ "$status" -ne 124 ] || return 124
    if [ "$status" -ne 0 ] ||
        ! jq -e '.summary and (.complete | type == "boolean") and .findings' "$base.json" > /dev/null 2>&1; then
        log "#$p: the review ended with status $status and no answer in the form reviewer.json gives"
        return 1
    fi
    echo "$p $from $to $took $base" > "$state/pending"
    finish || log "#$p: filing incomplete; it is tried again before the next review"
    return 0
}

# runs P FROM TO: print how many times the range has been run since it was
# last reviewed or given up, which clears its count.
runs()
{
    n=$(awk -v r="$1 $2 $3" 'index($0, r " ") == 1 { print $4 }' "$state/tries" 2> /dev/null)
    echo "${n:-0}"
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
    wait_session
    if [ "$0" != "$state/run/reviewer.sh" ]; then
        refresh
        while update reviewer.sh 2>> "$state/loop.log"; [ $? -eq 2 ]; do
            log "update failed"
            sleep 60
        done
        log "loop starts: $agent in $repo"
        exec sh "$state/run/reviewer.sh" -a "$agent" -s "$state" -r "$repo"
    fi
    while [ ! -e "$state/stop" ]; do
        if ! refresh; then
            log "fetch failed"
            sleep 60
            continue
        fi
        update reviewer.sh 2>> "$state/loop.log"
        case $? in
            0)
                log "restarting as master's loop"
                exec sh "$state/run/reviewer.sh" -a "$agent" -s "$state" -r "$repo" ;;
            2)
                log "update failed"
                sleep 60
                continue ;;
        esac
        if [ -e "$state/failure" ] && ! publish; then
            log "the failure in $state/failure is not yet on the phase"
            sleep 300
            continue
        fi
        if [ -e "$state/pending" ] && ! finish; then
            log "filing of the review in $state/pending failed"
            sleep 300
            continue
        fi
        if ! range=$(pick); then
            log "board unreachable"
            sleep 300
            continue
        fi
        if [ -z "$range" ]; then
            sleep "$PERIOD"
            continue
        fi
        # shellcheck disable=SC2086 # the range is three words
        set -- $range
        run=$(($(runs "$@") + 1))
        log "review of #$1 at $2..$3 starts, run $run"
        review "$@"
        status=$?
        log "review of #$1 ended $status"
        case $status in
            0) run=0; [ "$3" != "$(cat "$state/until" 2> /dev/null)" ] || rm -f "$state/until" ;;
            75) sleep 900; continue ;;
            124)
                run=0
                give_up "$1" "it reached its limit of $DEADLINE seconds" ;;
            *)
                if [ "$run" -ge "$RUNS" ]; then
                    run=0
                    give_up "$1" "$RUNS runs ended without an answer in the form reviewer.json gives"
                fi ;;
        esac
        grep -v "^$1 $2 $3 " "$state/tries" > "$state/tries.new" 2> /dev/null
        [ "$run" -eq 0 ] || echo "$1 $2 $3 $run" >> "$state/tries.new"
        mv "$state/tries.new" "$state/tries"
    done
    log stopped
    exit 0
}

. "$(dirname "$0")/lib.sh"
main "$@"
