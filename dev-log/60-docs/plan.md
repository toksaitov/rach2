# Milestone 60: the documentation, rewritten from the code

## Why

The documentation grew one milestone at a time. Each change added a paragraph where it was needed and none removed one, so `Readme.md` is 2700 words of image contents, hardware notes, milestone history and reference, with no order a reader can follow, and the component Readmes and the code comments drifted the same way: stale facts, history where a contract belongs, and no common outline. This milestone rewrites every in-house Markdown document and code comment from the code as it is now, and adds the few rules that keep it from drifting again. It is also the first work done through the process of `59-flow`.

## What other projects do

* Front pages are short and point elsewhere. Go, Rust, LLVM and curl say what the project is in their first lines and take 200 to 300 words; FreeBSD adds a table of its top-level directories; xv6 and egos-2000, both written to be read, say so and send the reader to the code or the book. The long front pages, ToaruOS and SerenityOS, are long because of history and author lists.
* Each document has one purpose. Diataxis names four, tutorial, how-to, reference and explanation, and finds that mixing them is the common fault; the operating systems keep reference in manual pages, explanation in papers or a book, and history in change logs.
* A component's document has a fixed outline. Kubernetes enforces one per page type, and mdoc(7) fixes a manual page's section order, which `mandoc -T lint` checks.
* A comment states the contract, not the implementation, so that only a change to the contract can make it wrong, and that change is in the same diff (Linux coding style and kernel-doc, the GNU standards, Google's C++ guide, style(9), Go's doc comments). Fluri and others found that 90 to 97 percent of comment changes land in the same revision as the code they describe.
* What keeps documents true is the same commit, a check against a list the code already has (curl checks that every option has a page, as `mancheck` does here), and deleting what cannot be made true. Large lint rule sets and per-document owners are sized for teams of technical writers; the Linux kernel's build prints so many documentation warnings that they are ignored.

## The front page

`Readme.md` is the project's front page, read by a colleague who has never seen it. It is about 500 to 700 words, in this order:

1. What Rach2 is and for whom, in one paragraph: an operating system written to be read, on both architectures, and what it runs, in plain words with numbers from `facts.md`.
2. What a person can do on it, as a short list, each item linking to the component or manual page that has the detail.
3. What runs on it: the static musl system every image has and the GNU system `vendor/gnu/` builds, a sentence or two each, then links to the lists of programs (`man/programs.md`), system calls (`rach2/syscalls.md`) and hardware (`rach2/hardware.md`).
4. Status and the major limits, in a few sentences.
5. Quick start: what the host needs (with a link to `tools/Readme.md`), the commands from a clone to a booted system in QEMU, and how to log in. Testing is a link to `tests/Readme.md`, not a step of the first boot.
6. The source and its documentation, in one table of the top-level directories with one line and the Readme of each, followed by a sentence each on `man/`, `dev-log/` and `AGENTS.md`, and one sentence on licenses that links the vendored projects in `man/programs.md`.

The lists themselves, image contents, hardware procedures, feature inventories, the reading route through the kernel (which belongs in `rach2/Readme.md`) and milestone history do not belong on it; each goes to the component, manual page or record that owns it.

## The component documents

Every other in-house Readme opens with a summary paragraph under its title, with no heading: what the component does and why it exists. Its `##` sections then come from one list, in this order, and a document has only the sections it needs:

* `## Use` (programs and tools) or `## Running` (tests): what a person types and what happens.
* `## Results` (tests): how to read what a run prints.
* `## Interfaces` (kernel subsystems and libraries): what callers and other components see, linking to manual pages rather than repeating them.
* `## Sources and license` and `## Local changes` (ports): where the pinned source comes from, its license, and what Rach2 changes in it.
* `## Design`: the data structures and the main paths in reading order, with file names, and the reason for each choice that is not obvious.
* `## Building`: build steps of the component's own.
* `## Updating` (ports): how a new upstream version is brought in.
* `## Adding tests` (tests).
* `## Limits`: what it does not do, stated as present facts.
* `## Tests`: the suites and host tests that cover it, and how to run them.

So a kernel subsystem reads Interfaces, Design, Limits, Tests; a program Use, Design, Building, Limits, Tests; a port Sources and license, Local changes, Building, Updating, Tests; and `tests/Readme.md` Running, Results, Design, Adding tests. `###` subsections are free. Policy files (`AGENTS.md`, `CLAUDE.md`, the skills), generated documents such as `tests/inventory.md`, and lists such as `tools/verify/exceptions.md` keep their own form.

A document states what the code does now, and a fact has one home: a count, a table or a contract is written where it is owned and linked from elsewhere. History, milestone numbers and the reasons a design was abandoned belong in `dev-log/` and the commit messages. A document that cannot be made true is deleted, and two that cover one component become one.

## Comments

The rules of `AGENTS.md`'s Comments section stand, and the rewrite applies them to every in-house source file:

* A file begins with a comment of a line or two on what it is for.
* A function has a `/**` block before it that states its contract: what it does, an argument whose meaning is not obvious, the return values and errors, ownership and lifetime, and the locking and context it requires. A contract has one home: the declaration in a header for a function other files call, and the definition otherwise, where the reasoning of the implementation also goes.
* A structure member has a comment when its units, invariants or special values are not obvious.
* Inside a body, a comment is allowed only for a hardware or ABI fact or a deliberate hack, as one line.
* A comment does not narrate the body, give the history of the code, or argue with an earlier version. A comment that is already correct and clear stays as it is.

A comment-only change changes no code, and the writer shows it. The compiler cannot show it: the kernel and the host tests are built with `-g`, the sanitizers of the host tests record line numbers, and the images stage the sources, so all of them change with a comment. The proof is on the source instead. GCC's `-fpreprocessed` removes the comments of a C file and leaves its directives and code unexpanded, and with the white space reduced to one separator the file is a sequence of tokens. A comment-only change leaves that sequence the same in every C file it touches. A file that is not C, a script, a Makefile or a manifest, has no such proof; its diff must change only comment lines, and the writer reads it for anything a tool reads from a comment: a `shellcheck` directive, a `fallthrough` mark, a line continuation.

## Keeping it true

* A commit that changes a contract rewrites the Readme, manual page or `/**` comment that states it, as `AGENTS.md` already says. The phase review checks that every touched component's documents and comments are still true.
* `tools/verify/textcheck.sh` checks two things more in the component documents: their `##` headings come from the list above and in its order, and they cite no milestone of `dev-log/` by number. Both fail `tests/run style`, and both are turned on only when every component document conforms, so that no writer's check fails on another writer's file.
* The existing checks stay: links (`linkcheck.sh`), the manual's coverage (`mancheck`), the register (`prosecheck`) and ASCII.

## The work

The phase is #73; each task below is a sub-issue, sized for one worker session with its helpers, and no two touch the same file. `facts.md` is the fact base: the counts, the directory map, the faults found in each document, the comment samples, the text the rewrite must keep, and the units.

1. The rules: `AGENTS.md`'s Documentation section and this plan, written by the manager first.
2. The front page, `Readme.md`, with `.github/screenshot.png`.
3. The component documents, in ten issues by directory. A large document is an issue of its own.
4. The comments, in the nineteen units of `facts.md`, K1 to K9 for the kernel first, then U1 to U3 and T1 to T7. A unit's documents are the issue of step 3 for the same directory, so a writer reads the other's work once it has landed.
5. The two checks in `textcheck.sh`, once every component document conforms.
6. The edit: the whole set read as a new reader would, for contradictions, repetition, links and reading order, and fixed where it fails. The component documents are three issues for the workers, by part of the tree; the manager edits the policy files, `AGENTS.md`, `CLAUDE.md` and the skills.

A document task and a comment unit are checked by `tools/verify/doccheck.sh`, run in a built tree on the testing host with the change in place. It compares each changed C file's tokens with those of `origin/master` once the comments are removed, keeping the inside of every string literal as it is, reads every other changed file for a line that is not a comment, and runs the document checks of `tests/run style`.

The writer puts its last line, `doccheck: result: pass`, in a comment on the unit's issue. A unit that has to change code as well is checked as code, as `AGENTS.md` says.
