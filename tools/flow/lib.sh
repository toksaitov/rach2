# shellcheck shell=sh disable=SC2154 # state and repo are the loop's
# lib.sh: what worker.sh and manager.sh share, read by both with `.`.
#
# Each loop has a state directory, STATE. The loop runs from a copy of
# `tools/flow` in STATE/run that `update` takes from `origin/master`, so the
# version on master is the one that runs, whatever branch the clone is on,
# and a loop restarts as master's version when master changes it. When the
# copy fails, STATE/run stays as it was, and the loop logs the failure and
# tries again a minute later. While a session runs, STATE/session has the id
# of its process group.

# log TEXT: append TEXT to STATE/loop.log with the time in UTC.
log()
{
    echo "$(date -u '+%F %T') $*" >> "$state/loop.log"
}

# update SCRIPT: put master's `tools/flow` in STATE/run, or this clone's
# when master has none. Return 0 when SCRIPT or this file changed, which the
# loop must restart to run; 1 when neither did; and 2 when a step failed or
# the new copy lacks SCRIPT or this file, which leaves STATE/run as it was.
# The new copy is built in STATE/new and replaces the old by a rename, so a
# shell that is reading the old one keeps reading the file it opened.
update()
{
    rm -rf "$state/new" "$state/new.tar"
    mkdir -p "$state/new" || return 2
    if git rev-parse -q --verify origin/master:tools/flow/lib.sh > /dev/null; then
        git archive -o "$state/new.tar" origin/master tools/flow &&
            tar -x -f "$state/new.tar" -C "$state/new" --strip-components 2
    else
        cp "$repo"/tools/flow/* "$state/new/"
    fi
    copied=$?
    rm -f "$state/new.tar"
    if [ "$copied" -ne 0 ] || [ ! -f "$state/new/$1" ] || [ ! -f "$state/new/lib.sh" ]; then
        rm -rf "$state/new"
        return 2
    fi
    if diff -r -q "$state/new" "$state/run" > /dev/null 2>&1; then
        rm -rf "$state/new"
        return 1
    fi
    restart=1
    cmp -s "$state/new/$1" "$state/run/$1" && cmp -s "$state/new/lib.sh" "$state/run/lib.sh" ||
        restart=0
    rm -rf "$state/old"
    if [ -d "$state/run" ] && ! mv "$state/run" "$state/old"; then
        rm -rf "$state/new"
        return 2
    fi
    if ! mv "$state/new" "$state/run"; then
        [ ! -d "$state/old" ] || mv "$state/old" "$state/run"
        return 2
    fi
    return "$restart"
}

# refresh: bring a clone with nothing uncommitted or unpushed to
# `origin/master`; leave a clone with work in it as it is.
refresh()
{
    git fetch -q origin || return 1
    if [ -z "$(git status --porcelain)" ] && [ -z "$(git rev-list origin/master..HEAD)" ]; then
        git switch -q master 2> /dev/null && git merge -q --ff-only origin/master
    fi
    return 0
}

# limited OUT ERR: succeed when the session whose output is OUT and whose
# errors are ERR ended on the account's usage limit, and print the start of
# its message. Only the last lines are read: the agent's final message
# is there, and earlier lines can quote the limit's text, as this file does.
limited()
{
    tail -n 2 "$1" "$2" 2> /dev/null | grep -o 'hit your [a-z]* limit[^"\\]*' | head -n 1 | grep .
}

# run_session OUT ERR LIMIT COMMAND...: run COMMAND with its output in OUT and
# its errors in ERR, in a process group of its own, and stop the whole group
# when it is still running after LIMIT seconds: TERM, then KILL half a minute
# later. Return COMMAND's status once it ends and `stop_group` has stopped
# what it left in its group, such as a suite it started in the background;
# or, when the limit stopped it, 124, after the KILL has gone to the whole
# group, so that a child that outlives the TERM, such as an emulator, does
# not outlive the call.
#
# STATE/session has the group's id while COMMAND runs. The watcher, the perl
# process that enforces the limit, has a group of its own too, so a hangup
# that ends the loop reaches neither: the session runs on under its limit,
# and `wait_session` keeps the next loop from starting another beside it. The
# watcher has no children, and it is stopped by its pid, since it may not have
# made its group yet.
run_session()
{
    out=$1 err=$2 limit=$3
    shift 3
    rm -f "$state/expired"
    perl -e 'setpgrp(0, 0); exec @ARGV or die "exec $ARGV[0]: $!\n"' "$@" \
        > "$out" 2> "$err" < /dev/null &
    pid=$!
    echo "$pid" > "$state/session"
    perl -e 'setpgrp(0, 0); sleep $ARGV[0]; kill 0, -$ARGV[1] or exit;
        open(F, ">", $ARGV[2]) and close(F); kill "TERM", -$ARGV[1];
        sleep 30; kill "KILL", -$ARGV[1]' "$limit" "$pid" "$state/expired" &
    watch=$!
    wait "$pid"
    status=$?
    [ -e "$state/expired" ] || kill -TERM "$watch" 2> /dev/null
    wait "$watch" 2> /dev/null
    if [ -e "$state/expired" ]; then
        kill -KILL -"$pid" 2> /dev/null
        status=124
    else
        stop_group "$pid"
    fi
    rm -f "$state/expired" "$state/session"
    return "$status"
}

# stop_group GROUP: stop the processes left in the process group GROUP after
# its leader ended, as the limit does: TERM, then KILL to those still there
# half a minute later. Log each process found: a session should leave none,
# and one it leaves, such as a `tests/run` that holds the clone's run lock,
# would keep the next session's suite from running.
stop_group()
{
    kill -0 -"$1" 2> /dev/null || return 0
    ps -A -o pgid= -o pid= -o args= | awk -v g="$1" '$1 == g { $1 = ""; print }' |
        while read -r left; do
            log "session group $1 left $left; stopping it"
        done
    kill -TERM -"$1" 2> /dev/null
    waited=0
    while kill -0 -"$1" 2> /dev/null && [ "$waited" -lt 30 ]; do
        sleep 1
        waited=$((waited + 1))
    done
    if kill -0 -"$1" 2> /dev/null; then
        log "session group $1 outlived the TERM; killing it"
        kill -KILL -"$1" 2> /dev/null
    fi
}

# wait_session: wait while the process group whose id is in STATE/session is
# alive, testing it every half minute, and then remove the file. A loop calls
# this before it does anything in the clone: a session that outlived the loop
# that started it, as one does when the loop's tmux session is killed, still
# has its group and the file, and the clone must not get a second session.
wait_session()
{
    group=$(cat "$state/session" 2> /dev/null)
    if [ -n "$group" ] && kill -0 -"$group" 2> /dev/null; then
        log "session group $group of an earlier loop still runs; waiting for it"
        while kill -0 -"$group" 2> /dev/null; do
            sleep 30
        done
        log "session group $group ended"
    fi
    rm -f "$state/session"
}
