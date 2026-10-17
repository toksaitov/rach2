# shellcheck shell=sh disable=SC2154 # state and repo are the loop's
# lib.sh: what the flow's loops share, read by each with `.`.
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
# its message. Only the last two lines of each are read, since earlier lines
# can quote the limit's text, as this file does. In OUT the message counts
# only on a line that reports an error: Claude Code's result line with
# "is_error":true, or Codex's error or turn.failed event. A session that ends
# normally has a result line too, whose text is the agent's final message and
# may quote the limit; in JSON that text cannot contain an unescaped quote, so
# it cannot match these keys. Codex writes the message to ERR as plain text.
limited()
{
    { tail -n 2 "$1" |
        grep -E '"type":"result".*"is_error":true|"is_error":true.*"type":"result"|"type":"(error|turn\.failed)"'
        tail -n 2 "$2"; } 2> /dev/null | grep -o 'hit your [a-z]* limit[^"\\]*' | head -n 1 | grep .
}

# run_session OUT ERR LIMIT COMMAND...: run COMMAND with its output in OUT and
# its errors in ERR under `session.pl`, in a process group of its own that
# STATE/session records before COMMAND starts, and stop the whole group when
# it is still running after LIMIT seconds: TERM, then KILL half a minute
# later. Return COMMAND's status once it ends and what it left in its group,
# such as a suite it started in the background, has been stopped and logged;
# 124 when the limit stopped it; and 125 when the group could not be recorded,
# and COMMAND did not run.
#
# The supervisor that `session.pl` runs leaves the loop's group before it
# starts anything, so a hangup that ends the loop reaches neither it nor the
# session: the session runs on under its limit, and `wait_session` keeps the
# next loop from starting another beside it.
run_session()
{
    out=$1 err=$2 limit=$3
    shift 3
    perl "$(dirname "$0")/session.pl" "$state/session" "$state/loop.log" "$limit" "$@" \
        > "$out" 2> "$err" < /dev/null
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

# merged REF: succeed when `origin/master` has everything REF has: a merge of
# REF into it, which `git merge-tree` makes without touching a checkout, would
# leave its tree as it is. That holds for a branch master contains and for one
# rebased or cherry-picked before its push, and fails for any change master
# lacks, whitespace and a change in another place included, and for a merge
# that conflicts. A commit message proves nothing.
merged()
{
    master=$(git rev-parse -q --verify 'origin/master^{tree}') || return 1
    tree=$(git merge-tree --write-tree --no-messages origin/master "$1" 2> /dev/null | sed -n 1p)
    [ "$tree" = "$master" ]
}

# tidy: remove what finished work left in the clone. A helper's worktree under
# .claude/worktrees goes when its HEAD is merged and `git worktree remove` takes
# it without force, which it refuses for a worktree with changes, untracked
# files or a lock, as Claude Code keeps on a helper's worktree while the helper
# runs, and only when `disposable` finds nothing ignored in it but build output,
# which goes with it. A branch goes when it is merged, unless it is master, the
# branch checked out, one checked out in a worktree, which git refuses to
# delete, or one named for the issue in STATE/issue or STATE/parked. Each
# removal is logged.
tidy()
{
    keep=$(cat "$state/issue" "$state/parked" 2> /dev/null)
    top=$(git rev-parse --show-toplevel) || return 1
    git worktree list --porcelain | sed -n 's/^worktree //p' | while read -r w; do
        case $w in "$top"/.claude/worktrees/*) ;; *) continue ;; esac
        head=$(git -C "$w" rev-parse -q --verify HEAD) && merged "$head" && disposable "$w" &&
            git worktree remove "$w" 2> /dev/null && log "tidy: removed the worktree $w"
    done
    git worktree prune
    current=$(git symbolic-ref -q --short HEAD)
    git for-each-ref --format='%(refname:short)' refs/heads | while read -r b; do
        case $b in master|"$current") continue ;; esac
        for n in $keep; do
            case $b in "$n"-*) continue 2 ;; esac
        done
        merged "refs/heads/$b" && git branch -q -D "$b" 2> /dev/null &&
            log "tidy: deleted the branch $b"
    done
}

# disposable W: succeed when every ignored file of the worktree W is output
# that a build makes again: build/, the compilation database and clangd's
# cache, the vendored projects' trees and stamps (not the GNU downloads), and
# Finder's .DS_Store files. A key, a password, the Doom WAD or a person's
# notes keep the worktree.
disposable()
{
    ignored=$(git -C "$1" ls-files --others --ignored --exclude-standard --directory) || return 1
    ! printf '%s\n' "$ignored" |
        grep -v -E '^(build/|\.cache/|compile_commands\.json$|vendor/)|(^|/)\.DS_Store$' | grep -q . &&
        ! printf '%s\n' "$ignored" | grep -q '^vendor/gnu/downloads'
}

# stems REF: print, one to a line, the stems of the scratch names that the
# tests and tools of REF make and that begin with `rach2`, as no one else's
# files do: the part before `.XXXXXX` of each mktemp template, and before the
# last dot of each quoted prefix, as Python's tempfile takes. A stem with a
# shell variable in it is not listed.
stems()
{
    git grep -hoE "[/\"']rach2[A-Za-z0-9_-]*\.XXXXXX|[\"']rach2[A-Za-z0-9_-]*\.[\"']" "$1" -- tests tools |
        sed -E "s/^[/\"']//; s/\.(XXXXXX|[\"'])\$//" | sort -u
}

# prune_logs PATTERN...: remove the files of STATE that a PATTERN matches and
# that are more than fourteen days old, then the oldest of the rest while they
# add up to more than 1 GiB; a file written in the last day is kept whatever
# the total. A loop calls this between sessions, so the transcript of the
# session that runs is never among them.
prune_logs()
{
    n=$(perl -MFile::Glob=:bsd_glob -e '
        my ($dir, @pat) = @ARGV;
        my %seen;
        my @f = sort { $a->[1] <=> $b->[1] }
            map { [$_, (stat $_)[9, 7]] } grep { -f && !$seen{$_}++ }
            map { bsd_glob("$dir/$_") } @pat;
        my $total = 0;
        $total += $_->[2] for @f;
        my $n = 0;
        for (@f) {
            my $age = time - $_->[1];
            last if $age < 86400;
            next unless $age > 14 * 86400 || $total > 1 << 30;
            next unless unlink $_->[0];
            $total -= $_->[2];
            $n++;
        }
        print "$n\n";' "$state" "$@")
    [ "${n:-0}" -eq 0 ] || log "pruned $n logs"
}
