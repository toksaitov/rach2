# Milestone 3: the text and documentation standard

This milestone brought every piece of written language in the repository to one standard: the prose in the Readmes, the comments in the source, and the messages the tools and build scripts print. It set the standard in `CLAUDE.md` (and `AGENTS.md` for Codex), then refactored the existing tree to meet it and added a mechanical gate so it cannot regress.

It was built from `plan.md`, executed one phase at a time under the `/goal` runner, with a harsh independent Codex review at each phase. The phases ran from the policy and the advisory checker (0) through punctuation and Markdown (1), vocabulary and voice (2), messages (3), the high-traffic Readmes (4), the foundation and the newer C comments (5 and 6), the header audit (7), and finally flipping the text-style check from advisory to a hard gate (8). Each phase changed text only, proved no behavior change, and ended in one scoped commit.
