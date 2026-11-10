# shellcheck shell=sh disable=SC2154 # state and repo are the loop's
# lib.sh: what the flow's loops share, read by each with `.`.
#
# Each loop has a state directory, STATE. The loop runs from a copy of
# `tools/flow` in STATE/run that `update` takes from `origin/master`, so the
# version on master is the one that runs, whatever branch the clone is on,
# and a loop restarts as master's version when master changes it. When the
# copy fails, STATE/run stays as it was, and the loop logs the failure and
# tries again a minute later. While a session runs, STATE/session has the id
# of its process group. STATE is under `build/flow` of the clone's main
# working tree unless the loop is given another.

# primary REPO: print the main working tree of the clone REPO, the one whose
# `.git` the clone's worktrees share, or REPO itself when it is that tree.
primary()
{
    (cd "$1" && cd "$(git rev-parse --git-common-dir)/.." && pwd)
}

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

# refresh: fetch, and bring a clone with nothing uncommitted and no commit
# that origin lacks to `origin/master`, so that a finished task's branch is
# checked out nowhere and `tidy` can delete it; leave a clone with work in it
# as it is. A linked worktree cannot check out master while another working
# tree has it, so there the clone is detached at `origin/master` instead.
refresh()
{
    git fetch -q origin || return 1
    if [ -z "$(git status --porcelain)" ] &&
        [ -z "$(git rev-list -n 1 HEAD --not --remotes=origin --)" ]; then
        if git switch -q master 2> /dev/null; then
            git merge -q --ff-only origin/master
        else
            git switch -q --detach origin/master
        fi
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
# it is still running after LIMIT seconds or the supervisor gets TERM or INT:
# TERM, then KILL half a minute later. Return COMMAND's status once it ends
# and what it left in its group, such as a suite it started in the
# background, has been stopped and logged; 124 when the limit stopped it; 128
# plus the signal's number, once the group is gone, when a signal to the
# supervisor stopped it; and 125 when the group could not be recorded, and
# COMMAND did not run.
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

# landed REF: succeed when `origin/master` or a branch `origin/milestone/*`
# has the commit REF, as a milestone's branch has a task's once its worker has
# pushed it there by fast-forward. A commit message proves nothing.
landed()
{
    for r in $(git for-each-ref --format='%(refname)' refs/remotes/origin/master \
        refs/remotes/origin/milestone/); do
        git merge-base --is-ancestor "$1" "$r" 2> /dev/null && return 0
    done
    return 1
}

# tidy: remove what finished work left in the clone. A worktree under the
# clone's .claude/worktrees or build/worktrees goes when `closed_task` finds its
# task closed, since a rebased task's commit is not master's, after `salvage`
# has moved its run logs out, or, when it is not
# named for a task, when its HEAD has landed, which a task's new worktree has
# before its first commit while its session works in it from the clone's top; no
# process has its working directory in it, as a loop has in the worktree it runs
# in, and `git worktree remove` takes it without force, which it refuses for a
# worktree with changes, untracked files or a lock, as Claude Code keeps on a
# helper's worktree while the helper runs, and only when `disposable` finds
# nothing ignored in it but build output, which goes with it. A branch goes when
# it has landed, unless it is master or a branch checked out here or in a
# worktree, which git refuses to delete. Each removal is logged, and so is a
# finished worktree that stays, with the condition that keeps it.
tidy()
{
    top=$(git rev-parse --show-toplevel) || return 1
    busy=$(cwds)
    git worktree list --porcelain | sed -n 's/^worktree //p' | while read -r w; do
        case $w in "$top"/.claude/worktrees/*|"$top"/build/worktrees/*) ;; *) continue ;; esac
        head=$(git -C "$w" rev-parse -q --verify HEAD) || continue
        if [ -n "$(task_of "$w")" ]; then
            closed_task "$w" "$head" || continue
        else
            landed "$head" || continue
        fi
        if inside "$w" "$busy"; then
            log "tidy: kept: $w: a process has its working directory in it"
        elif [ -n "$(task_of "$w")" ] && ! salvage "$w"; then
            log "tidy: kept: $w: its run logs could not be moved out"
        elif ! disposable "$w"; then
            log "tidy: kept: $w: $why"
        elif err=$(git worktree remove "$w" 2>&1); then
            log "tidy: removed the worktree $w"
        else
            log "tidy: kept: $w: $(printf '%s\n' "$err" | head -1)"
        fi
    done
    git worktree prune
    git for-each-ref --format='%(refname:short)' refs/heads | while read -r b; do
        [ "$b" != master ] || continue
        if landed "refs/heads/$b" && git branch -q -D "$b" 2> /dev/null; then
            log "tidy: deleted the branch $b"
        fi
    done
}

# salvage W: move the run evidence of the closed task's worktree W out of
# it, so that the worktree and its images can go: each file under W's
# build/runs and build/flow smaller than 16 MiB, its logs and notes, goes to
# the same path under build/runs/task-N of the main working tree, where
# sweep.sh ages it out as any other log; the larger files, disk images and
# dumps, go with the worktree. Fail, and leave W whole, when a file cannot be
# copied.
salvage()
{
    n=$(task_of "$1")
    dest=$(primary "$1")/build/runs/task-$n
    (cd "$1/build" 2> /dev/null && find runs flow -type f -size -16384k 2> /dev/null) |
        while read -r f; do
            mkdir -p "$dest/$(dirname "$f")" && cp -p "$1/build/$f" "$dest/$f" || exit 1
        done || return 1
    rm -rf "$1/build/runs" "$1/build/flow"
}

# task_of W: print N when W is a worktree named N, tN or task-N, made for the
# task N.
task_of()
{
    basename "$1" | sed -n 's/^t\{0,1\}\(ask-\)\{0,1\}\([0-9][0-9]*\)$/\2/p'
}

# closed_task W HEAD: succeed when W, a worktree named N, tN or task-N, was made
# for the task N and that issue is closed. A detached W's commit HEAD gets the
# branch done/N, so that removing W leaves it on a branch.
closed_task()
{
    n=$(task_of "$1")
    [ -n "$n" ] || return 1
    [ "$(gh issue view "$n" --repo toksaitov/rach2 --json state --jq .state 2> /dev/null)" = CLOSED ] ||
        return 1
    git -C "$1" symbolic-ref -q HEAD > /dev/null || git branch -f "done/$n" "$2"
}

# disposable W: succeed when every ignored file of the worktree W is output
# that a build makes again: build/ but for build/runs and build/flow, which
# hold run evidence and loop state, the compilation database and clangd's
# cache, the vendored projects' trees and stamps (not the GNU downloads), the
# krffs files that make writes from the committed key pairs, Python's caches
# Finder's .DS_Store files, and a Doom WAD or GNU downloads equal to the main
# working tree's. Another key, a password, any other WAD, a person's notes or
# any file under build/runs or build/flow keeps the worktree. On failure, `why`
# names what keeps it.
disposable()
{
    why="git cannot list its ignored files"
    ignored=$(git -C "$1" ls-files --others --ignored --exclude-standard --directory) || return 1
    main=$(primary "$1")
    wad=ports/doom/doom1.wad
    if cmp -s "$1/$wad" "$main/$wad"; then
        ignored=$(printf '%s\n' "$ignored" | grep -v -x "$wad")
    fi
    if downloaded "$1" "$main"; then
        ignored=$(printf '%s\n' "$ignored" | grep -v '^vendor/gnu/downloads')
    fi
    why="ignored files that are not build output"
    ! printf '%s\n' "$ignored" |
        grep -v -E '^(build/|\.cache/|compile_commands\.json$|vendor/)|(^|/)(__pycache__/|\.DS_Store$)|^\.claude/scheduled_tasks\.lock$|^krffs/(authorized_keys|recovery_authorized_keys|test_authorized_keys|shadow|rach2_(demo|test)_password)$' |
        grep -q . || return 1
    why="GNU downloads that the main working tree lacks"
    ! printf '%s\n' "$ignored" | grep -q '^vendor/gnu/downloads' || return 1
    why="files under build/runs or build/flow"
    ! find "$1/build/runs" "$1/build/flow" ! -type d 2> /dev/null | grep -q .
}

# downloaded W MAIN: succeed when MAIN has each file of the GNU downloads of W.
downloaded()
{
    (cd "$1/vendor/gnu/downloads" 2> /dev/null && find . -type f) | while read -r f; do
        cmp -s "$1/vendor/gnu/downloads/$f" "$2/vendor/gnu/downloads/$f" || exit 1
    done
}

# cwds: print the working directory of each process that this user may read,
# one to a line, from /proc where there is one and from lsof(8) elsewhere, as
# on macOS.
cwds()
{
    if [ -d /proc/self ]; then
        for p in /proc/[0-9]*/cwd; do
            readlink "$p" 2> /dev/null
        done
    else
        lsof -w -n -a -d cwd -Fn 2> /dev/null | sed -n 's/^n//p'
    fi
}

# inside DIR LIST: succeed when a line of LIST, a path as `cwds` prints it, is
# DIR or under it, or when DIR cannot be resolved.
inside()
{
    printf '%s\n' "$2" | D=$(cd "$1" 2> /dev/null && pwd -P) \
        awk 'index($0 "/", ENVIRON["D"] "/") == 1 { f = 1 } END { exit !f }'
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

# The home directories on the testing host whose leftovers `leftovers`
# removes: the owner's account's, which has the copies that testing.sh makes,
# and those of the five accounts with worker loops.
HOMES='/home/testing /home/toksaitov-a /home/toksaitov-d /home/toksaitov-m /home/toksaitov-s /home/toksaitov-v'

# leftovers [-n]: run leftovers.pl, from the directory of the script that
# calls this, as root on the testing host for HOMES with their loose files,
# and here for the Desktop, which has the owner's checkout and the worktrees
# and copies made beside it. Each run gets the text of the open issues,
# master's commit from this clone, and the copies to keep: the default copy
# of testing.sh, the release's and its preparation's, and each loop's that the
# file `copy` of a state directory under build/flow of this clone's main
# working tree names, unless STATE/stop says that the loop has ended.
# Print the lines of each run with its machine's name first; fail, and run
# neither, when master or the issues cannot be read. -n removes nothing and
# lists what would go.
leftovers()
{
    here=$(dirname "$0")
    master=$(git rev-parse -q --verify origin/master) ||
        { echo "leftovers: no origin/master in $(pwd)"; return 1; }
    text=$(gh issue list --repo toksaitov/rach2 --state open --limit 2000 \
        --json title,body,comments --jq '.[] | .title, .body, .comments[].body') ||
        { echo "leftovers: the open issues cannot be read"; return 1; }
    keep=
    # shellcheck disable=SC2016,SC2046 # sed's dollar; a copy is a word each
    for c in /home/testing/rach2 $(sed -n 's/^COPY=\$OWNER/\/home\/testing\/rach2/p
        s/^PREPARE_COPY=\$OWNER/\/home\/testing\/rach2/p' "$here/release.sh") \
        $(for f in "$(primary .)"/build/flow/*/copy; do
            [ -e "${f%/copy}/stop" ] || cat "$f" 2> /dev/null
        done); do
        case $c in ''|/*[!A-Za-z0-9/._-]*|*/..*|[!/]*) continue ;; esac
        keep="$keep -k $c"
    done
    out=$({ cat "$here/leftovers.pl"; echo __END__; printf '%s\n' "$text"; } |
        perl -e 'alarm shift; exec @ARGV' 600 ssh -o BatchMode=yes -o ConnectTimeout=10 "$TESTING" \
        "MSYS_NO_PATHCONV=1 wsl.exe -u root -- perl - ${1:-} -l -m $master$keep ${age:-86400} $HOMES" 2>&1)
    rc=$?
    printf '%s\n' "$out" | sed "s/^/$TESTING: /"
    [ "$rc" -eq 0 ] || echo "$TESTING: leftovers: failed with status $rc"
    out=$({ cat "$here/leftovers.pl"; echo __END__; printf '%s\n' "$text"; } |
        perl -e 'alarm shift; exec @ARGV' 600 perl - ${1:+"$1"} -m "$master" \
        -r "$(git rev-parse --absolute-git-dir)" "${age:-86400}" "$HOME/Desktop" 2>&1)
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
# unless given, with `etude build -e` (tools/etude/Readme.md, The step store).
# An entry is read-only and its builder's, so root removes them, with the
# etude of origin/master, which provision_version.sh builds from this clone
# into root's cache of versions on the host, or finds built there; it runs
# with sh, since this clone builds no etude of its own. Print the lines of
# `etude build` with the machine's name first, and nothing when the host has
# no store.
store_expire()
{
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$TESTING" \
        "MSYS_NO_PATHCONV=1 wsl.exe -u root -- test -d $STORE" < /dev/null 2> /dev/null || return 0
    script=$(sh tools/etude/provision_version.sh -r origin/master -s) ||
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
        "MSYS_NO_PATHCONV=1 wsl.exe -u root -- $dir/bin/etude build -s $STORE -e ${1:-$STORE_DAYS}" \
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
