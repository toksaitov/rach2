# Milestone 27: reading every file

## The goal

One person reads every document and every comment this project owns, judges it against the code it describes, and fixes what is wrong or badly written. Milestone 26 built the checks a script can make and found that they reach only part of the way: a machine can see a banned word and an over-wide line, and it cannot see a sentence that is well formed and false. Eight such sentences were found by hand in six documents at the end of that milestone, including a fresh-machine setup sequence that did not work. There are 511 files.

## How this milestone is run, which matters more than usual

`worklist.md` lists every file, in order, with a box beside it. The order is by reading value, so an interrupted run leaves the useful part done.

Work one entry at a time. Read the file whole, including the code the comments sit on. Fix what is wrong. Then mark the entry `[x]`, and only then start the next one.

That marking is the whole mechanism. This milestone is long enough that the conversation will be compacted many times, and after each one the worklist, not memory, is what says where the work got to. On resuming: open `worklist.md`, find the first unmarked entry, continue from there. Do not re-derive the position from anything else.

Commit every ten finished entries, in the house style, one line. `git log --oneline` is then a second record of the position, independent of the file.

## What may be used

Read files and edit them. That is the work.

Do not use `grep`, `rg`, `find`, `python`, or any other tool to survey, search, or summarise a file instead of reading it. The point of this milestone is that a person reads every line, because the errors being looked for are exactly the ones a search cannot describe.

Two commands are allowed, neither of which is analysis: `git commit` at each batch, and `make verify-style` after each commit. The second exists because a writing pass cannot check its own work and this one is 511 files long: without it a broken comment delimiter or an over-wide line rides along for hundreds of commits and becomes expensive to find. With it, any breakage is bisectable to ten files. If it fails, fix what it names before continuing.

## What is being judged

Style, against the rules in `CLAUDE.md`. The register rule, the comment rule, the message shape, ASCII, no em-dash, Markdown that is not wrapped to a column. The half of the register rule that a checker decides is already clean; the half a person decides is the reason for this milestone.

Correctness, which is the harder half and the point. A document states contracts: does the code still do that? A comment explains a function: is it still true of the function beneath it? Check the claim, not the grammar. Counts, file lists, paths, command lines, target names, and behaviour described in prose are all things that drift, and every one of them was found wrong somewhere in milestone 26.

Two failures to watch for specifically, because both were found in this tree: a document that describes a check as covering everything when it covers a subset, and a comment that describes the reasoning that produced the code rather than what the code does.

When a comment is right but reads badly, rewrite it. When a comment is wrong, fix the statement. When a comment explains something the code now says better by itself, delete it: `CLAUDE.md` says the body is made clear by naming and structure, not narrated.

## What is out of scope

The vendored trees, and `ports/doom/src`. Nothing in `worklist.md` is third-party; if a path looks like it might be, it is not on the list.

`dev-log/` records, including this plan. They are evidence of what was decided and when, and rewriting them would falsify the record. `dev-log/Readme.md` says so.

Code. This milestone changes comments and documents. A file where the code itself is wrong gets the comment fixed and the code left alone, with a line in the record naming what was found; milestone 28 is where behaviour changes.

## Done when

Every entry in `worklist.md` is marked, `make verify-style` passes, and `dev-log/27-prose/Readme.md` records what was found: the claims that turned out to be false, the comments that described reasoning rather than behaviour, and anything noticed about the code that this milestone deliberately did not change.
