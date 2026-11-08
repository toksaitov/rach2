---
name: codex-exec
description: Run a job in Codex through its command-line program, `codex exec`. Use for a review of a plan, a milestone or a fix, for a question when stuck, and when the owner asks to hand Codex a task.
---

# Codex jobs

* Run Codex with `codex exec`; the Codex plugin for Claude Code is not used. `codex exec -s read-only -c project_doc_max_bytes=65536 -C DIR --json -o ANSWER "PROMPT" < /dev/null > LOG 2> ERR` reads and reviews without changing a file; a job that changes files runs with `-s workspace-write` in a git worktree of its own. Close its input, as `< /dev/null` does: `codex exec` otherwise waits for more of the prompt on stdin. `-c project_doc_max_bytes=65536` raises Codex's limit on AGENTS.md from 32 KiB, so the whole file is read should it grow past that. Web search needs the top-level flag: `codex --search exec ...`.
* `~/.codex/config.toml` sets the model, `gpt-6-astra`, and the effort, `xhigh`, and one `gpt-6-astra` job runs at a time; a plan's review and a milestone's review are each one. A job that needs less names `-m gpt-6.1-sol -c model_reasoning_effort='"high"'`. A refusal is a refusal of that request, not a fact about the account: `-m astra` is refused because the name is wrong.
* Put a long job's task in a file and tell it to reread that file after a compaction. Start each round of a discussion as a fresh `codex exec` that reads the earlier answers from their files, not as a resume of a long thread. Read the answer before trusting a status of 0: two jobs that lost their task at a compaction ended with a one-line non-answer and status 0.
* A job starts subagents only when its prompt allows it. `-c agents.max_threads=N` allows N threads at a time, the lead's among them, so N-1 subagents, and `-c agents.max_depth=1` keeps a subagent from starting its own; `-c agents.default_subagent_model='"MODEL"'` and `-c agents.default_subagent_reasoning_effort='"EFFORT"'` set theirs. The session file of each thread under `~/.codex/sessions` records the model it ran.
* `--output-schema FILE` makes the last message JSON in the form the JSON Schema FILE gives, and `-o ANSWER` writes that message to ANSWER.
* Run a long job in the background with the tool's own notice when it ends, and read its answer before reporting it.
