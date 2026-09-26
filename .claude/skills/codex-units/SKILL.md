---
name: codex-units
description: Start, watch and read Codex units through the Codex companion script. Use when handing work or a review to Codex.
---

# Codex units

* The companion script is `~/.claude/plugins/cache/openai-codex/codex/VERSION/scripts/codex-companion.mjs`. `node SCRIPT task --background "PROMPT"` starts a unit and prints its id, `status ID --json` gives its state, `result ID` its answer, and `cancel ID` stops it. Read the state in the same turn as any report that the unit is running.
* `~/.codex/config.toml` sets the model, `gpt-6-astra`, and the reasoning effort, `xhigh`. Pass no `--model` and no `--effort` unless the owner names others, and spell them as the configuration or a milestone's record does. A refusal is a refusal of that request, not a fact about the account: `--model astra` is refused with `the 'astra' model is not supported when using Codex with a ChatGPT account`, though only the name is wrong.
* A unit that changes files needs `--write`; without it the sandbox is read-only and the unit only reports.
* A unit that changes files runs with `--cwd` in a git worktree of its own, because units that share a checkout overwrite each other's objects in the middle of a build. `status`, `result` and `cancel` for that unit need the same `--cwd`.
* A unit, like any delegated review, covers at most six files and runs at most ten minutes. These limits are fixed. One reviewer is assigned to a diff.
* Ask a reviewer to state the most serious findings first. Do not ask it to write or tee a file; its sandbox refuses. Do not launch it where a tool timeout can kill its process group.
