#!/bin/sh
#
# notify.sh: send MESSAGE to the owner's phone through the Claude app.
#
# Usage: notify.sh MESSAGE
#
# Claude Code sends a push to the phone only from a session with Remote
# Control on, and only while nobody is typing in that session's terminal. A
# headless session counts as watched, and so does one in the minute after
# the prompt it was started with. So this script starts an interactive
# session with Remote Control, named `rach2 incident`, in a detached tmux(1)
# session in the clone it is in, with a prompt that waits two and a half
# minutes and then sends MESSAGE; the push reaches the phone about that long
# after this script returns, and a caller that waits for an answer counts from
# then. The session then stays open, so that the owner can open it from the
# phone and ask about the incident. The session gets this script's PATH,
# since a tmux session gets the environment of the tmux server, which may not
# find `claude`. The next notify.sh replaces it; `tmux kill-session -t
# rach2-incident` closes it.
#
# It needs the Claude app on the phone, signed in to the same account with
# notifications allowed; Remote Control accepted on this machine, and `Push
# when Claude decides` on in Claude Code's /config; and this clone trusted,
# which running `claude` in it once and accepting the question does.

if [ $# -eq 0 ]; then
    echo "usage: notify.sh MESSAGE" >&2
    exit 2
fi
cd "$(dirname "$0")/../.." || exit 1
prompt="First run \`sleep 150\` with the Bash tool, in the foreground. Then call the PushNotification tool once, with the status \"proactive\" and this message: $*

Then wait for the owner, who may open this session from the phone to ask about the message. Answer from the repository, the board (\`gh\`) and \`sh tools/flow/status.sh\`, and change nothing unless the owner asks."
tmux kill-session -t rach2-incident 2> /dev/null
# The prompt comes first, since --allowedTools takes every word after it.
tmux new-session -d -s rach2-incident -x 200 -y 50 -e "PATH=$PATH" \
    claude "$prompt" --remote-control "rach2 incident" --allowedTools 'Bash(sleep:*)'
