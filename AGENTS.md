# AGENTS.md

Follow `CLAUDE.md`. It is the project policy for the build, writing, comments, messages, ports, verification, and git.

New here? For what Rach2 is, the build and run quick start, and the directory map, read `Readme.md` first. The per-milestone design and build history, and the current state of the tree, live in `dev-log/` (start at `dev-log/Readme.md`, whose last entry is where the tree stands now). Each component has its own `Readme.md`; read the owning one before changing its code. Run `make doctor` to check the toolchain and `make help` to list the targets.

The rule this project breaks most often is in `CLAUDE.md` under Agents, and it is worth repeating where it will be seen: never tell the user a delegated job is running without a command in the same turn that reads its state. Three reports of a review "still running" in one session were all of jobs that had already died. The bound on a review's size lives there too, and only there.
