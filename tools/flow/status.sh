#!/bin/sh
#
# status.sh: one look at the whole process, for the manager's check.
#
# Usage: status.sh
#        status.sh sizes
#
# It moves every closed card to Done with `board.sh done`, archives those of
# finished milestones with `board.sh archive`, then prints
# master's newest commit; the open milestones, with their branches,
# their counts of tasks and the tasks In progress with their assignees, as
# `board.sh milestones` gives them; a line for each worker loop that
# `workers` lists, and then for each other state directory under build/flow
# of this clone's main working tree that has a `loop.log`, as the manager's
# own loops have; what `board.sh check` finds wrong on the board, given
# the tasks the loops hold; the release; and the resources.
#
# A loop's line says whether the loop runs, has been asked to stop, or is
# stopped; what `session.pl -q`, run with this clone's session.pl on the
# loop's host, says of its session: running, ended, `lost GROUP` for one
# whose supervisor was killed and whose group may still run, or `unknown`;
# the task in its STATE/issue; whether its account is out of its
# usage limit, and since when in UTC, which holds while the last session
# that ended was followed by a `limit:` line of the log, counted from the
# first such line after the last session that did not end on the limit; the
# minutes since its newest session wrote its output; and its log's last line.
# A loop on another host is read over ssh; one whose host is `-` runs on this
# machine. The release's part is the last line of `build/flow/release/record`
# and whether its tmux session `rach2-release` runs on the default tmux
# server, whichever server the caller is in. The Lenovo's line says who holds
# the lock the `machine` skill describes, `build/flow/lenovo` of the main
# working tree, and since when in UTC, or that it is free.
#
# The resources are the free space of this machine's disk, of the testing
# host's WSL root and /tmp, whose tmpfs is the host's memory, and of its
# Windows drives C: and D:, which hold Windows and WSL's disk file; the free
# inodes of this machine's disk, the WSL root and /tmp; and the free memory
# of this machine, of Windows and of WSL, and WSL's free swap.
# The section ends with the verdict `resources: ok`, `resources: low: WHAT` or
# `resources: critical: WHAT`, WHAT naming each reading below its threshold:
# low is under 20% free, and critical under 10%, or under 40 GiB on a disk,
# the floor that tests/run keeps; the Mac's disk, which the owner lets fill
# to 5 GiB, is only critical, under 5 GiB. Memory is reported and not
# judged, since a suite takes it for minutes. A reading that is missing or
# malformed has a line `NAME KIND: did not answer`; the verdict is then
# `resources: unknown`, or a low or critical one that ends `; unknown: NAME
# KIND ... did not answer`.
#
# After the verdict comes the total of the run files that GitHub keeps for the
# repository, the artifacts of full.yml, with a warning above 1 GiB, since the
# owner's account must not be seen to abuse GitHub's storage.
#
# `sizes` prints the sizes of the build trees, the GNU caches, the ci checkouts,
# the loops' state, the copies under `build/worktrees` and /tmp on the testing
# host, and of the loops' state, the worktrees and the build tree of this
# clone's main working tree here, largest first, for the manager to choose what
# to remove when space is low. It then lists, through `leftovers -n` in lib.sh,
# the failed tests' directories, loose files and copies that `leftovers` would
# remove, and the copies and old entries it keeps, with why. It reads every
# file of those trees, which takes minutes, so the manager runs it only then.

# shellcheck disable=SC2016 # the dollars are the probe's
REPO=toksaitov/rach2
TESTING=testing
# shellcheck source=tools/flow/lib.sh
. "$(dirname "$0")/lib.sh"
here=$(primary "$(dirname "$0")")

# probe is the script that reads a loop's state directory, given as its
# argument, on the host the loop runs on, and prints the loop's line, after
# the function `query RECORD` that probe_script defines.
probe='
st=$1
case $st in "~"*) st=$HOME${st#"~"} ;; esac
if [ ! -d "$st" ]; then echo "no loop has run here: no $st"; exit 0; fi
# The loop holds STATE/loop.live for its life; a lock that cannot be probed
# reads as held.
if perl -MFcntl=:flock -e '"'"'open(L, "<", shift) or exit($!{ENOENT} ? 1 : 0);
        exit(flock(L, LOCK_SH | LOCK_NB) ? 1 : 0)'"'"' "$st/loop.live"; then
    loop=runs
    [ ! -e "$st/stop" ] || loop="runs, asked to stop"
else
    loop=stopped
fi
task=$(cat "$st/issue" 2> /dev/null)
limit=$(awk "
    / session [0-9-]+ ended / { if (open) { since = \"\" }; open = 1; next }
    / limit: / { if (since == \"\") since = \$1 \" \" \$2; open = 0 }
    END { if (since != \"\" && !open) print \"out of limit since \" since \" UTC\" }
" "$st/loop.log" 2> /dev/null)
cur=$(ls -t "$st"/s-*.jsonl "$st"/s-*.err 2> /dev/null | head -n 1)
silent=none
[ -z "$cur" ] || silent=$(perl -e "print int((time - (stat shift)[9]) / 60)" "$cur")m
echo "loop $loop | session $(query "$st/session") | task ${task:+#}${task:-none} |" \
    "${limit:-within limit} | silent $silent | $(tail -n 1 "$st/loop.log" 2> /dev/null)"
'

# readings: print one line for each resource the header names, as `NAME KIND
# FREE TOTAL`, KIND being disk, tmpfs, inodes or memory, and FREE and TOTAL
# in KiB, in inodes, or for this machine's memory in percent. A reading the
# host does not give is missing; `resources` finds it so.
readings()
{
    df -Pk "$HOME" | awk 'NR == 2 { print "mac:disk disk", $4, $2 }'
    df -Pi "$HOME" | awk 'NR == 2 { print "mac:disk inodes", $(NF - 2), $(NF - 3) + $(NF - 2) }'
    memory_pressure -Q 2> /dev/null |
        awk -F ': ' '/free percentage/ { sub(/%/, "", $2); print "mac:memory memory", $2, 100 }'
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$TESTING" 'df -Pk / /d' 2> /dev/null |
        awk 'NR > 1 { print "windows:" substr($1, 1, 1), "disk", $(NF - 2), $(NF - 4) }'
    # shellcheck disable=SC2016 # the dollars are PowerShell's and awk's
    printf '%s\n' \
        'df -Pk / /tmp | awk '"'"'NR > 1 { print "wsl:" $6, ($6 == "/tmp" ? "tmpfs" : "disk"), $4, $2 }'"'" \
        'df -Pi / /tmp | awk '"'"'NR > 1 { print "wsl:" $6, "inodes", $4, $2 }'"'" \
        'awk '"'"'{ m[$1] = $2 } END { print "wsl:memory memory", m["MemAvailable:"], m["MemTotal:"]; print "wsl:swap memory", m["SwapFree:"], m["SwapTotal:"] }'"'"' /proc/meminfo' \
        'cd / && timeout 60 powershell.exe -NoProfile -Command '"'"'$o = Get-CimInstance Win32_OperatingSystem; "$($o.FreePhysicalMemory) $($o.TotalVisibleMemorySize)"'"'"' | tr -d "\r" | awk '"'"'NF == 2 { print "windows:memory memory", $1, $2 }'"'" |
        ssh -o BatchMode=yes -o ConnectTimeout=10 "$TESTING" 'wsl.exe -u root -- sh -s' 2> /dev/null
}

# resources: print the readings in words and the verdict, as the header says.
resources()
{
    readings | awk '
        BEGIN {
            n = split("windows:C disk,windows:D disk,windows:memory memory,wsl:/ disk," \
                "wsl:/tmp tmpfs,wsl:/ inodes,wsl:/tmp inodes,wsl:memory memory,wsl:swap memory," \
                "mac:disk disk,mac:disk inodes,mac:memory memory", want, ",")
        }
        NF == 4 && $3 ~ /^[0-9]+$/ && $4 ~ /^[0-9]+$/ {
            seen[$1 " " $2] = 1
            pct = $4 > 0 ? int(100 * $3 / $4) : 0
            if ($2 == "inodes") printf "%s inodes: %d%% free\n", $1, pct
            else if ($1 == "mac:memory") printf "%s: %d%% free\n", $1, pct
            else printf "%s: %.0f GiB free of %.0f (%d%%)\n", $1, $3 / 1048576, $4 / 1048576, pct
            if ($2 == "memory") next
            if ($1 " " $2 == "mac:disk disk") {
                if ($3 < 5 * 1048576) critical = critical " " $1 " " $2
                next
            }
            if (pct < 10 || ($2 == "disk" && $3 < 40 * 1048576)) critical = critical " " $1 " " $2
            else if (pct < 20) low = low " " $1 " " $2
        }
        END {
            for (i = 1; i <= n; i++) {
                if (want[i] in seen) continue
                split(want[i], w, " ")
                printf "%s %s: did not answer\n", w[1], w[2]
                unknown = unknown " " w[1] " " w[2]
            }
            u = unknown != "" ? "; unknown:" unknown " did not answer" : ""
            if (critical != "") print "resources: critical:" critical u
            else if (low != "") print "resources: low:" low u
            else if (unknown != "") print "resources: unknown:" unknown " did not answer"
            else print "resources: ok"
        }'
}

# runfiles: print the total the header names, or that it cannot be read.
runfiles()
{
    if bytes=$(gh api --paginate "repos/$REPO/actions/artifacts?per_page=100" \
        --jq '.artifacts[] | select(.expired | not) | .size_in_bytes' 2> /dev/null); then
        printf '%s\n' "$bytes" | awk '
            { total += $1 }
            END {
                gib = total / 1073741824
                printf "github: %.2f GiB of run files", gib
                print (gib > 1 ? "; warning: above 1 GiB" : "")
            }'
    else
        echo "github: run files: did not answer"
    fi
}

# sizes: print the sizes the header names, in GiB, largest first.
sizes()
{
    echo "== sizes on the testing host, in GiB"
    # du counts a directory once, so nested trees have a du of their own.
    printf '%s\n' '{ du -sxk /home/*/rach2*/build /home/ghrunner/rach2-ci-*/_work /tmp
        du -sxk /home/*/rach2*/build/vendor/gnu /home/*/rach2*/build/x86_64/vendor/gnu
        du -sxk /home/*/rach2/build/flow /home/*/rach2/build/worktrees/*; } 2> /dev/null | sort -rn |
        awk '"'"'{ printf "%7.1f %s\n", $1 / 1048576, $2 }'"'" |
        ssh -o BatchMode=yes -o ConnectTimeout=10 "$TESTING" 'wsl.exe -u root -- sh -s' 2>&1
    echo "== sizes here, in GiB"
    du -sxk "$here"/build/flow/* "$here"/build/worktrees/* "$here"/build \
        "$here"/.claude/worktrees 2> /dev/null | sort -rn |
        awk '{ printf "%7.1f %s\n", $1 / 1048576, $2 }'
    echo "== leftovers that sweep.sh removes, and the old ones it keeps"
    leftovers -n
}


# probe_script: print probe after the function `query RECORD`, which runs
# this clone's session.pl -q on RECORD, the program sent with the script.
probe_script()
{
    echo "query() { perl - -q \"\$1\" << 'END_OF_SESSION'"
    cat "$(dirname "$0")/session.pl"
    echo 'END_OF_SESSION'
    echo '}'
    printf '%s' "$probe"
}

# loops: print a line for each loop, as the header says.
loops()
{
    grep -v '^#' "$(dirname "$0")/workers" | while read -r name host st; do
        [ -n "$name" ] || continue
        printf '%s: ' "$name"
        if [ "$host" = - ]; then
            case $st in /*|"~"*) ;; *) st=$here/$st ;; esac
            probe_script | sh -s "$st" 2>&1
        else
            probe_script | ssh -o BatchMode=yes -o ConnectTimeout=10 "$host" sh -s "$st" 2>&1
        fi
    done
    listed=$(grep -v '^#' "$(dirname "$0")/workers" |
        awk -v h="$here" '$2 == "-" { print ($3 ~ /^\// ? $3 : h "/" $3) }')
    for st in "$here"/build/flow/*; do
        [ -f "$st/loop.log" ] || continue
        printf '%s\n' "$listed" | grep -q -F -x "$st" && continue
        printf '%s: ' "${st##*/}"
        probe_script | sh -s "$st" 2>&1
    done
}


if [ "$1" = sizes ]; then
    sizes
    exit
fi

echo "== master $(git ls-remote "https://github.com/$REPO" refs/heads/master | cut -c 1-8)  $(date -u '+%F %T') UTC"
echo "== milestones"
sh "$(dirname "$0")/board.sh" 'done' > /dev/null 2>&1 || echo "status: board.sh done failed"
sh "$(dirname "$0")/board.sh" archive > /dev/null 2>&1 || echo "status: board.sh archive failed"
sh "$(dirname "$0")/board.sh" milestones 2>&1
echo "== workers"
workers=$(loops)
printf '%s\n' "$workers"
echo "== board"
# shellcheck disable=SC2046
sh "$(dirname "$0")/board.sh" check $(printf '%s\n' "$workers" | sed -n 's/.*| task #\([0-9]*\) |.*/\1/p') 2>&1
echo "== release"
if env -u TMUX tmux has-session -t rach2-release 2> /dev/null; then echo "release.sh runs"; else echo "release.sh is not running"; fi
r=$here/build/flow/release/record
if [ -f "$r" ]; then
    echo "record: $(tail -n 1 "$r")"
else
    echo "no release record"
fi
l=$here/build/flow/lenovo
if [ -d "$l" ]; then
    holder=$(cat "$l/issue" 2> /dev/null) || holder=
    case $holder in
        '') holder='?' ;;
        *[!0-9]*) ;;
        *) holder="#$holder" ;;
    esac
    echo "lenovo: held by $holder since $(date -u -r "$l" '+%Y-%m-%d %H:%M')"
else
    echo "lenovo: free"
fi
echo "== resources"
resources
runfiles
