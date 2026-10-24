# shellcheck shell=bash
# agy: the version its recipe pins, the program the path finds, the terms
# beside it, the updater turned off in a login shell's environment, and
# --help. Then its screen in tmux, offline and in a home of the row's own:
# the menu of ways to sign in, and after Google OAuth is chosen, Google's
# sign-in address and the prompt for the code the browser shows. Every
# process of the program exits once tmux has ended. A native run leaves out
# the screen, which needs QEMU's restricted network to be offline.

# screen WHAT TEXT...: wait until one pane snapshot contains every TEXT.
# Keep the complete snapshot in the row output for a failed run or signal.
screen() {
    local what=$1 text complete previous=''
    shift
    while OUT=$(tmux -S "$WORK/tmux" capture-pane -p -J -t agy 2>&1); do
        if [ "$OUT" != "$previous" ]; then
            printf 'agy: %s pane:\n%s\n' "$what" "$OUT"
            previous=$OUT
        fi
        complete=1
        for text in "$@"; do
            case "$OUT" in
                *"$text"*) ;;
                *) complete= ;;
            esac
        done
        [ -z "$complete" ] || return 0
        # interval: a look at the pane each second until it draws
        sleep 1
    done
    fail "$what: the pane could not be read: $OUT"
    return 1
}

# row_cleanup: stop the row's tmux server, and the program in it, on any
# exit of the row.
row_cleanup() {
    tmux -S "$WORK/tmux" kill-server 2> /dev/null || true
}

row() {
    output "agy --version" agy --version
    same "the version" "$OUT" "$VERSION"
    same "the program" "$(readlink -f "$(type -P agy)")" "/opt/agy/bin/agy"
    has "the terms" "$(cat /opt/agy/TERMS)" "https://antigravity.google/terms"
    # shellcheck disable=SC2016 # the login shell expands the variable
    output "a login shell" env DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent \
        bash -lc 'printf "%s" "$AGY_CLI_DISABLE_AUTO_UPDATE"'
    same "the updater's setting in a login shell" "$OUT" "true"
    output "agy --help" agy --help
    has "agy --help" "$OUT" "Usage of agy:"
    has "agy --help" "$OUT" "--dangerously-skip-permissions"
    [ -z "$NATIVE" ] || return 0

    mkdir -p "$WORK/home"
    output "tmux" tmux -S "$WORK/tmux" new-session -d -s agy -x 120 -y 40 \
        "env HOME=$WORK/home AGY_CLI_DISABLE_AUTO_UPDATE=true agy"
    if screen "the sign-in menu" "Select login method" "1. Google OAuth"; then
        has "the sign-in menu" "$OUT" "1. Google OAuth"
        tmux -S "$WORK/tmux" send-keys -t agy Enter
        if screen "Google OAuth" "authorization code" \
            "https://accounts.google.com/o/oauth2/auth?" "paste the authorization code below"; then
            has "Google OAuth" "$OUT" "https://accounts.google.com/o/oauth2/auth?"
            has "Google OAuth" "$OUT" "paste the authorization code below"
        fi
    fi
    tmux -S "$WORK/tmux" kill-server 2> /dev/null
    exited "agy's processes after tmux ends" pgrep -x -u "$(id -u)" agy
}
