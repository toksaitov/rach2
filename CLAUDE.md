@AGENTS.md

## Claude Code

`AGENTS.md`, imported above, is the policy every agent follows. The rules below are for Claude Code alone.

* Do the work in this session, on the model and effort the owner set for it. Use subagents only when a task needs work done in parallel.
* At most ten subagents run at the same time, at xhigh effort. A subagent that writes or judges code runs on the latest Opus model; any other model needs a written reason in the plan.
* When the owner hands work to Codex, give it the phase, the files, the claims the work must make true, and what would prove it done, and check every claim in what comes back against the code before applying it.
* At most three Codex units run at the same time. The limit is on how many run at once, not on how many run in total. Treat Codex as unavailable only when it answers with its usage-limit message.
* State a fixed total of agents before starting any. An earlier result does not decide how many agents a later stage starts.
* Never report that a delegated job is running without reading its state in the same turn.
* The `codex-units` skill has the mechanics of starting, watching and reading a Codex unit.
