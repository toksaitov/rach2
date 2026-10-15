# shellcheck shell=sh disable=SC2154 # state and repo are the loop's
# lib.sh: what worker.sh and manager.sh share, read by both with `.`.
#
# Each loop has a state directory, STATE. The loop runs from a copy of
# `tools/flow` in STATE/run that `update` takes from `origin/master`, so the
# version on master is the one that runs, whatever branch the clone is on,
# and a loop restarts as master's version when master changes it.

# log TEXT: append TEXT to STATE/loop.log with the time in UTC.
log()
{
    echo "$(date -u '+%F %T') $*" >> "$state/loop.log"
}

# update SCRIPT: put master's `tools/flow` in STATE/run, or this clone's
# when master has none, and succeed when SCRIPT or this file changed, which
# the loop must restart to run. The new copy replaces the old by a rename, so
# a shell that is reading the old one keeps reading the file it opened.
update()
{
    rm -rf "$state/new"
    mkdir -p "$state/new"
    if git rev-parse -q --verify origin/master:tools/flow/lib.sh > /dev/null; then
        git archive origin/master tools/flow | tar -x -f - -C "$state/new" --strip-components 2
    else
        cp "$repo"/tools/flow/* "$state/new/"
    fi
    if diff -r -q "$state/new" "$state/run" > /dev/null 2>&1; then
        rm -rf "$state/new"
        return 1
    fi
    restart=no
    cmp -s "$state/new/$1" "$state/run/$1" && cmp -s "$state/new/lib.sh" "$state/run/lib.sh" ||
        restart=yes
    rm -rf "$state/old"
    [ ! -d "$state/run" ] || mv "$state/run" "$state/old"
    mv "$state/new" "$state/run"
    [ "$restart" = yes ]
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

# run_session OUT ERR LIMIT COMMAND...: run COMMAND with its output in OUT and
# its errors in ERR, in a process group of its own, and stop the whole group
# when it is still running after LIMIT seconds: TERM, then KILL half a minute
# later. Return COMMAND's status, or 124 when the limit stopped it.
run_session()
{
    out=$1 err=$2 limit=$3
    shift 3
    rm -f "$state/expired"
    perl -e 'setpgrp(0, 0); exec @ARGV or die "exec $ARGV[0]: $!\n"' "$@" \
        > "$out" 2> "$err" < /dev/null &
    pid=$!
    perl -e 'setpgrp(0, 0); sleep $ARGV[0];
        kill "TERM", -$ARGV[1] and open(F, ">", $ARGV[2]) and close(F);
        sleep 30; kill "KILL", -$ARGV[1]' "$limit" "$pid" "$state/expired" &
    watch=$!
    wait "$pid"
    status=$?
    kill -TERM -"$watch" 2> /dev/null
    wait "$watch" 2> /dev/null
    if [ -e "$state/expired" ]; then
        rm -f "$state/expired"
        return 124
    fi
    return "$status"
}
