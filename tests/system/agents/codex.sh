# shellcheck shell=bash
# codex: the version its recipe pins, the program the path finds and its
# license beside it, `codex login status` with no account signed in, and
# `codex exec --help`. Then Codex's screen in tmux, from a home that has no
# ~/.codex: it offers to sign in, among other ways by a device code, while
# Codex is one process, whose program is /opt/codex/bin/codex, so that it
# finds its helpers there, and has made no copy of itself in
# ~/.codex/packages. Codex starts its background server, and makes that
# copy, before it draws the screen. Last, `codex features list` says that
# the server is off, as /etc/codex/config.toml sets it.

row() {
    local pane pids

    rm -rf "$HOME/.codex"
    output "codex --version" codex --version
    same "the version" "$OUT" "codex-cli $VERSION"
    same "the program" "$(readlink -f "$(type -P codex)")" "/opt/codex/bin/codex"
    has "the license" "$(head -n 2 /opt/codex/LICENSE)" "Apache License"
    status "codex login status" 1 codex login status
    has "codex login status" "$OUT" "Not logged in"
    output "codex exec --help" codex exec --help
    has "codex exec --help" "$OUT" "Run Codex non-interactively"

    output "tmux new-session" tmux new-session -d -s codex -x 100 -y 30 codex
    while pane=$(tmux capture-pane -p -t codex 2>&1); do
        case $pane in
            *"Sign in with Device Code"*) break ;;
        esac
        # interval: the pane is read every second until the screen is drawn
        sleep 1
    done
    has "the screen" "$pane" "Sign in with Device Code"
    pids=$(pgrep -u "$(id -u)" -x codex | tr '\n' ' ')
    same "the processes" "$(echo "$pids" | wc -w)" "1"
    same "the screen's program" "$(readlink "/proc/${pids%% *}/exe")" "/opt/codex/bin/codex"
    same "a copy in ~/.codex/packages" "$(ls -A "$HOME/.codex/packages" 2> /dev/null || echo none)" "none"
    tmux kill-session -t codex
    output "codex features list" codex features list
    same "the background server" \
        "$(printf '%s\n' "$OUT" | awk '$1 == "daemon_auto_start" { print $3 }')" "false"
    rm -rf "$HOME/.codex"
}
