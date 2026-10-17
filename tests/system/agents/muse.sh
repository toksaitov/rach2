# shellcheck shell=bash
# muse: the version its recipe pins, the program the path finds and the
# terms beside it, `muse --help`, and its screen in tmux, in a home of its
# own, up to its sign-in: it asks whether to trust the directory it starts
# in, records the answer in ~/.config/muse/trust.json when told to trust it,
# and offers a sign-in by browser or by API key; Esc there ends it, not
# signed in, with status 0.
# bound: 300

# pane WHAT TEXT SECONDS: wait until the muse window of tmux shows TEXT,
# looking once a second for at most SECONDS.
pane() {
    local n=0 screen
    while :; do
        screen=$(tmux -L muse capture-pane -p -t muse 2>&1)
        case "$screen" in
            *"$2"*) return 0 ;;
        esac
        if [ "$n" -ge "$3" ]; then
            fail "$1: in $3 s the screen showed no [$2], but [$(printf '%s' "$screen" |
                sed '/^ *$/d' | tail -n 4 | tr '\n' ' ')]"
            return 1
        fi
        n=$((n + 1))
        sleep 1
    done
}

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
}
