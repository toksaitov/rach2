---
name: codex-exec
description: Run a job in Codex through its command-line program, `codex exec`. Use when the owner asks Claude Code to hand Codex a review or a task.
---

# Codex jobs

* Run Codex with `codex exec`; the Codex plugin for Claude Code is not used. `codex exec -s read-only -C DIR --json -o ANSWER "PROMPT" < /dev/null > LOG 2> ERR` reads and reviews without changing a file; a job that changes files runs with `-s workspace-write` in a git worktree of its own. Close its input, as `< /dev/null` does: `codex exec` otherwise waits for more of the prompt on stdin.
* `~/.codex/config.toml` sets the model, `gpt-6-astra`, and the effort, `xhigh`, and one `gpt-6-astra` job runs at a time; the reviewer loop's review is one. A job that needs less names `-m gpt-6.1-sol -c model_reasoning_effort='"high"'`. A refusal is a refusal of that request, not a fact about the account: `-m astra` is refused because the name is wrong.
* A job starts subagents only when its prompt allows it. `-c agents.max_threads=N` allows N threads at a time, the lead's among them, so N-1 subagents, and `-c agents.max_depth=1` keeps a subagent from starting its own; `-c agents.default_subagent_model='"MODEL"'` and `-c agents.default_subagent_reasoning_effort='"EFFORT"'` set theirs. The session file of each thread under `~/.codex/sessions` records the model it ran.
* `--output-schema FILE` makes the last message JSON in the form the JSON Schema FILE gives, and `-o ANSWER` writes that message to ANSWER.
* Run a long job in the background with the tool's own notice when it ends, and read its answer before reporting it.
