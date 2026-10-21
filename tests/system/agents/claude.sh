# shellcheck shell=bash
# claude: the version its recipe pins, the program the path finds and
# Anthropic's license beside it, `claude --help`, `claude update`, which the
# managed settings of /etc/claude-code refuse without fetching a release, so
# that the version stays the pinned one, and `claude auth status` with no
# account signed in. Claude Code keeps its configuration in its home and
# makes a directory under TMPDIR, so the row gives it both in WORK. Nothing
# here needs the network.

row() {
    export HOME="$WORK/home" TMPDIR="$WORK/tmp"
    mkdir -p "$HOME" "$TMPDIR"

    output "claude --version" claude --version
    same "the version" "$OUT" "$VERSION (Claude Code)"
    same "the program" "$(readlink -f "$(type -P claude)")" "/opt/claude/bin/claude"
    has "the license" "$(head -n 1 /opt/claude/LICENSE.md)" "Anthropic PBC. All rights reserved."

    output "claude --help" claude --help
    same "claude --help" "$(printf '%s\n' "$OUT" | head -n 1)" \
        "Usage: claude [options] [command] [prompt]"
    has "claude --help" "$OUT" "setup-token"

    output "claude update" claude update
    has "claude update" "$OUT" "Updates are disabled by your administrator"
    output "claude --version after claude update" claude --version
    same "the version after claude update" "$OUT" "$VERSION (Claude Code)"

    status "claude auth status" 1 claude auth status
    has "claude auth status" "$OUT" '"loggedIn": false'
    has "claude auth status" "$OUT" "\"configDirectory\": \"$HOME/.claude\""
}
