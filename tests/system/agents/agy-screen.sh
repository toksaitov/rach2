# shellcheck shell=bash
# agy-screen: Antigravity CLI plays the exchange of
# tests/system/fakeapi/agy-session.json in its screen, in tmux, as the test
# account, with the settings, the names and the session of the agy-print
# row. Started in a home it has not run in, it asks for a color scheme, for
# consent to Google's use of the person's interactions, which it offers
# given, and whether to trust the directory; the row answers each with the
# keys a person would press, withholds the consent and trusts the
# directory, which the program records in its settings. It then types the
# request, waits for the answer and ends the session with /exit. Beside the
# first turn the program asks a smaller model for the conversation's title,
# which the script has as an unordered step. The host verifies the session as
# for agy-print.
# bound: 1200
# hosts: fakeapi.test generativelanguage.googleapis.com
# hosts: antigravity-unleash.goog play.googleapis.com
# script: fakeapi/agy-session.json
# script-filter: agy_session_verify.py
# package: agy
# verifier: agy_session_verify.py

# shellcheck source=tests/system/agents/agy-print.sh
. /tmp/agents/agy-print.sh

# shellcheck disable=SC2034 # agents_rows.sh's pane reads it
PANE=agy

# keys KEY...: press the keys KEY in the screen.
keys() {
    tmux -u -L agy send-keys -t agy "$@"
}

# press WHAT KEY TEXT: press KEY until the screen has shown TEXT for 5 s,
# within 60 s. A key that comes while the program still draws a question can
# be lost under TCG, so the row presses KEY again whenever TEXT is not on the
# screen, at most every 5 s; a question that takes the key twice, as the
# consent's box does, is then put right by the next press.
press() {
    local n=0 held=0
    while [ "$n" -lt 60 ]; do
        case "$(tmux -u -L agy capture-pane -p -t agy)" in
            *"$3"*)
                held=$((held + 1))
                [ "$held" -le 5 ] || return 0
                ;;
            *)
                held=0
                [ $((n % 5)) != 0 ] || keys "$2"
                ;;
        esac
        n=$((n + 1))
        sleep 1
    done
    pane "$1" "$3" 0
}

row() {
    local pid env
    session_start screen || return
    env=$(session_env "\$TERM" | sed 's/.*/"&"/' | tr '\n' ' ')
    tmux -u -L agy new-session -d -s agy -x 120 -y 40 -c "$SESSION/repo" \
        "env -i $env agy --dangerously-skip-permissions; \
        echo agy ended with status \$?; sleep 600" ||
        fail "tmux new-session: status $?"
    pane "the color scheme" "Choose your color scheme" 300 && step "the color scheme" &&
        press "the consent" Enter "Terms of Service & Data Use" &&
        press "the consent withheld" Enter "[ ] Yes, I agree" &&
        press "the buttons" Down ">  Previous" &&
        press "the button Done" Right ">  Done" &&
        press "the prompt" Enter "? for shortcuts" && step "the prompt"
    if [ -z "$FAILED" ]; then
        pid=$(pgrep -u "$(id -u)" -x agy)
        session_unmodified "agy's screen" "$pid"
        keys -l "$PROMPT" && pane "the request" "and commit the change." 120 && keys Enter &&
            pane "the answer" "The fixture was read, checked, edited and committed." 600 &&
            step "the answer" && keys -l /exit && pane "the command" "/exit" 60 &&
            keys Enter && pane "the end" "agy ended with status" 300 && step "the end"
        has "the end" "$(tmux -u -L agy capture-pane -p -t agy)" "agy ended with status 0"
        same "the trusted directory" \
            "$(grep -c "\"$SESSION/repo\"" "$SESSION/home/.gemini/antigravity-cli/settings.json")" 1
    fi
    tmux -L agy kill-server 2> /dev/null
    session_end
}
