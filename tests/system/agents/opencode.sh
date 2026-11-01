# shellcheck shell=bash
# opencode: the version its recipe pins, the program the path finds and its
# license beside it, and ripgrep, which its search tools run, on the path.
# Then OpenCode's server, `opencode serve`, in a home and a TMPDIR of its own:
# it maps no file but the program, glibc, the C++ runtime, its database's
# shared memory and the three libraries Bun writes out of the program into
# TMPDIR at its start, each of them byte for byte the slice of the pinned
# program that the table below gives. Two servers share the home and its
# SQLite database and search a git tree with FFF, the file finder, at once;
# one is killed, and the other and a third started after it still search the
# tree, while none keeps an LMDB, which FFF opens only when given a path for
# one. A server started in a tree of more directories than the account may
# watch, fs.inotify.max_user_watches, still searches it, and once it has
# ended the account holds no watch: a new instance adds as many as the limit
# allows. Last, the client's and the private server's memory and time to
# start are sampled, `--standalone` leaves no process, and a command run
# without it leaves the background service, one process, which
# `opencode service stop` ends. The row prints what it sampled; nothing of
# it is a pass criterion.

# The libraries OpenCode 2.2.5 writes into TMPDIR as .bun-UID-NAME: NAME, its
# length and its offset in /opt/opencode/bin/opencode. 99cf65a7ec1d243c.so is
# libopentui, which draws the screen, 89812e6e6a68aaeb.so is FFF and
# 71b650bef1b3e52c.node the file watcher.
LIBRARIES="99cf65a7ec1d243c.so 5210768 185231116
89812e6e6a68aaeb.so 12846400 218596087
71b650bef1b3e52c.node 514960 216199491"

# oc HOME TMP ARGUMENT...: run opencode with ARGUMENTs as the test account
# would, in the home HOME and the scratch TMP, with the update check and the
# fetch of models.dev's list off and nothing on its standard input.
oc() {
    local home=$1 tmp=$2
    shift 2
    env -i PATH=/usr/bin:/bin HOME="$home" TMPDIR="$tmp" LANG=C.UTF-8 TERM=dumb \
        OPENCODE_DISABLE_AUTOUPDATE=1 OPENCODE_DISABLE_MODELS_FETCH=1 \
        opencode "$@" < /dev/null
}

# left: print, a process to a line, each process of the test account that
# runs /opt/opencode/bin/opencode, with its arguments.
left() {
    local dir
    for dir in /proc/[0-9]*; do
        [ "$(readlink "$dir/exe" 2> /dev/null)" = /opt/opencode/bin/opencode ] || continue
        printf '%s %s\n' "${dir#/proc/}" "$(tr '\0' ' ' < "$dir/cmdline" 2> /dev/null)"
    done
}

# rss PID: print the resident memory of the process PID in KiB, or nothing.
rss() {
    awk '$1 == "VmRSS:" { print $2 }' "/proc/$1/status" 2> /dev/null
}

# child PID: print the first process whose parent is PID, or nothing.
child() {
    local dir
    for dir in /proc/[0-9]*; do
        [ "$(awk '{ print $4 }' "$dir/stat" 2> /dev/null)" = "$1" ] || continue
        echo "${dir#/proc/}"
        return
    done
}

# serve NAME DIR: start `opencode serve` on a free port of 127.0.0.1 in DIR,
# with the home and scratch of the row, and wait until it listens, looking
# once a second. Set NAME_PID, NAME_URL and NAME_PASSWORD, and NAME_START
# to the seconds it took; return 1 when it ended first.
serve() {
    local name=$1 dir=$2 pid began log=$WORK/$1.log url
    began=$(date +%s)
    (cd "$dir" && exec env -i PATH=/usr/bin:/bin HOME="$WORK/home" TMPDIR="$WORK/tmp" \
        LANG=C.UTF-8 TERM=dumb OPENCODE_DISABLE_AUTOUPDATE=1 OPENCODE_DISABLE_MODELS_FETCH=1 \
        opencode serve --port 0 --hostname 127.0.0.1 < /dev/null > "$log" 2>&1) &
    pid=$!
    while ! url=$(sed -n 's/^server listening on //p' "$log") || [ -z "$url" ]; do
        if ! kill -0 "$pid" 2> /dev/null; then
            fail "opencode serve $name: ended: $(tail -n 3 "$log" | tr '\n' ' ')"
            return 1
        fi
        # interval: the server's log is read every second until it listens
        sleep 1
    done
    printf -v "${name}_PID" %s "$pid"
    printf -v "${name}_URL" %s "$url"
    printf -v "${name}_PASSWORD" %s "$(sed -n 's/^server password //p' "$log")"
    printf -v "${name}_START" %s "$(($(date +%s) - began))"
}

# find_in NAME DIR QUERY PATH: require the server NAME to find PATH, and
# only PATH, for QUERY in DIR.
find_in() {
    local url password
    url=${1}_URL password=${1}_PASSWORD
    output "the search of $1" curl -sS -u "opencode:${!password}" --get \
        --data-urlencode "query=$3" --data-urlencode "directory=$2" "${!url}/api/fs/find"
    has "the search of $1" "$OUT" "\"data\":[{\"path\":\"$4\",\"type\":\"file\"}]"
}

# libraries PID: require the process PID to map no file but the program,
# glibc, the C++ runtime, its database's shared memory and the three
# libraries in TMPDIR, and each of these to be its slice of the program.
libraries() {
    local name length offset file found=0
    unmodified "opencode serve" "$1" /opt/opencode/bin/opencode \
        /lib64/ld-linux-x86-64.so.2 /lib/libc.so.6 /lib/libm.so.6 /lib/libpthread.so.0 \
        /lib/libdl.so.2 /lib/librt.so.1 /lib/libgcc_s.so.1 /usr/lib/libstdc++.so.6 \
        "$WORK/tmp/.bun-$(id -u)-*" "$WORK/home/.local/share/opencode/opencode.db-shm"
    while read -r name length offset; do
        file=$WORK/tmp/.bun-$(id -u)-$name
        grep -q " $file\$" "/proc/$1/maps" || fail "opencode serve: maps no $file"
        same "the size of $file" "$(stat -c %s "$file" 2>&1)" "$length"
        cmp -s -n "$length" -i "0:$offset" "$file" /opt/opencode/bin/opencode ||
            fail "$file is not the program's $length bytes at $offset"
    done <<< "$LIBRARIES"
    for file in "$WORK"/tmp/.bun-*; do
        found=$((found + 1))
    done
    same "the files Bun wrote into TMPDIR" "$found" 3
}

# stop NAME: end the server NAME with SIGTERM and wait for it.
stop() {
    local pid=${1}_PID
    kill "${!pid}" 2> /dev/null
    wait "${!pid}" 2> /dev/null
}

# Stop every OpenCode process of the account on every exit path.
row_cleanup() {
    left | awk '{ print $1 }' | xargs -r kill -9 2> /dev/null
}

row() {
    local limit status pid server i peak_client=0 peak_server=0 began now got

    mkdir -p "$WORK/home" "$WORK/tmp" "$WORK/tree"
    output "opencode --version" oc "$WORK/home" "$WORK/tmp" --version
    same "the version" "$OUT" "opencode v$VERSION"
    same "the program" "$(readlink -f "$(type -P opencode)")" "/opt/opencode/bin/opencode"
    has "the license" "$(head -n 1 /opt/opencode/LICENSE)" "MIT License"
    output "rg --version" rg --version
    has "rg --version" "$OUT" "ripgrep 15.2.0"
    same "rg" "$(readlink -f "$(type -P rg)")" "/opt/ripgrep/rg"

    printf 'hello, world\n' > "$WORK/tree/greeting.txt"
    printf 'Change the greeting in greeting.txt to: hello, rach2\n' > "$WORK/tree/notes.txt"
    printf '#!/bin/sh\ngrep -qx "hello, rach2" greeting.txt\n' > "$WORK/tree/check.sh"
    output "the tree's repository" git -C "$WORK/tree" init -q
    output "the tree's files" git -C "$WORK/tree" add -A
    output "the tree's commit" git -C "$WORK/tree" -c user.name=rach2 \
        -c user.email=test@rach2.invalid commit -qm tree
    serve A "$WORK/tree" && serve B "$WORK/tree" || return
    echo "opencode: serve: listening after $A_START s, then $B_START s beside it;" \
        "VmRSS $(rss "$A_PID") and $(rss "$B_PID") KiB"
    libraries "$A_PID"
    find_in A "$WORK/tree" greet greeting.txt
    find_in B "$WORK/tree" notes notes.txt
    kill -9 "$A_PID"
    wait "$A_PID" 2> /dev/null
    find_in B "$WORK/tree" check check.sh
    serve C "$WORK/tree" || return
    find_in C "$WORK/tree" greet greeting.txt
    echo "opencode: serve: listening after $C_START s once one sharing its database was killed"
    stop B
    stop C
    same "an LMDB" "$(find "$WORK" -name '*.mdb' | tr '\n' ' ')" ""

    limit=$(cat /proc/sys/fs/inotify/max_user_watches)
    for i in $(seq 0 $((limit / 80))); do
        mkdir -p "$WORK"/big/d$i/e{0..79} && touch "$WORK"/big/d$i/e{0..79}/f ||
            { fail "the tree of $limit directories"; return; }
    done
    echo needle > "$WORK/big/d0/e0/needle.txt"
    output "the big tree's repository" git -C "$WORK/big" init -q
    serve D "$WORK/big" || return
    find_in D "$WORK/big" needle d0/e0/needle.txt
    echo "opencode: serve: listening after $D_START s in a tree of more than $limit directories"
    stop D
    exited "the servers" left
    # shellcheck disable=SC2016 # perl expands its own variables
    output "the watches left" perl -e '
        ($fd = syscall(294, 0)) >= 0 or die "inotify_init1: $!\n";
        $n = 0;
        for $d (glob("$ARGV[0]/d*/e*")) {
            syscall(254, $fd, $d, 0x100) >= 0 or do { print "$n $!\n"; exit };
            $n++;
        }
        print "$n none\n";' "$WORK/big"
    same "the watches the account may add once the servers have ended" "$OUT" \
        "$limit No space left on device"

    # The client and its private server, sampled twice a second.
    began=$(date +%s)
    (cd "$WORK/tree" && exec env -i PATH=/usr/bin:/bin HOME="$WORK/home" TMPDIR="$WORK/tmp" \
        LANG=C.UTF-8 TERM=dumb OPENCODE_DISABLE_AUTOUPDATE=1 OPENCODE_DISABLE_MODELS_FETCH=1 \
        opencode models --standalone < /dev/null > "$WORK/models.log" 2>&1) &
    pid=$!
    server=
    while kill -0 "$pid" 2> /dev/null; do
        got=$(rss "$pid")
        [ -z "$got" ] || [ "$got" -le "$peak_client" ] || peak_client=$got
        [ -n "$server" ] || { server=$(child "$pid") && [ -n "$server" ] && now=$(date +%s); }
        got=${server:+$(rss "$server")}
        [ -z "$got" ] || [ "$got" -le "$peak_server" ] || peak_server=$got
        # interval: the memory is sampled twice a second while the client runs
        sleep 0.5
    done
    wait "$pid"
    status=$?
    same "opencode models --standalone: status" "$status" 0
    has "opencode models --standalone" "$(cat "$WORK/models.log")" "openai/"
    echo "opencode: models --standalone: $(($(date +%s) - began)) s, its server started" \
        "after ${now:+$((now - began))} s; VmRSS at most $peak_client KiB," \
        "and $peak_server KiB in its server"
    exited "opencode models --standalone: the processes left" left

    status "opencode models" 0 oc "$WORK/home" "$WORK/tmp" models
    has "opencode models" "$OUT" "openai/"
    got=$(left)
    same "the processes left by opencode models" "$(printf '%s\n' "$got" | wc -l)" 1
    has "the background service" "$got" " serve --service"
    has "the service's record" "$(cat "$WORK/home/.local/state/opencode/service.json" 2>&1)" \
        "\"pid\":${got%% *},"
    echo "opencode: service: VmRSS $(rss "${got%% *}") KiB"
    output "opencode service stop" oc "$WORK/home" "$WORK/tmp" service stop
    exited "opencode service stop: the processes left" left
    [ ! -e "$WORK/home/.local/state/opencode/service.json" ] ||
        fail "opencode service stop left its record"
}
