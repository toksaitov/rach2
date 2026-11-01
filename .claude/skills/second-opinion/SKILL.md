---
name: second-opinion
description: Ask agy (Gemini 4 Argon) or Muse Code (Muse Wonder Full) a question or for a review through their headless modes. Use when stuck, or when a hard question needs another view beside Codex and Fable.
---

# agy and Muse Code

* Both are signed in only in the owner's accounts on the Mac, on basic plans with low limits. Ask them when stuck or for a second opinion on a hard question, not as routine reviewers: one job at a time, a narrow prompt, and a read-only job; work that changes files goes to a worker loop or Codex. Treat one as unavailable only when it answers with its usage-limit message; `muse usage` shows Muse's allowance.
* agy, Google's Antigravity CLI, answers one prompt with `agy -p "PROMPT" --mode plan --output-format text < /dev/null > ANSWER 2> ERR`, started in the checkout; plan mode reads and does not edit. Its default is Gemini 4 Argon at medium effort (`~/.gemini/antigravity-cli/settings.json`); a hard problem passes `--model gemini-4-argon-high`, and `agy models` lists the others. The prompt tells it to read `AGENTS.md`.
* Muse Code answers one prompt with `muse exec --workspace DIR --trust-workspace --disable-write "PROMPT" < /dev/null > ANSWER 2> ERR`, or `--prompt-file FILE` for a long prompt. `--trust-workspace` loads `AGENTS.md` as its rules (it ignores `CLAUDE.md` beside it), and `--disable-write` keeps it from editing. Its default is Muse Wonder Full, `muse-wonder-full-1.0`, at high effort (`~/.config/muse/settings.json`); a hard problem passes `--reasoning-effort xhigh`.
* The prompt gives the milestone, the files, the question and what would settle it, and says to use no subagents unless the problem needs them. Check every claim in the answer against the code before using it.
* Run a long job in the background with the tool's own notice when it ends, and read its answer before reporting it.
