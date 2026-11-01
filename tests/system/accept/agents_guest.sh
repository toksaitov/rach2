#!/bin/bash
#
# agents_guest.sh: the machine's half of agents_accept.sh, which copies it to
# ~/rach2-accept/guest.sh in the account it accepts and runs it there over
# SSH, once for each command below.
#
# Usage: guest.sh COMMAND [AGENT] [ARGUMENT]
#
# AGENT is claude, codex, agy, muse or opencode. Each works in the
# disposable repository ~/rach2-accept/AGENT, made from the files
# agents_accept.sh puts in ~/rach2-accept/fixture, and its screen is the tmux
# session rach2-accept-AGENT on the server rach2-accept. The commands:
#
#   prepare AGENT     make the repository again, with one commit.
#   run AGENT ACTION  run the agent as ACTION says, in its repository and in
#                     the environment ~/rach2-accept/env gives where it
#                     exists, then print `rach2-accept: ended with status N`.
#                     ACTION is start or resume, for its screen, or once,
#                     for one request without a screen.
#   version AGENT     print the agent's version, as its --version prints it,
#                     and the SHA-256 of its program; AGENT may also be gh.
#   pid AGENT         print the process of the agent in its tmux session.
#   exe AGENT         print the program that process runs.
#   sleeps AGENT      print each process in the session that runs `sleep 600`.
#   watch AGENT       start in the background a watcher that reads `sleeps
#                     AGENT` four times a second and records what it first
#                     prints in ~/rach2-accept/AGENT.sleeps; it ends then, or
#                     when the session has gone.
#   seen AGENT        end the watcher and print what it recorded.
#   pane AGENT        print the pane's size, its terminal's size by stty, and
#                     the number of clients attached to the session.
#   screen AGENT [all]
#                     print the agent's screen, or with `all` its whole
#                     history.
#   work AGENT        print the repository's commits, its greeting, the result
#                     of its check at HEAD and the files `git status` lists.
#   credentials MODE [AGENT...]
#                     print `SOURCE SHA256` or `SOURCE missing` for each
#                     selected agent's credential source and gh's, all agents
#                     by default.
#   secrets MODE [AGENT...]
#                     read a log on standard input and print how many strings
#                     of those credential sources it has.
#   redact            copy standard input to standard output with each
#                     credential string replaced, as below.
#   filtered MODE [AGENT...]
#                     print what `secrets MODE AGENT...` counts in those
#                     sources after `redact`: `0 of N` when the filter
#                     removes every string.
#   setup [AGENT]     give the account the settings and dummy keys that
#                     point every agent at the fake API server, for the QEMU
#                     mode, and import OpenCode's key into its database when
#                     AGENT is opencode; the server's certificate authority
#                     is ~/rach2-accept/ca.pem.
#
# MODE is lenovo or qemu. A credential source is a file and what reading it
# prints: for most, the file itself; for OpenCode's database, opencode.db,
# the secrets of its credential and account records, read by sqlite3 in one
# transaction, so that the read is consistent while a session writes the
# database and does not change with the sessions it stores. The sources of
# the Lenovo are those the manual pages name for each sign-in; those of the
# QEMU mode are the dummy keys `setup` writes, OpenCode's database, into
# which `setup` imports its key, and the token `setup` gives gh. A
# source's strings are the runs of 16 or more of the characters a token is
# written with in what reading it prints, which may be binary, and only
# their count is printed.
#
# Every command but setup, secrets and redact, run with start or resume, and
# the watcher that watch starts, prints through `redact`, so that no text
# read from the machine has a credential when it reaches agents_accept.sh;
# run with once prints its standard error there too. `redact` reads the
# credential sources of both modes that exist, replaces each of their
# strings with `[credential]`, and then each run of 16 or more token
# characters that is part of one, such as the half of a string a screen
# wraps.

set -u
export LC_ALL=C.UTF-8

DIR=$HOME/rach2-accept
SERVER=rach2-accept
CANCEL_LINE='sleep 600'
OPENCODE_DB=$HOME/.local/share/opencode/opencode.db
ONCE_PROMPT='Reply with the words: signed in.'

# session AGENT: the agent's tmux session.
session() {
    printf 'rach2-accept-%s\n' "$1"
}

# command AGENT ACTION: print the command line of ACTION for AGENT. MUSE_ARGS
# is empty on the Lenovo; the QEMU mode's environment names the model of the
# fake server's script. OpenCode runs with a private server, which ends with
# it, in place of the background service; its request without a screen runs
# under SSH without the login shell whose profile turns off its update check,
# so its command line does that.
# shellcheck disable=SC2016 # the run command's eval expands MUSE_ARGS
command_line() {
    case $1.$2 in
        claude.start) echo 'claude --permission-mode bypassPermissions' ;;
        claude.resume) echo 'claude --permission-mode bypassPermissions --continue' ;;
        claude.once) echo "claude -p '$ONCE_PROMPT'" ;;
        codex.start) echo 'codex --ask-for-approval never' ;;
        codex.resume) echo 'codex resume --last --ask-for-approval never' ;;
        codex.once) echo "codex exec '$ONCE_PROMPT' < /dev/null" ;;
        agy.start) echo 'agy --dangerously-skip-permissions' ;;
        agy.resume) echo 'agy --dangerously-skip-permissions --continue' ;;
        agy.once) echo "agy -p '$ONCE_PROMPT'" ;;
        muse.start) echo 'muse --yolo $MUSE_ARGS' ;;
        muse.resume) echo 'muse resume --last --yolo $MUSE_ARGS' ;;
        muse.once) echo "muse exec --yolo \$MUSE_ARGS '$ONCE_PROMPT'" ;;
        opencode.start) echo 'opencode --standalone --auto' ;;
        opencode.resume) echo 'opencode --standalone --auto --continue' ;;
        opencode.once)
            echo "OPENCODE_DISABLE_AUTOUPDATE=1 opencode run --standalone '$ONCE_PROMPT' < /dev/null"
            ;;
        gh.once) echo 'gh auth status' ;;
        *) return 1 ;;
    esac
}

# pane_pid AGENT: print the process of the shell in the agent's pane.
pane_pid() {
    tmux -L "$SERVER" display-message -p -t "$(session "$1")" '#{pane_pid}' 2> /dev/null
}

# descendants PID: print each process below PID, with its name and command
# line, a process to a line.
descendants() {
    ps -e -o pid=,ppid=,comm=,args= | awk -v root="$1" '
        { parent[$1] = $2; line[$1] = $0 }
        END {
            for (p in parent) {
                q = p
                while (q in parent && q != root && q > 1)
                    q = parent[q]
                if (q == root && p != root)
                    print line[p]
            }
        }' | sort -n
}

# credential_sources MODE [AGENT...]: print the selected agents' credential
# sources and gh's, a path to a line; all agents if none are given. With MODE
# `all`, include the sources of both modes for the output filter.
credential_sources() {
    local mode=$1 agent
    shift
    if [ "$#" -eq 0 ]; then
        set -- claude codex agy muse opencode
    fi
    for agent in "$@"; do
        if [ "$mode" != lenovo ]; then
            printf '%s\n' "$DIR/keys/$agent"
        fi
        if [ "$mode" != qemu ]; then
            case $agent in
                claude) printf '%s\n' "$HOME/.claude/.credentials.json" ;;
                codex) printf '%s\n' "$HOME/.codex/auth.json" ;;
                agy) printf '%s\n' "$HOME/.gemini/antigravity-cli/antigravity-oauth-token" ;;
                muse) printf '%s\n' "$HOME/.config/muse/auth.json" ;;
            esac
        fi
        if [ "$agent" = opencode ]; then
            printf '%s\n' "$OPENCODE_DB"
        fi
    done
    printf '%s\n' "$HOME/.config/gh/hosts.yml"
}

# source_read SOURCE: print what the credential source SOURCE holds, or
# nothing when it holds no credential. OpenCode's database gives the secret
# fields of its credential and account records, each table in the order of
# its key, in one read transaction; sqlite3 opens it read-only and waits up
# to ten seconds for a writer's lock.
source_read() {
    case $1 in
        "$OPENCODE_DB")
            [ ! -f "$1" ] || sqlite3 -readonly -batch -noheader -list "$1" \
                '.timeout 10000' 'BEGIN' \
                'SELECT value FROM credential ORDER BY id' \
                'SELECT access_token, refresh_token FROM account ORDER BY id' \
                'SELECT access_token, refresh_token FROM control_account ORDER BY email, url' \
                'COMMIT'
            ;;
        *)
            [ ! -f "$1" ] || cat "$1"
            ;;
    esac
}

# credential_strings MODE [AGENT...]: print the strings of the selected
# credential sources, each once, a string to a line. grep reads a source as
# text whatever bytes it holds.
credential_strings() {
    credential_sources "$@" | while read -r source; do
        source_read "$source" | grep -aoE '[A-Za-z0-9._~+/-]{16,}'
    done | sort -u
}

# redact: copy standard input to standard output with the credential strings
# of both modes, and the runs of token characters that are part of one,
# replaced by `[credential]`. awk reads the strings on descriptor 3, since a
# process's arguments are readable by every user of the machine.
redact() {
    LC_ALL=C awk '
        function cut(text, string,    out, i) {
            out = ""
            while ((i = index(text, string)) > 0) {
                out = out substr(text, 1, i - 1) "[credential]"
                text = substr(text, i + length(string))
            }
            return out text
        }
        function part(run,    i) {
            for (i = 1; i <= n; i++)
                if (index(strings[i], run) > 0)
                    return 1
            return 0
        }
        BEGIN { while ((getline line < "/dev/fd/3") > 0) strings[++n] = line }
        {
            text = $0
            for (i = 1; i <= n; i++)
                if (strings[i] != "")
                    text = cut(text, strings[i])
            out = ""
            while (match(text, /[A-Za-z0-9._~+\/-]+/)) {
                run = substr(text, RSTART, RLENGTH)
                if (RLENGTH >= 16 && part(run))
                    run = "[credential]"
                out = out substr(text, 1, RSTART - 1) run
                text = substr(text, RSTART + RLENGTH)
            }
            print out text
        }' 3< <(credential_strings all)
}

# scratch: install the traps that remove $dir, the command's scratch
# directory, when the shell exits or a hangup, an interrupt, a broken pipe or
# a termination ends it. A pipeline runs main in a subshell, which keeps none
# of its parent's traps, so each command that makes scratch calls this first.
scratch() {
    dir=
    trap '[ -z "$dir" ] || rm -rf "$dir"' EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 141' PIPE
    trap 'exit 143' TERM
}

# setup [AGENT]: write the QEMU mode's dummy keys, the environment that points
# the agents at the fake server, and the settings each agent reads from its
# home. OpenCode keeps its key in its database, where only the program writes,
# so for AGENT opencode its own `auth import` stores the key there.
setup() {
    local ca=$DIR/ca.pem
    mkdir -p "$DIR/keys" "$HOME/.codex" "$HOME/.config/muse" "$HOME/.config/gh" \
        "$HOME/.gemini/antigravity-cli" "$HOME/.config/opencode" || return 1
    umask 077
    printf '%s\n' rach2-offline-dummy-key > "$DIR/keys/claude"
    printf '%s\n' sk-fakeapi-dummy > "$DIR/keys/codex"
    printf '%s\n' rach2-offline-dummy-key > "$DIR/keys/agy"
    printf '%s\n' meta-fakeapi-dummy > "$DIR/keys/muse"
    printf '%s\n' rach2-offline-dummy-key > "$DIR/keys/opencode"
    cat > "$DIR/env" <<EOF
export ANTHROPIC_BASE_URL=https://fakeapi.test
export ANTHROPIC_API_KEY=\$(cat "$DIR/keys/claude")
export ANTHROPIC_MODEL=claude-sonnet-4-6
export NODE_EXTRA_CA_CERTS=$ca
export CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC=1
export CLAUDE_CODE_DISABLE_OFFICIAL_MARKETPLACE_AUTOINSTALL=1
export CLAUDE_CODE_ENABLE_PROMPT_SUGGESTION=false
export FAKEAPI_KEY=\$(cat "$DIR/keys/codex")
export CODEX_CA_CERTIFICATE=$ca
export GEMINI_API_KEY=\$(cat "$DIR/keys/agy")
export GOOGLE_GEMINI_BASE_URL=https://fakeapi.test
export SSL_CERT_FILE=$ca
export META_API_KEY=\$(cat "$DIR/keys/muse")
export OPENCODE_DISABLE_AUTOUPDATE=1
export OPENCODE_DISABLE_MODELS_FETCH=1
MUSE_ARGS='--model muse-spark-1.2'
EOF
    cat > "$HOME/.codex/config.toml" <<'EOF'
model = "gpt-5.5"
model_provider = "fakeapi"

[features]
plugins = false

[analytics]
enabled = false

[model_providers.fakeapi]
name = "fakeapi"
base_url = "https://fakeapi.test/v1"
env_key = "FAKEAPI_KEY"
wire_api = "responses"
supports_websockets = true
EOF
    cat > "$HOME/.config/muse/settings.json" <<EOF
{
  "schema_version": 1,
  "endpoint_transport": {
    "base_url": "https://muse.fakeapi.test/v1",
    "auth": "bearer",
    "ca_bundle": "$ca"
  },
  "telemetry": {"enabled": false},
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
    printf '%s\n' '{"modelProvider": "gemini", "enableTelemetry": false}' \
        > "$HOME/.gemini/antigravity-cli/settings.json"
    cat > "$HOME/.config/gh/hosts.yml" <<'EOF'
github.com:
    users:
        rach2-test:
            oauth_token: gho_rach2offlinedummytoken0000000000
    git_protocol: https
    user: rach2-test
    oauth_token: gho_rach2offlinedummytoken0000000000
EOF
    cat > "$HOME/.config/opencode/opencode.json" <<'EOF'
{
  "model": "anthropic/claude-sonnet-4-6",
  "small_model": "anthropic/claude-sonnet-4-6",
  "provider": {"anthropic": {"options": {"baseURL": "https://fakeapi.test/v1"}}}
}
EOF
    if [ "${1:-}" = opencode ]; then
        printf '[{"id": "cred_rach2accept", "integrationID": "anthropic", "label": "Anthropic",
            "active": true, "value": {"type": "key", "key": "%s"}}]\n' \
            "$(cat "$DIR/keys/opencode")" |
            OPENCODE_DISABLE_AUTOUPDATE=1 OPENCODE_DISABLE_MODELS_FETCH=1 \
                opencode auth import --standalone > /dev/null
    fi
}

# main COMMAND [AGENT] [ARGUMENT]: run COMMAND.
main() {
    case ${1:-} in
        prepare)
            rm -rf "${DIR:?}/$2" && mkdir -p "$DIR/$2" && cp "$DIR"/fixture/* "$DIR/$2/" &&
                cd "$DIR/$2" && git init -q --initial-branch=master &&
                git config user.name 'Rach2 acceptance' &&
                git config user.email accept@rach2.invalid &&
                git config commit.gpgsign false && git add . && git commit -qm 'The fixture'
            ;;
        run)
            line=$(command_line "$2" "$3") || { echo "guest: no action $3 for $2" >&2; exit 2; }
            # shellcheck disable=SC2034 # the eval below reads it
            MUSE_ARGS=
            # shellcheck disable=SC1091 # the QEMU mode's environment
            [ ! -f "$DIR/env" ] || . "$DIR/env"
            # OpenCode signs in by the key in its database, not Claude Code's
            [ "$2" != opencode ] || unset ANTHROPIC_API_KEY ANTHROPIC_BASE_URL ANTHROPIC_MODEL
            if [ -d "$DIR/$2" ]; then
                cd "$DIR/$2" || exit 1
            fi
            eval "$line"
            echo "rach2-accept: ended with status $?"
            ;;
        version)
            case $2 in
                gh) out=$(gh --version | head -n 1) ;;
                *) out=$("$2" --version 2>&1) ;;
            esac
            printf '%s\n' "$out"
            sha256sum "$(readlink -f "$(type -P "$2")")"
            ;;
        pid)
            root=$(pane_pid "$2") || exit 0
            descendants "$root" | awk -v name="$2" '$3 == name { print $1; exit }'
            ;;
        exe)
            pid=$(bash "$0" pid "$2")
            [ -z "$pid" ] || readlink "/proc/$pid/exe"
            ;;
        sleeps)
            root=$(pane_pid "$2") || exit 0
            descendants "$root" | awk -v line="$CANCEL_LINE" '
                { args = $0; sub(/^ *[0-9]+ +[0-9]+ +[^ ]+ +/, "", args) }
                args == line { print $1 }'
            ;;
        watch)
            rm -f "$DIR/$2.sleeps" "$DIR/$2.watch"
            setsid bash "$0" watcher "$2" < /dev/null > /dev/null 2>&1 &
            ;;
        watcher)
            echo "$$" > "$DIR/$2.watch"
            while pane_pid "$2" > /dev/null; do
                out=$(main sleeps "$2")
                if [ -n "$out" ]; then
                    printf '%s\n' "$out" > "$DIR/$2.sleeps"
                    break
                fi
                # interval: the session is read four times a second
                sleep 0.25
            done
            rm -f "$DIR/$2.watch"
            ;;
        seen)
            [ ! -f "$DIR/$2.watch" ] || kill "$(cat "$DIR/$2.watch")" 2> /dev/null
            rm -f "$DIR/$2.watch"
            [ ! -f "$DIR/$2.sleeps" ] || cat "$DIR/$2.sleeps"
            rm -f "$DIR/$2.sleeps"
            ;;
        pane)
            tmux -L "$SERVER" display-message -p -t "$(session "$2")" \
                '#{pane_width}x#{pane_height} #{pane_tty} #{session_attached}' 2> /dev/null |
                while read -r size tty attached; do
                    printf '%s %s %s\n' "$size" \
                        "$(stty -F "$tty" size | awk '{ print $2 "x" $1 }')" "$attached"
                done
            ;;
        work)
            cd "$DIR/$2" || exit 1
            printf 'commits %s\n' "$(git rev-list --count HEAD)"
            printf 'subject %s\n' "$(git log -1 --format=%s)"
            printf 'greeting %s\n' "$(git show HEAD:greeting.txt)"
            scratch
            dir=$(mktemp -d "${TMPDIR:-/tmp}/rach2-accept-check.XXXXXX") || exit 1
            git archive HEAD | tar -x -C "$dir"
            printf 'check %s\n' "$(cd "$dir" && sh check.sh 2>&1)"
            rm -rf "$dir"
            printf 'status %s\n' "$(git --no-optional-locks status --porcelain | tr '\n' ' ')"
            ;;
        credentials)
            scratch
            dir=$(mktemp -d "${TMPDIR:-/tmp}/rach2-accept-sources.XXXXXX") || exit 1
            credential_sources "${@:2}" | while read -r source; do
                source_read "$source" > "$dir/read"
                if [ -s "$dir/read" ]; then
                    printf '%s %s\n' "$source" "$(sha256sum < "$dir/read" | cut -d ' ' -f 1)"
                else
                    printf '%s missing\n' "$source"
                fi
            done
            rm -rf "$dir"
            ;;
        screen)
            if [ "${3:-}" = all ]; then
                tmux -u -L "$SERVER" capture-pane -p -S - -t "$(session "$2")"
            else
                tmux -u -L "$SERVER" capture-pane -p -t "$(session "$2")"
            fi
            ;;
        secrets)
            scratch
            dir=$(mktemp -d "${TMPDIR:-/tmp}/rach2-accept-secrets.XXXXXX") || exit 1
            cat > "$dir/log"
            credential_strings "${@:2}" > "$dir/strings"
            printf '%d of %d strings\n' "$(($(grep -aoFf "$dir/strings" "$dir/log" | sort -u | wc -l)))" \
                "$(($(wc -l < "$dir/strings")))"
            rm -rf "$dir"
            ;;
        redact)
            redact
            ;;
        filtered)
            credential_sources "${@:2}" | while read -r source; do
                source_read "$source"
            done | redact | main secrets "${@:2}"
            ;;
        setup)
            setup "${2:-}"
            ;;
        *)
            echo "usage: guest.sh COMMAND [AGENT] [ARGUMENT]" >&2
            exit 2
            ;;
    esac
}

case ${1:-}.${3:-} in
    setup.* | secrets.* | redact.* | watcher.* | run.start | run.resume)
        main "$@"
        ;;
    run.*)
        main "$@" 2>&1 | redact
        exit "${PIPESTATUS[0]}"
        ;;
    *)
        main "$@" | redact
        exit "${PIPESTATUS[0]}"
        ;;
esac
