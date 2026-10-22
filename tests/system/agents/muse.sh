# shellcheck shell=bash
# muse: the version its recipe pins, the program the path finds and the
# terms beside it, `muse --help`, and its screen in tmux, in a home of its
# own, up to its sign-in: it asks whether to trust the directory it starts
# in, records the answer in ~/.config/muse/trust.json when told to trust it,
# and offers a sign-in by browser or by API key; Esc there ends it, not
# signed in, with status 0. A headless session then reads the shared fixture,
# runs its check, edits the greeting and commits, against the fake Model API.
# The host verifies the returned repository and every recorded request. A
# native run leaves out the session, which needs the fake API server.
# hosts: muse.fakeapi.test
# script: fakeapi/muse.json

# shellcheck disable=SC2034 # agents_rows.sh's pane reads it
PANE=muse

# The mark Muse puts before the answer a question has selected, U+27E9, in
# UTF-8.
SELECTED=$(printf '\342\237\251')

row() {
    output "muse --version" env HOME="$WORK/home" muse --version
    same "the version" "$OUT" "Muse Code ${VERSION%%-*} ($VERSION)"
    same "the program" "$(readlink -f "$(type -P muse)")" "/opt/muse/bin/muse"
    has "the terms" "$(cat /opt/muse/TERMS)" "Meta Model API Terms of Service"
    output "muse --help" env HOME="$WORK/home" muse --help
    has "muse --help" "$OUT" "Usage: muse [OPTIONS] [PROMPT]"
    has "muse --help" "$OUT" "Disable approval and sandbox"

    mkdir -p "$WORK/home" "$WORK/project"
    tmux -L muse new-session -d -s muse -x 100 -y 30 -c "$WORK/project" \
        "env HOME='$WORK/home' muse; echo muse ended with status \$?; sleep 600" ||
        fail "tmux new-session: status $?"
    pane "the trust question" "Do you trust this workspace?" 120 &&
        tmux -L muse send-keys -t muse Enter &&
        pane "the sign-in" "Set an API key" 60 &&
        has "the sign-in" "$(tmux -L muse capture-pane -p -t muse)" "Log in with browser" &&
        tmux -L muse send-keys -t muse Escape &&
        pane "the end" "muse ended with status" 60
    has "the end" "$(tmux -L muse capture-pane -p -t muse)" "Not logged in."
    has "the end" "$(tmux -L muse capture-pane -p -t muse)" "muse ended with status 0"
    has "the trust" "$(cat "$WORK/home/.config/muse/trust.json" 2>&1)" "$WORK/project"
    tmux -L muse kill-server 2> /dev/null

    [ -z "$NATIVE" ] || return 0
    muse_session exec
}

# Stop the memory sampler and the private tmux server on every exit path.
row_cleanup() {
    if [ -n "${SAMPLER:-}" ]; then
        kill "$SAMPLER" 2>/dev/null || true
        wait "$SAMPLER" 2>/dev/null || true
        SAMPLER=
    fi
    tmux -L muse kill-server 2>/dev/null || true
}

# muse_quit: end Muse in its window as a person does, with Ctrl-C twice, and
# wait until the window shows that Muse ended. Muse takes the first press as
# a request to quit and shows "Press Ctrl-C again to quit" for three seconds;
# a second press while the notice shows quits. While local work, such as a
# background command, is still active, Muse asks first whether to exit, and
# the row answers Exit anyway. The row reads the screen once a second and
# presses the keys that the screen's state calls for each time the state
# changes: both presses on the prompt, the second on the notice, and Up and
# then Enter on the question. A press that Muse has yet to read leaves the
# state as it was and is not repeated; a notice that ends before the second
# press is read leaves the prompt, where the row presses twice again.
muse_quit() {
    local n=0 screen state seen=''
    while :; do
        screen=$(tmux -u -L muse capture-pane -p -t muse)
        case "$screen" in
            *"muse ended with status"*) return 0 ;;
            *"$SELECTED 1. Exit anyway"*"Enter to confirm"*)
                keys Enter
                pane "the session exit" "muse ended with status" 120
                return
                ;;
            *"Exit anyway"*) state=Up ;;
            *"Press Ctrl-C again to quit"*) state=C-c ;;
            *) state="C-c C-c" ;;
        esac
        if [ "$state" != "$seen" ]; then
            # shellcheck disable=SC2086 # a state calls for one key or two
            keys $state
            seen=$state
        fi
        if [ "$n" -ge 120 ]; then
            pane "the session exit" "muse ended with status" 0
            return 1
        fi
        n=$((n + 1))
        sleep 1
    done
}

# Run the shared script in headless mode or in the terminal, in a fresh home.
# In the terminal the row ends Muse with muse_quit. The script that runs Muse
# runs a handler of its own on SIGINT while Muse runs, which Muse does not
# inherit, and ignores SIGINT once Muse has ended, so a Ctrl-C that comes
# after the end does not end the window.
muse_session() {
    local began screen
    mkdir -p "$WORK/session/home/.config/muse" "$WORK/session/repo" /tmp/agents/results
    cp /tmp/agents/fixture/* "$WORK/session/repo/"
    cat > "$WORK/session/home/.gitconfig" <<'EOF'
[user]
    name = Muse Test
    email = muse@fakeapi.test
[init]
    defaultBranch = main
EOF
    cat > "$WORK/session/home/.config/muse/settings.json" <<EOF
{
  "schema_version": 1,
  "endpoint_transport": {
    "base_url": "https://muse.fakeapi.test/v1",
    "auth": "bearer",
    "ca_bundle": "$CA"
  },
  "telemetry": {"enabled": false},
  "tui": {"prompt_hint_enabled": false},
  "runtime_capabilities": {
    "plugin:tbh-reminders:reminder:goal-reminder": {"enabled": false},
    "plugin:tbh-reminders:reminder:memory": {"enabled": false},
    "plugin:tbh-reminders:reminder:scope-reminder": {"enabled": false},
    "plugin:tbh-reminders:reminder:skill-reminder": {"enabled": false},
    "plugin:tbh-reminders:reminder:todo-reminder": {"enabled": false},
    "plugin:tbh-reminders:reminder:verify-reminder": {"enabled": false}
  }
}
EOF
    cd "$WORK/session/repo" || { fail "the session directory"; return; }
    output "git init" env HOME="$WORK/session/home" git init -q .
    output "git add" git add .
    output "the fixture commit" env HOME="$WORK/session/home" git commit -q -m 'The fixture'
    output "the binary before the session" sha256sum /opt/muse/bin/muse
    printf '%s\n' "$OUT" > "$WORK/session/binary.before"
    sample_memory > "$WORK/session/memory" &
    SAMPLER=$!
    printf '%s\n' "$1" > "$WORK/session/mode"
    began=$(date +%s)
    if [ "$1" = exec ]; then
        output "muse exec" env -i PATH="$PATH" HOME="$WORK/session/home" TMPDIR="$WORK/session" \
        LANG=C.UTF-8 TERM=dumb META_API_KEY=meta-fakeapi-dummy \
        muse exec --yolo --model muse-spark-1.2 --max-model-steps 8 \
        'Read notes.txt and do what it says. Check the change with check.sh, then commit it.'
    else
        cat > "$WORK/session/start.sh" <<EOF
#!/bin/bash
cd '$WORK/session/repo' || exit 1
trap : INT
env -i PATH='$PATH' HOME='$WORK/session/home' TMPDIR='$WORK/session' \\
    LANG=C.UTF-8 TERM=xterm-256color META_API_KEY=meta-fakeapi-dummy \\
    muse --yolo --model muse-spark-1.2 \\
    'Read notes.txt and do what it says. Check the change with check.sh, then commit it.'
status=\$?
trap '' INT
echo "muse ended with status \$status"
sleep 600
EOF
        tmux -L muse new-session -d -s muse -x 100 -y 40 \
            "/bin/bash '$WORK/session/start.sh'" || fail "the tmux session"
        pane "the completed session" "the change is committed." 300
        tmux -L muse capture-pane -p -S - -t muse > "$WORK/session/screen.txt"
        muse_quit
        screen=$(tmux -L muse capture-pane -p -t muse)
        case "$screen" in
            *"muse ended with status 0"*|*"muse ended with status 130"*) ;;
            *) fail "the interactive session's exit: $screen" ;;
        esac
        tmux -L muse kill-server 2>/dev/null || true
    fi
    printf '%s\n' "$(($(date +%s) - began))" > "$WORK/session/seconds"
    kill "$SAMPLER"
    wait "$SAMPLER" 2>/dev/null || true
    SAMPLER=
    output "the binary after the session" sha256sum /opt/muse/bin/muse
    printf '%s\n' "$OUT" > "$WORK/session/binary.after"
    output "the repository archive" tar -cf "/tmp/agents/results/$ROW.tar" \
        -C "$WORK/session" repo binary.before binary.after memory seconds mode
}
