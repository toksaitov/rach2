# shellcheck shell=bash
# opencode-run: OpenCode plays the exchange of tests/system/fakeapi/opencode.json
# with `opencode run --standalone --auto`, as the test account, in a fresh
# home and a clone of the shared fixture, with FFF, its file finder, on.
# Asked to, it reads notes.txt, searches the tree with its grep tool, which
# runs ripgrep, runs check.sh, edits greeting.txt with its patch tool, and
# runs check.sh again and commits; the fake server checks each tool result
# before it answers. OpenCode reaches the server as the provider openai,
# whose address and dummy key the home's opencode.json gives, and trusts the
# test authority by NODE_EXTRA_CA_CERTS. It opens a WebSocket first, which
# the server refuses with 426, and goes on over HTTPS. The session's title is
# given, so that it asks the model for none, and its list of models is not
# fetched from models.dev. Every other name OpenCode reaches as shipped is
# the fake server's too, so a request to any of them that the script does not
# have fails the row. The row's script has the last answer held, and while
# OpenCode waits for it the row requires the client and its private server
# each to run the program with nothing preloaded: they map no file but the
# program, glibc, the C++ runtime, the database's shared memory and the
# libraries Bun writes out of the program into TMPDIR. Once the session has
# ended, no process of the program may be left.
# The row returns in /tmp/agents/results/opencode-run.tar the repository,
# the program's digest before and after the session, the free frames sampled
# each second and the time of each step; the case judges them on the host
# with every recorded request.
# hosts: fakeapi.test api.openai.com models.dev opencode.ai
# script: fakeapi/opencode.json
# package: opencode

PROMPT="Read notes.txt and do what it says. Check the change with check.sh, then commit it."

# session_start: make the session's home, scratch and the fixture's
# repository in SESSION, WORK/session, with OpenCode's settings for the fake
# server, record the program's digest and the start, and sample the free
# frames until session_end.
session_start() {
    SESSION=$WORK/session
    mkdir -p "$SESSION/home/.config/opencode" "$SESSION/tmp" "$SESSION/repo" /tmp/agents/results
    printf '[user]\n\tname = OpenCode Test\n\temail = opencode@fakeapi.test\n' > "$SESSION/home/.gitconfig"
    cat > "$SESSION/home/.config/opencode/opencode.json" <<'EOF'
{
  "$schema": "https://opencode.ai/config.json",
  "model": "openai/gpt-5.5",
  "provider": {
    "openai": {
      "options": {"baseURL": "https://fakeapi.test/v1", "apiKey": "sk-fakeapi-dummy"}
    }
  }
}
EOF
    cp /tmp/agents/fixture/* "$SESSION/repo/"
    cd "$SESSION/repo" || { fail "the session's repository"; return 1; }
    output "git init" env HOME="$SESSION/home" git init -q .
    output "git add" git add .
    output "the fixture's commit" env HOME="$SESSION/home" git commit -qm "The fixture"
    printf '%s\n' run > "$SESSION/mode"
    output "the program's digest before the session" sha256sum /opt/opencode/bin/opencode
    printf '%s\n' "$OUT" > "$SESSION/binary.before"
    : > "$SESSION/steps"
    sample_memory > "$SESSION/memory" &
    SAMPLER=$!
    BEGAN=$(date +%s)
}

# step WHAT: record the seconds since session_start at WHAT.
step() {
    printf '%s %s\n' "$(($(date +%s) - BEGAN))" "$1" >> "$SESSION/steps"
}

# session_end: stop sampling, record the session's length and the program's
# digest, and archive what the host verifies.
session_end() {
    printf '%s\n' "$(($(date +%s) - BEGAN))" > "$SESSION/seconds"
    kill "$SAMPLER"
    wait "$SAMPLER" 2> /dev/null
    SAMPLER=
    output "the program's digest after the session" sha256sum /opt/opencode/bin/opencode
    printf '%s\n' "$OUT" > "$SESSION/binary.after"
    output "the session's archive" tar -cf "/tmp/agents/results/$ROW.tar" -C "$SESSION" \
        repo mode binary.before binary.after memory seconds steps
}

# session_env: print the environment of the session, a variable to a line.
session_env() {
    printf '%s\n' PATH=/usr/bin:/bin "HOME=$SESSION/home" LANG=C.UTF-8 TERM=dumb \
        "TMPDIR=$SESSION/tmp" "NODE_EXTRA_CA_CERTS=$CA" OPENCODE_DISABLE_AUTOUPDATE=1 \
        OPENCODE_DISABLE_MODELS_FETCH=1
}

# left: print, a process to a line, each process of the test account that
# runs /opt/opencode/bin/opencode.
left() {
    local dir
    for dir in /proc/[0-9]*; do
        [ "$(readlink "$dir/exe" 2> /dev/null)" = /opt/opencode/bin/opencode ] || continue
        printf '%s %s\n' "${dir#/proc/}" "$(tr '\0' ' ' < "$dir/cmdline" 2> /dev/null)"
    done
}

# server PID: print the private server the client PID started, the process
# whose parent is PID and whose arguments are serve --stdio, or nothing.
server() {
    local dir
    for dir in /proc/[0-9]*; do
        [ "$(awk '{ print $4 }' "$dir/stat" 2> /dev/null)" = "$1" ] || continue
        case $(tr '\0' ' ' < "$dir/cmdline" 2> /dev/null) in
            *" serve --stdio "*)
                echo "${dir#/proc/}"
                return
                ;;
        esac
    done
}

# maps WHAT PID: require the process PID to run the program with nothing
# preloaded, mapping no file but those the head of this file names.
maps() {
    unmodified "$1" "$2" /opt/opencode/bin/opencode \
        /lib64/ld-linux-x86-64.so.2 /lib/libc.so.6 /lib/libm.so.6 /lib/libpthread.so.0 \
        /lib/libdl.so.2 /lib/librt.so.1 /lib/libgcc_s.so.1 "/usr/lib/libstdc++.so.6*" \
        "$SESSION/tmp/.bun-$(id -u)-*" "$SESSION/home/.local/share/opencode/opencode.db-shm"
}

# Stop the sampler and what OpenCode left on every exit path.
row_cleanup() {
    if [ -n "${SAMPLER:-}" ]; then
        kill "$SAMPLER" 2> /dev/null
        wait "$SAMPLER" 2> /dev/null
        SAMPLER=
    fi
    left | awk '{ print $1 }' | xargs -r kill -9 2> /dev/null
}

row() {
    local pid status env private
    session_start || return
    mapfile -t env < <(session_env)
    env -i "${env[@]}" opencode run --standalone --auto --title "Greet Rach2" "$PROMPT" \
        < /dev/null > "$SESSION/client.log" 2>&1 &
    pid=$!
    if held "opencode run" "$pid"; then
        step "the last answer held"
        maps "opencode run" "$pid"
        private=$(server "$pid")
        if [ -n "$private" ]; then
            maps "opencode serve --stdio" "$private"
        else
            fail "opencode run: no private server"
        fi
        release "$pid"
    fi
    wait "$pid"
    status=$?
    step "the session ended"
    [ "$status" = 0 ] ||
        fail "opencode run: status $status: $(tail -n 3 "$SESSION/client.log" | tr '\n' ' ')"
    has "opencode run" "$(cat "$SESSION/client.log")" "the change is committed."
    exited "opencode run: the processes left" left
    session_end
}
