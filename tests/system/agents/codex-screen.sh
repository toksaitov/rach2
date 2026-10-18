# shellcheck shell=bash
# codex-screen: Codex plays the exchange of tests/system/fakeapi/codex.json
# in its screen, in tmux, as the test account, with the settings, the names
# and the session of the codex-exec row. Started in a fresh home, it asks
# whether to trust the repository and whether to try a newer model than the
# one its settings name; the row answers with the keys a person would press,
# trusting the repository and keeping the model the script requires. The
# row then types the request, waits for the answer and ends the session
# with /quit. As it starts, the screen fetches a notice to show from
# raw.githubusercontent.com, which the server answers with 404, and beside
# the first turn it asks the model for the session's title on a connection
# of its own, a WebSocket that the server refuses with 426 and then a POST;
# codex-screen.json adds the three to the script. Once Codex has ended, no
# process it started may be left; the host verifies the session as for
# codex-exec.
# bound: 1200
# hosts: fakeapi.test api.openai.com chatgpt.com auth.openai.com api.github.com
# hosts: raw.githubusercontent.com
# script: fakeapi/codex.json
# script-filter: codex_session_verify.py
# package: codex
# verifier: codex_session_verify.py

# shellcheck source=tests/system/agents/codex-exec.sh
. /tmp/agents/codex-exec.sh

# shellcheck disable=SC2034 # agents_rows.sh's pane reads it
PANE=codex

# The mark Codex puts before the answer a question has selected, U+203A, in
# UTF-8.
# shellcheck disable=SC2034 # agents_rows.sh's choose reads it
SELECTED=$(printf '\342\200\272')

row() {
    local pid env
    session_start screen || return
    env=$(session_env "\$TERM" | sed 's/.*/"&"/' | tr '\n' ' ')
    tmux -u -L codex new-session -d -s codex -x 120 -y 40 -c "$SESSION/repo" \
        "env -i $env codex; echo codex ended with status \$?; sleep 600" ||
        fail "tmux new-session: status $?"
    pane "the trust" "Trust and continue" 300 && step "the trust" &&
        choose "the trust" Up "1. Trust and continue" && keys Enter &&
        pane "the model" "Use existing model" 300 &&
        choose "the model" Down "2. Use existing model" && keys Enter &&
        pane "the prompt" "Ask Codex to do anything" 300 && step "the prompt"
    if [ -z "$FAILED" ]; then
        pid=$(program codex)
        unmodified "codex's screen" "$pid" /opt/codex/bin/codex \
            "$(readlink -f /etc/localtime)" "$SESSION/home/.codex/*.sqlite-shm"
        keys -l "$PROMPT" && pane "the request" "then commit it." 120 && keys Enter &&
            pane "the answer" "the change is committed." 600 &&
            step "the answer" && keys -l /quit && pane "the command" "/quit" 60 &&
            keys Enter && pane "the end" "codex ended with status" 300 && step "the end"
        has "the end" "$(tmux -u -L codex capture-pane -p -t codex)" "codex ended with status 0"
        gone "codex's screen"
    fi
    tmux -L codex kill-server 2> /dev/null
    session_end
}
