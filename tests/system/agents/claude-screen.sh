# shellcheck shell=bash
# claude-screen: Claude Code plays the exchange of
# tests/system/fakeapi/claude-session.json in its screen, in tmux, as the
# test account, with the settings, the names and the session of the
# claude-print row. Started in an empty home, it checks that it can reach
# Anthropic's services, as its first start does, and asks for a theme,
# whether to use the key in ANTHROPIC_API_KEY, to read its security notes,
# whether to trust the directory and whether to accept the bypass of its
# permissions; the row answers each with the keys a person would press, and
# the server answers the two checks, /api/hello and /v1/oauth/hello, as
# claude-screen.json says. The row then types the request, waits for the
# answer and ends the session with /exit. Beside the first turn Claude Code
# asks the model for the session's title, which the script also has. The
# host verifies the session as for claude-print.
# bound: 1200
# hosts: fakeapi.test api.anthropic.com platform.claude.com claude.com
# hosts: downloads.claude.ai raw.githubusercontent.com
# hosts: http-intake.logs.us5.datadoghq.com
# script: fakeapi/claude-session.json
# script-filter: claude_session_verify.py
# package: claude
# verifier: claude_session_verify.py

# shellcheck source=tests/system/agents/claude-print.sh
. /tmp/agents/claude-print.sh

# shellcheck disable=SC2034 # agents_rows.sh's pane reads it
PANE=claude

# The mark Claude Code puts before the answer a question has selected,
# U+276F, in UTF-8.
# shellcheck disable=SC2034 # agents_rows.sh's choose reads it
SELECTED=$(printf '\342\235\257')

row() {
    local pid env
    session_start screen || return
    env=$(session_env "\$TERM" | sed 's/.*/"&"/' | tr '\n' ' ')
    tmux -u -L claude new-session -d -s claude -x 120 -y 40 -c "$SESSION/repo" \
        "env -i $env claude --model claude-sonnet-4-6 --permission-mode bypassPermissions \
            --tools Read,Bash,Edit --strict-mcp-config --mcp-config '{\"mcpServers\":{}}' \
            --setting-sources user --settings '{\"sandbox\":{\"enabled\":false}}'; \
        echo claude ended with status \$?; sleep 600" ||
        fail "tmux new-session: status $?"
    pane "the theme" "Choose the text style" 300 && step "the theme" && keys Enter &&
        pane "the key" "Do you want to use this API key?" 300 &&
        choose "the key" Up "Yes" && keys Enter &&
        pane "the security notes" "Press Enter to continue" 300 && keys Enter &&
        pane "the trust" "Yes, I trust this folder" 300 &&
        choose "the trust" Down "Yes, I trust this folder" && keys Enter &&
        pane "the bypass" "Yes, I accept" 300 &&
        choose "the bypass" Down "Yes, I accept" && keys Enter &&
        pane "the prompt" "bypass permissions on" 300 && step "the prompt"
    if [ -z "$FAILED" ]; then
        pid=$(program claude)
        unmodified "claude's screen" "$pid" /opt/claude/bin/claude \
            /lib64/ld-linux-x86-64.so.2 /lib/libc.so.6 /lib/libm.so.6 /lib/libpthread.so.0 \
            /lib/libdl.so.2 /lib/librt.so.1
        keys -l "$PROMPT" && pane "the request" "and commit the change." 120 && keys Enter &&
            pane "the answer" "The fixture was read, checked, edited and committed." 600 &&
            step "the answer" && keys -l /exit && pane "the command" "/exit" 60 &&
            keys Enter && pane "the end" "claude ended with status" 300 && step "the end"
        has "the end" "$(tmux -u -L claude capture-pane -p -t claude)" "claude ended with status 0"
    fi
    tmux -L claude kill-server 2> /dev/null
    session_end
}
