# AGENTS.md

Follow `CLAUDE.md`, the project policy for the build, writing, comments, messages, ports, verification, and git.

Read `Readme.md` first for the project overview, build and run instructions, and directory map. The design and build history is under `dev-log/`; its [index](dev-log/Readme.md) ends with the milestone that produced the current tree. Read a component's `Readme.md` before changing its code. The build checks its own tools before compiling; `sh tools/doctor.sh` is the wider machine-setup report, and `tests/run NAME` runs the test suites.

The most frequently broken agent rule is repeated here: never tell the user that a delegated job is running without reading its state in the same turn. Three reviews reported as "still running" in one session had already died. `CLAUDE.md` defines the review size limit.

For a new machine, `dev-log/26-portable/arrival.md` lists what a clone does not provide: credentials, the machine's SSH host key, the Doom WAD, the build tree, and GitHub authentication. `Readme.md` has the fresh Mac bootstrap instructions.
