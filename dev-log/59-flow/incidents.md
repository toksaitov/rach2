# Incidents of the loops, 2026-10-15 and 16

The faults that the process of `AGENTS.md` showed in its first two days with the worker, manager and reviewer loops, each with its cause, its fix and the rule that now covers it. Times are UTC.

## The night

* The scheduled night did not start. `night.yml` was due at 19:00 on 2026-10-15, its first night, and GitHub created no run; the repository has no scheduled run of any workflow. GitHub delays or drops scheduled runs due at the start of an hour under load, and f4aee4647 (#442) moved the night to 19:17.
* Two nights were started by hand with work outstanding. The one of 16:06 on 2026-10-15 ran while #262, #270 and #338 waited in Backlog, and the one of 20:06 ran before the review of phase #73, which then filed fifteen findings. Both were cancelled, at 17:04 and 20:18. ab1be006a makes a night started by hand wait until no open issue is left but phases, `owner` issues and the tasks of unreleased plans, and until every phase whose issues have closed has had its review and the review's issues have closed too.
* The cancelled night was filed as two failures, #407 and #410. c2c9e7d23 has `report.sh` file nothing for a cancelled run and gives each architecture a time limit of its own, so that an architecture that overruns is still reported as unfinished; 5c4302f6c (#412) stops it at that limit with timeout(1) and lets `tests/run` stop its machines first.
* The night of 21:34 on `d2905082` failed `doom` on both architectures, because neither ci runner had `ports/doom/doom1.wad`, which Git does not track; a comment in the Makefile said that the ci build had none. The verified shareware WAD was copied into both runners' checkouts, which keep it since every workflow checks out with `clean: false`, and 147088a3b (#443, #444) records it in `tools/Readme.md`.
* Nothing started that night again. Its fix landed at 00:13 on 2026-10-16 and ci passed at 00:40, but no rule said that a failed night runs again, and the Lenovo update was the owner's alone, so the team began the day at 09:00 UTC+6 without a commit that a whole night had passed. The night was started again by hand at 04:05. 65dcebc9f has the manager start a failed night again once its `ci:` issues have closed, build the commit of a whole night from clean and update the Lenovo to it before 09:00 UTC+6, and makes a missed morning a major incident; daf218d72 adds `tools/dev/machine_update.sh`, which installs, tries and commits the kernel; and 6433c3d36 lets an issue labelled `intermittent`, which stays open by rule, stop neither the restart nor a night started by hand.
* The first form of that rule had the manager build with `make distclean`, which rebuilds the GNU system: 67 minutes on the testing host, past the forty minutes a check may run, so the build could never finish. 4b4fc72a1 builds after `make clean`, which keeps the vendored builds, as the `milestone` skill's build from clean does: 59 seconds for i686 and 303 for `make machine` on x86-64, measured on 2026-10-16.
* Running `doom` again for that night showed that the suite rules that stage os-real's manifest did not build e2fsprogs first, so a suite failed in a tree whose architecture had not been built (#445, 795d177bb). ci did not see it, since its runs build everything before any suite.

## The board

* The queue was reported empty while three issues waited in Backlog. The manager had left #262, a GRUB regeneration whose procedure a Readme describes, for the owner, and #338 had no `bug` label, so triage never read it. cbdad4b34 and the rules of #411 leave for the owner only the decisions `AGENTS.md` names, under the label `owner` with one question sent to the owner's phone; the manager triages any other issue it finds in Backlog, and `status.sh` counts each kind.
* Finished work waited unpushed while a `hold` was open, because sessions handed back issues whose commits needed only the push. 43dc29be6 (#377) keeps such an issue with its worker, whose loop starts no session on it until the hold closes.
* Two closed issues stayed out of Done: #444 in Ready, released by its worker as it closed, and #417 in In progress, taken as it closed. `board.sh done`, which moves every closed issue to Done, existed and nothing ran it. #450 has `release` and `take` leave a closed issue in Done and the manager run `board.sh done` at each check.

## The loops

* A worker loop backed off after every session shorter than two minutes, from one minute doubling to thirty, even when the session had closed its issue, so a run of short document fixes slowed each worker. 53efb9049 (#441) backs off only while the issue is open.
* The usage-limit check read a session's own text about limits as a limit, and the loops of m, s and v waited fifteen minutes four times on 2026-10-15 after sessions that ended with status 0. bc85a1faf (#449) reads the message only from a line that reports an error.
* `notify.sh` delivered no message. It gave the prompt after `--allowedTools`, which takes every word after it, so the incident sessions of #371 and #447 started with no prompt, and the owner's phone showed a push without text. e77a49e07 (#448) gives the prompt first, and the notice for #447 was sent again at 04:36.

## Reports

* The manager's day report, #446, gave 35 finished core runs, 28 passed and 155 cancelled, where `gh run list` gives 50, 43 and 179, and called #331 to #369 the findings of the review of phase #72, though the range also holds ci issues, phase #353 and its tasks, and bugs that workers filed. A comment on #446 corrects both.

## The owner's session

* It reported the queue empty without reading Backlog, started the night twice with work outstanding, the second time with the review of #73 still to run, and said that #262's regeneration would not affect the night, though it changes the boot code that the suites run.
* After the night of `d2905082` failed, it wrote the morning report and stopped, where it should have run the night again and updated the Lenovo; the manager now does both.

## Open

Nothing. The night of 04:05 on 2026-10-16, on `795d177b`, failed `selfbuild` on i686, where `musl/pwauthmusl`'s session leader was killed by the hangup of its terminal; 95392e1d8 (#452) reaps it first, and the night of 06:49 passed whole. 1faac41ec (#450) settled the closed issues outside Done. The night itself was replaced by a full run per phase; phases.md records why.
