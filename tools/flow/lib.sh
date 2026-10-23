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
# that conflicts, even one whose tree is master's, as when REF deletes a file
# that master changed. A commit message proves nothing.
merged()
{
    master=$(git rev-parse -q --verify 'origin/master^{tree}') || return 1
    merge=$(git merge-tree --write-tree --no-messages origin/master "$1" 2> /dev/null) || return 1
    [ "$(printf '%s\n' "$merge" | sed -n 1p)" = "$master" ]
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

# The home directories on the testing host whose leftovers the manager's loop
# removes: the owner's account's, which has the copies that testing.sh makes,
# and those of the five accounts with worker loops.
HOMES='/home/testing /home/toksaitov-a /home/toksaitov-d /home/toksaitov-m /home/toksaitov-s /home/toksaitov-v'

# leftovers [-n]: run leftovers.pl, from the directory of the script that
# calls this, as root on the testing host for HOMES with their loose files,
# and here for the Desktop, which has the owner's checkout and the worktrees
# and copies made beside it. Each run gets the text of the open issues,
# master's commit from this clone, and the copies to keep: the default copy
# of testing.sh, the release's, each loop's that STATE/copy of a loop here
# names, and each home's `flow`. Print the lines of each run with its
# machine's name first; fail, and run neither, when master or the issues
# cannot be read. -n removes nothing and lists what would go.
leftovers()
{
    here=$(dirname "$0")
    master=$(git rev-parse -q --verify origin/master) ||
        { echo "leftovers: no origin/master in $(pwd)"; return 1; }
    text=$(gh issue list --repo toksaitov/rach2 --state open --limit 2000 \
        --json title,body,comments --jq '.[] | .title, .body, .comments[].body') ||
        { echo "leftovers: the open issues cannot be read"; return 1; }
    keep=
    # shellcheck disable=SC2046,SC2086 # the copies and HOMES are a path a word
    for c in /home/testing/rach2 "$(sed -n 's/^COPY=//p' "$here/release.sh")" \
        $(cat "$HOME"/flow/*/copy 2> /dev/null) $(printf '%s/flow ' $HOMES); do
        case $c in ''|/*[!A-Za-z0-9/._-]*|*/..*|[!/]*) continue ;; esac
        keep="$keep -k $c"
    done
    out=$({ cat "$here/leftovers.pl"; echo __END__; printf '%s\n' "$text"; } |
        perl -e 'alarm shift; exec @ARGV' 600 ssh -o BatchMode=yes -o ConnectTimeout=10 "$TESTING" \
        "MSYS_NO_PATHCONV=1 wsl.exe -u root -- perl - ${1:-} -l -m $master$keep 86400 $HOMES" 2>&1)
    rc=$?
    printf '%s\n' "$out" | sed "s/^/$TESTING: /"
    [ "$rc" -eq 0 ] || echo "$TESTING: leftovers: failed with status $rc"
    out=$({ cat "$here/leftovers.pl"; echo __END__; printf '%s\n' "$text"; } |
        perl -e 'alarm shift; exec @ARGV' 600 perl - ${1:+"$1"} -m "$master" \
        -r "$(git rev-parse --absolute-git-dir)" 86400 "$HOME/Desktop" 2>&1)
    rc=$?
    printf '%s\n' "$out" | sed "s/^/$(hostname -s): /"
    [ "$rc" -eq 0 ] || echo "$(hostname -s): leftovers: failed with status $rc"
    return 0
}

# The testing host's store of step outputs, and the days an entry of it that
# no copy's stamp names stays unused before store_expire removes it.
STORE=/var/cache/rach2-store
STORE_DAYS=3

# store_expire [DAYS]: remove from STORE on the testing host the entries that
# no copy's stamp names and that no step has used for DAYS days, STORE_DAYS
# unless given, with `etude-step -e` (tools/etude/Readme.md, The step store).
# An entry is read-only and its builder's, so root removes them, with the
# etude of origin/master, which version.sh builds from this clone into root's
# cache of versions on the host, or finds built there. Print the lines of
# etude-step with the machine's name first, and nothing when the host has no
# store.
store_expire()
{
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$TESTING" \
        "MSYS_NO_PATHCONV=1 wsl.exe -u root -- test -d $STORE" < /dev/null 2> /dev/null || return 0
    script=$(sh tools/etude/version.sh -r origin/master -s) ||
        { echo "$TESTING: store: etude's build script cannot be made"; return 1; }
    nonce=$(printf '%s\n' "$script" | sed -n 's/^ETUDE_NONCE=//p')
    out=$(printf '%s\n' "$script" | perl -e 'alarm shift; exec @ARGV' 600 \
        ssh -o BatchMode=yes -o ConnectTimeout=10 "$TESTING" \
        "MSYS_NO_PATHCONV=1 wsl.exe -u root -- sh -s" 2>&1)
    dir=$(printf '%s\n' "$out" | sed -n "s/^@etude-version $nonce [a-z]* [0-9a-f]* //p")
    case $dir in
        /*) ;;
        *)
            echo "$TESTING: store: etude was not built: $(printf '%s\n' "$out" | tail -n 1)"
            return 1 ;;
    esac
    perl -e 'alarm shift; exec @ARGV' 600 ssh -o BatchMode=yes -o ConnectTimeout=10 "$TESTING" \
        "MSYS_NO_PATHCONV=1 wsl.exe -u root -- $dir/bin/etude-step -s $STORE -e ${1:-$STORE_DAYS}" \
        < /dev/null 2>&1 | sed "s/^/$TESTING: /"
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
