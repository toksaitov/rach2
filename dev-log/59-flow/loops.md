# The loops

## Why

On the first day of the process, 2026-10-14 and 15, the work stopped in four ways, and each time a person had to notice:

* The manager was the owner's interactive Claude Code session. A check ran only when a turn ran, and a turn that ended with nothing to wake it left the process unmanaged until the owner wrote; a command started with `&` sends no notice when it ends, and a cron task fires only while the session is idle. The owner found the process stopped more than once.
* A worker session in print mode stayed open until a wakeup it had scheduled fired: 6 of 28 sessions waited 16 to 26 minutes after their work was done.
* Print mode stopped a helper still running ten minutes after the session's last turn, until `CLAUDE_CODE_PRINT_BG_WAIT_CEILING_MS=0` turned that off.
* A worker waited 38 minutes for its suites with `until ! pgrep -f 'tests/run style host'`, a loop that matched its own command line.

The design follows the published practice for agents that run for days, as Codex reviewed it: each piece of work is a process that ends, a plain loop outside any session starts the next, the state a session needs to resume is in files, and finding a fault is kept apart from repairing it.

## What changes

* `tools/flow/worker.sh` runs a worker's sessions, Claude Code or Codex, and claims the issue itself; `tools/flow/manager.sh` runs one headless check at a time, thirty minutes apart, each under a deadline. Both run master's copy of themselves.
* The owner's computer runs a Claude Code worker, a Codex worker (`gpt-6.1-sol` at high effort) and the manager, with a copy on the testing host for each worker.
* An incident reaches the owner's phone through an interactive session with Remote Control, since a headless one sends no push.
* `AGENTS.md` forbids waiting by polling the process table.

## What tells us it works

* No session idle for more than twenty minutes without a running test, in the manager's log.
* A manager line every thirty to forty minutes, with no gap, whether or not anyone is at the owner's computer.
