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
# session in the clone it is in. MESSAGE is its prompt, and the steps below
# are added to its system prompt, so that the owner who opens the session
# from the push reads the message first. The session reads the issues that
# MESSAGE names and writes out the problem for the owner; two and a half
# minutes after it starts, it sends MESSAGE as the push, and a caller that
# waits for an answer counts from then. It then stays open, so that the owner
# can ask about the message. The session gets this script's PATH, since a
# tmux session gets the environment of the tmux server, which may not find
# `claude`. The next notify.sh replaces it; `tmux kill-session -t
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
steps="This session carries a notice to the owner of Rach2. The owner's first message here is the notice, which also goes to the owner's phone as a push. The owner opens this session from the phone and has seen nothing else: not the logs, not the issues, not the session that sent the notice.

1. Start \`sleep 150\` with the Bash tool in the background. Claude Code sends no push in the first minutes after a session starts, so the push waits for the timer.
2. While the timer runs, read every issue the notice names with \`gh issue view N --comments\`, and run \`sh tools/flow/status.sh\` when the notice is about the loops, ci, the disks or a release. Then write your reply to the owner. Say what happened or what is broken, in plain words, and its cause; what it stops or costs, and since when; and what the owner is asked to decide or do, with the options, the one you recommend, and where to answer: on the issue, or here. For an incident, say which fix was chosen and when it is applied unless the owner comments \`stop\` on the issue. Write as the section Writing of AGENTS.md says: short plain sentences, project terms explained or replaced by plain words, times in UTC+6, and each issue number beside what the issue is about. Say nothing about these steps, the timer or the push.
3. When the timer's notice arrives, call the PushNotification tool once, with the status \"proactive\" and the notice's text exactly as its message, and end the turn without text.

Then wait for the owner, who may ask about the notice. Answer from the repository, the board (\`gh\`) and \`sh tools/flow/status.sh\`, and change nothing unless the owner asks."
tmux kill-session -t rach2-incident 2> /dev/null
# The prompt comes first, since --allowedTools takes every word after it.
tmux new-session -d -s rach2-incident -x 200 -y 50 -e "PATH=$PATH" \
    claude "$*" --remote-control "rach2 incident" \
    --append-system-prompt "$steps" \
    --allowedTools 'Bash(sleep:*)' 'Bash(gh issue view:*)' \
    'Bash(sh tools/flow/status.sh:*)'
