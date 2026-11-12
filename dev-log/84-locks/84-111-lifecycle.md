# 84-111: the lifecycle of a release's Windows parts and WSL claims

This is the model that the release's Windows parts and WSL claims are written from, since 84-122. It lists the states that a part of a Windows release run and a claim of WSL's admission can be in, which process and which file own each state, every transition with the guard it checks, and then places on it each finding that the earlier model left open: j79's eleven, and those of j55, j68, the audit in #4783 and Muse Code's review in 84-111b that the new model removes, the seven races that review j90 found in 84-122's first form, which 84-134 closed, and the four faults that review j102 found in 84-134, which 84-139 closed. The earlier model, which identified processes by number and start time, killed them through handles and groups, and marked cancellation with files, is in git history before 84-122; 84-122's design round (Fable, on #4820) found that five rounds of review had not closed it, and replaced it with kernel objects that end with their processes.

Words: OUT is Windows' output directory, `D:/rach2/build/release`, which WSL sees as `/mnt/d/rach2/build/release`; TREE is Windows' copy, `D:/rach2`; LOCKS is `build/flow/claims` of the owner's clone in WSL, `/home/testing/rach2/build/flow/claims`, which is ext4; ID is a run's ID; PART is `prepare` or `cases`; KIND is `claim`, the preparation's claim, or `hold`, the whole host's claim for the cases, and N its attempt, 1 to 3; STATE is `build/flow/release` of the main working tree.

## The model

Liveness is a kernel object that ends with its processes: a named Job Object, `Global\rach2-ID.PART`, for a Windows part, and an flock of LOCKS/ID.KIND.lock for a claim in WSL. Permission to work is a token file: OUT/ID.PART.token for a part, LOCKS/ID.KIND.token for a claim. A stopper replaces the token whole, by rename, and then tests the object; a starter takes the object and then reads the token. A claim's token is published, in an ssh of its own that must succeed, before its runner is sent, and only where no token exists. A live grant is a lock of its own, LOCKS/ID.KIND.grant, which only the parent of the holding shell holds. A status file gives a part's result, never its end, and a free lock does not prove the ledger clean: a stop is confirmed only by the job absent, and a claim's end only by a recovered ledger that names none of its launchers. So a start in flight either holds an object that the stop then ends or waits for, or reads a token not its own and starts nothing. "Stopped" is the object absent or the lock free; every other answer, an ssh failure, a timeout or a state Windows cannot read among them, is "cannot say" and fails. No process number read from a file is signalled anywhere.

## The precondition

One manager and one STATE own Windows' copy and OUT. The gate's right to stop any run of OUT rests on that ownership, not on a test that the run's release is gone: a release under another STATE, or from another clone, that used the same copy would have its parts stopped by this one's gate. G13's locks are per STATE, so this is a rule of use, which `tools/flow/machines` keeps by naming one copy for the one release process of the main working tree.

## Host facts

Measured on the testing host for 84-122 (#4820): a process that WMI's Win32_Process.Create started makes a `Global\` job, joins it, and is found by name from a separate ssh session; etude's own jobs, `lock tree`'s and the cases' (tools/etude/win/hold.c, winexec.c), nest inside it; no process of a part leaves it, plain, `setsid`, double-forked and `nohup` MSYS2 programs, `cmd`, and PowerShell's Start-Process among them, and none is left after a stop; LOCKS is ext4 (OUT, under `/mnt/d`, is 9p, where flock does not hold), and an exclusive non-blocking flock is refused there while a shared one is held; perl 5.40, flock and setsid are in WSL.

## Processes

| Name | Process | Where | Started by | Stopped by |
|---|---|---|---|---|
| R | `release.sh M SHA`, with or without `-r`: the release | Mac | the manager, in tmux | TERM, INT, HUP and EXIT run `windows_stop` once `windows_start` has set the traps; KILL; a Mac reboot |
| Rp | `release.sh -p M SHA` | Mac | the manager | TERM, INT, HUP, EXIT (trap `prepare_stop`), KILL |
| A | `release.sh -a windows M SHA` | Mac | the manager | ends once it has started W |
| W | `release.sh -w ID M SHA`: a Windows run of -a, leader of its own process group | Mac | A, by `perl setpgrp` and `nohup` | TERM and EXIT (trap `windows_stop`), KILL; R's `windows_halt` sends TERM to its group |
| M | `release.sh -m ID M SHA` or `-p -m`: the Mac's run, leader of its own group | Mac | R or Rp | `mac_stop` of a later R; Rp's `prepare_stop`; stop.sh -r |
| Obs | `windows_detach`'s observer, a background subshell of R, W or Rp | Mac | `windows_prepare`, `windows` | `kill` and `reap` from the parent's trap |
| Prep | `windows_prepare`, a background subshell of R, W or Rp | Mac | `windows_start`, `prepare_all` | `kill` and `reap` from `windows_stop` or `prepare_stop`; its own TERM trap |
| Beat | `beat ID`: touches ID.alive in WSL each minute | Mac | `windows_prepare`, `windows` | `kill`; ends once its release is gone, by G12 |
| S | a part's start: MSYS2 `bash -l -s` in the ssh session of `windows_detach` | Windows, in the session's job | Obs | the end of its ssh session, which ends every process of that job |
| H | the part's helper, `OUT/winjob-SUM.exe run Global\rach2-ID.PART TOKEN NONCE bash.exe -l OUT/ID.PART.sh`, which winjob.c builds | Windows, in the part's job, which it made | WMI, for S | its command's end, after which it terminates the job; a stop's `winjob stop`, which ends it with the job |
| B | the part's bash and everything it starts, etude-ID.exe and QEMU among them | Windows, in the part's job | H | the job's end |
| C | `winclean.sh`, after winjob.sh's lines: stop and clean (no flag), `-s` state, `-l` list, `-g` gate, `-a` gate for no run | Windows, through ssh | `windows_clean` and `windows_gate` of R, W or Rp; stop.sh's `-a`; sweep.sh's own lines | ends by itself, within twenty minutes, which its callers' alarm bounds |
| K | a claim attempt's runner, `perl OUT/ID.KIND.N.pl`, which `claim_runner` prints, leader of its own session (`setsid`) | WSL | `claim_start` | its launcher's end; a change of the token |
| L | the attempt's launcher, `env ... etude-launch build env` or `alone`, K's child, which writes OUT/ID.KIND.N.owner before its exec and runs the holding shell once admitted; the ledger names it `owner PID START KNOWN` | WSL | K | K's TERM when the token changes, KILL half a minute later; the holding shell's end; TERM by PR_SET_PDEATHSIG when K dies first |
| G | the grant holder: L's command, a perl that takes the lock and the grant lock shared and runs the holding shell as its child, which inherits the lock and not the grant lock | WSL | L, once admitted | the holding shell's end; etude's keeper ends the case's group when L dies |
| X | `stop.sh [-r]` | Mac | the manager or the chat | ends by itself |
| Y | `sweep.sh [-a]` | Mac, with ssh to WSL and Windows | R at its close, stop.sh, the manager | ends by itself |

## Files

| File | Written by | Says | Removed by |
|---|---|---|---|
| OUT/ID.PART.token | S, the part's nonce, with noclobber, failing when the file exists; C, a new file holding `stop` renamed over it, before it terminates the job | whether a helper may start the part | Y at the milestone's close; Rp's cleanup keeps it |
| OUT/windows.current | C's `-g`, by rename: the one ID that may start; C's `-a`, `none` | which run's starts proceed | never; the next gate replaces it |
| OUT/ID.PART.sh | S, after its token and gate checks and before Create | the part's script | Rp's cleanup; Y at close |
| OUT/ID.PART.out | B (`exec >`) | the part's output; with no job and no status file, the part ran and ended | S before each start; Rp's cleanup; Y at close |
| OUT/ID.PART.rc | B, by rename, once its body's last line was `status N`, before it exits and before H ends the job | the part's status is N; not that it has ended | Rp's cleanup; Y at close |
| OUT/ID.PART.stopped | C, once `winjob stop` said the job is absent; S, when its gate check refused | the part has ended and is stopped for good | Rp's cleanup keeps it; Y at close |
| OUT/winjob-SUM.exe | winjob.sh's lines, built once from winjob.c, SUM being cksum's | the helper | none |
| OUT/ID.alive | B's alive loop each minute while B runs; Beat each minute while its release runs; the holding shell once at its grant | a claim of ID may hold | Y at close |
| OUT/etude-ID.exe and the run's logs, consoles and selection | B's body | the run's own files | Rp's cleanup; Y at close |
| TREE/build/tmp/ID | B's body (`mkdir`), its KEPT and record | the run's directory | B's body when it passed and kept nothing; C unless KEPT; Y unless KEPT or live, and whole at close |
| LOCKS/ID.KIND.lock | `claim_start`'s publication makes it; K opens it, never creating it, and holds a shared flock, which L inherits, and G takes, which the holding shell inherits | while held, some process of an attempt lives | `claim_stop`, once it holds it exclusively and the ledger is confirmed |
| LOCKS/ID.KIND.grant | `claim_start`'s publication makes it; G holds a shared flock of it while the holding shell runs | while held, an attempt is admitted and its command runs | `claim_stop`, with the lock |
| LOCKS/ID.KIND.token | `claim_start`'s publication, `go N`, by a link that fails when a token exists; `claim_stop`, `stop`, by rename | which attempt may work | `claim_stop`, with the lock |
| LOCKS/publish.lock | the first publication or `claim_stop`; each holds an exclusive flock of it, a publication for its whole ssh and `claim_stop` while it renames `stop` over the token | while held, a publication or a stop changes a claim's token | none, since a publication may hold it open |
| OUT/ID.KIND.N.pl, .started, .owner, .reaped, .held, .rc, OUT/ID.KIND.N | `claim_start` and K: the runner, K started L, L's number and start, K reaped L, L stayed in the ledger, L's status, K's output | the attempt's state | Rp's cleanup after `claim_stop`; Y at close |
| OUT/ID.claimed, OUT/ID.held (GOT) | the holding shell, once admitted | some attempt was granted | `claim_start`'s publication, once its token is in place and `claim_stop` has confirmed every earlier attempt; Y at close |
| OUT/ID.prepared, OUT/ID.end (DONE) | R, W or Rp | the claim may end | Y at close |
| STATE/pid | R and Rp, under STATE/lock: `PID START`, START being `ps -o lstart=` | the release that holds the lock | none; G12 rejects a stale one |
| STATE/windows.run | A: `PID START` of W | W | W's `windows_stop` when it names W; `windows_wait`; `windows_halt` |
| build/machines/mac/run | R or Rp: `PGRP ID` | M's group and run | `mac_wait`, `mac_stop`, `prepare_stop` |
| STATE/record lines `windows started ID`, `windows stopped ID` | `note` under STATE/record.lock | a run that was started; a stop ended it, so it counts as none of the three | -r's rewrite |
| ledger `/var/lib/rach2-etude/ledger/claims` | L, through etude | a claim or request with `owner PID START KNOWN` | L at its end; `etude status`'s recovery once the owner is dead |

## Guards

* J1, the job: a part's processes are born in its job, since H joins the job before it starts B, and no process may break away; the job's processes end when its last handle closes, which is H's, so a killed H takes the part with it; `winjob stop` terminates the job and waits half a minute for its last process. A part runs while `winjob state` says `active N`, and has stopped once it says `absent`; `unknown` is "cannot say".
* J2, the part's token: S writes its nonce with noclobber before anything else, so a part that a stop reached first never starts; H reads the token only after it holds the job, and starts B only while it holds the nonce; C renames a new file holding `stop` over the token before it calls `winjob stop`, and never truncates it, since S may have created it and not yet written its nonce: that write then lands in the file the rename unlinked. H reads the token sharing delete, and C tries a refused rename again five times. So a start in flight holds a job that the stop ends, or reads `stop`.
* J3, the gate's run: C's `-g ID` writes ID to OUT/windows.current, then lists every run of OUT with a token and no end by G4, and stops each but ID; `-a`, which stop.sh runs, writes `none` and stops each. S reads OUT/windows.current after it wrote its token and before Create, and starts nothing for any other ID. So a start that the gate did not list wrote its token after the gate's list, hence after the gate's write, and reads the new ID or `none`; no record of started IDs is needed, and none is lost when the record is replaced.
* J4, the helper's identity: `winjob run` refuses a job name that exists, so two helpers never share a part; the helper's program is named by its source's checksum, so a running one is never replaced.
* L1, the claim's lock: K opens the lock that the publication made, never creating it, and takes a shared flock before it reads the token, and clears close-on-exec so that L keeps it; G takes it again itself, since etude's keeper and bridge close inherited descriptors, and the holding shell inherits G's. While any process of any attempt that holds it lives the lock is held, so an exclusive non-blocking flock succeeds only once every one of them has ended or closed it; L3 covers what closed it.
* L2, the claim's token: `claim_start` first publishes `go N`, in an ssh of its own that holds LOCKS/publish.lock throughout, by linking a file to the token, which fails while any token exists, and sends the runner only once that ssh has succeeded; the publication then removes GOT and the attempt's files and makes the locks. K starts L only while the token holds `go N`, and sends TERM, then KILL half a minute later, to L, its own unreaped child, when it changes; the holding shell ends when it changes. `claim_stop` renames `stop` over the token while it holds LOCKS/publish.lock, so that every publication has ended whole before its token or starts after it and fails, and no publication's removal of GOT falls after a stop; it then waits up to two and a half minutes for an exclusive lock (L1), and at its end removes the token and the locks. A late K of an earlier attempt finds no lock, or reads `go N+1` or `stop`, and starts nothing. A late publication finds a token and changes nothing, or, after a confirmed stop, makes a token for an attempt whose runner was never sent, since its ssh did not succeed; the next attempt's publication then fails, and the attempt reads `unstarted`.
* L3, the ledger: L writes OUT/ID.KIND.N.owner, its number and its start from `/proc/self/stat`, before its exec, so before it can enter the ledger. K, once it has reaped L, touches .reaped, runs `etude status`, which drops a dead owner, and reads the ledger for L's `owner PID START` for a minute; it writes OUT/ID.KIND.N.held when L stays, and only then .rc, and releases the lock by exiting. `claim_stop`, once it holds the lock exclusively, has `etude status` recover the ledger, and fails when recovery fails, the ledger cannot be read, it names the owner of any attempt's .owner, or a .held exists. So a restart of WSL, which leaves no lock and an entry until recovery, and a K killed before it wrote .held, while a bridge that closed the lock's descriptor still holds the ports, are not confirmed.
* L4, a live grant: `claim_state` says `granted` only while GOT exists, the grant lock is held and .reaped is missing. G holds the grant lock only while the holding shell runs, and etude's launcher releases its admission only after its command, G, has ended, so the grant lock is free before the admission ends; the claim's lock is no test of a grant, since K holds it through its cleanup after L released its admission. With the lock free it says `dead` when GOT exists, since the holder has ended, `refused` when .rc exists without GOT, `unstarted` without .started, and `dead` otherwise. A lock it cannot read gives nothing, as a wait does.
* L5, K's death: L carries PR_SET_PDEATHSIG of TERM, on which admission withdraws its request, so a K killed while L waits leaves no request in the ledger; L holds the lock until it ends, so the claim reads neither free nor dead before.
* G4, the status file: OUT/ID.PART.rc gives a part's status and not its end: B writes it before it exits, and H terminates the job only after that, so a paused H leaves a live job beside it. A part has ended once its job is absent; C's list and gate count it ended only once OUT/ID.PART.stopped exists, which a stop of an absent job writes at once, and Obs reports a status only once the job is absent too, so that R ends the claim in WSL only after the job.
* G6, a confirmed stop is final: C writes OUT/ID.PART.stopped only once the job is absent; etude runs inside the part, so no separate check of etude is made.
* G7, IDs are not reused: each run gets a new ID from `stamp`, and only W's first run takes the ID that A recorded.
* G11, claims follow the parts: R, W and Rp end a claim of ID only after C confirmed ID stopped, or when no part of ID was started; otherwise the claim stays and ends by itself ten minutes after the last touch of OUT/ID.alive.
* G12, a release.sh process: a number from STATE/pid or STATE/windows.run is the release only while `ps` gives it the start the file records and shows it running tools/flow/release.sh, with ` -w ` for STATE/windows.run; M's group from build/machines/mac/run is the Mac's run only while its leader runs tools/flow/release.sh with `-m ID` on the same line. stop.sh and release.sh use the one test. Beat ends once R's number no longer has R's start.
* G13, locks: one R or Rp at a time per STATE, by STATE/lock; one A or Rp at a time, by STATE/again.lock; `note` and -r's rewrite hold STATE/record.lock.
* G14, bounds: every C that a trap runs is bounded by a twenty-minute alarm, and every `claim_stop` by a five-minute one over its whole ssh, `etude status` included; a call that an alarm ends counts as unconfirmed.
* G15, the claim sessions: stop.sh never signals a process whose session is a claim's: each session whose leader, in the same reading of ps, is a perl running OUT/ID.KIND.N.pl.
* G16, removal by result: C decides that a run's scratch is gone from the result of its last `rm -rf`, not from the count of tries.
* G19, stop's kills at the kill site: stop.sh selects and kills in one pass on each machine; on Windows one PowerShell command selects each program under D:\rach2 and kills it through its own handle, the helpers among them, whose death ends their parts' jobs.
* G20, cannot say is not stopped: C, `claim_stop`, stop.sh's `left` and sweep.sh take an ssh failure, a timeout, an unreadable state or a missing helper as "may still run": C exits 1, `claim_stop` fails, `left` prints `MACHINE: unknown: WHY` and counts it, and sweep.sh keeps the run's directory.

## The states of a part

| State | Shown by | What runs | Moved on by |
|---|---|---|---|
| P0 absent | no token | nothing | S |
| P1 claimed | token holds the nonce; no job | S, between its checks and Create | S; a stop writes `stop` (J2) |
| P1c start refused | token holds `stop`, from a stop before S (noclobber fails) or after S's gate check refused (with .stopped) | nothing | final |
| P2 helper starting | token holds the nonce; Create returned 0; no job yet | H, before it made the job | H |
| P3 running | job `active N`; token holds the nonce | H, B and everything B started | B's end, or a stop |
| P3r refused late | H made the job, then read `stop` | H exits; the job ends with it | final |
| P4 ended with status | .rc and the job absent; .rc with the job still active is P3 | nothing, once H has terminated the job | final; C's list counts it until P6 |
| P4x ended without status | job absent, .out, no .rc | nothing | Obs reports `lost`; C confirms |
| P4u did not start | job absent, no .out, no .rc | nothing | Obs reports `lost: it did not start`; C confirms |
| P5 stopping | token `stop`, `winjob stop` running | as before, being terminated | C |
| P6 stopped and confirmed | .stopped | nothing of the part, and none will start | final |
| P7 stop unconfirmed | token `stop`, no .stopped; job active or unknown, or C's alarm ended it | may still run | the next C |

The run ID as a whole is cleaned, R1, once both parts are in P4 or P6 and C has removed TREE/build/tmp/ID and the tooling paths its record says were made; it is kept, R2, while TREE/build/tmp/ID/KEPT exists, until Y at the milestone's close; and its files are removed, R3, by Rp's cleanup, all but the tokens and stopped files, or by Y at close.

## The states of a claim

| State | Shown by | What runs | Moved on by |
|---|---|---|---|
| Q0 absent | no lock file, or a free lock and no token | nothing | `claim_start` |
| Q1 sent | token `go N`, published and confirmed; no .started | maybe a K about to start | K, or `claim_stop` |
| Q1p published late | token `go N` of an attempt whose publication did not confirm; no runner was sent | nothing | the next publication fails; `claim_state` says `unstarted`; `claim_stop` |
| Q1u unstarted | as Q1 after `claim_start`'s half minute; lock free | nothing | `claim_state` says `unstarted`; `claim_stop`, then attempt N+1 |
| Q2 waiting | lock held, .started, no GOT | K, L as a request in the ledger | admission |
| Q3 granted | grant lock held, GOT, no .reaped | K, L, G, the holding shell | DONE, the age of ID.alive, or the token |
| Q3e ending | GOT, lock held, grant lock free or .reaped | L releasing its admission, or K checking the ledger | K's .rc and exit |
| Q4 released | lock free, .rc, GOT | nothing | `claim_stop` confirms the ledger and removes the locks and token |
| Q4d holder lost | lock free, GOT, no .rc, as a WSL restart leaves it, or .rc after GOT before the step began | nothing holds WSL | `claim_state` says `dead`; `claim_stop`, then attempt N+1 |
| Q5 refused | lock free, .rc, no GOT | nothing | `claim_state` says `refused`; the step fails |
| Q6 K killed | lock held by L, no .rc | L, which PR_SET_PDEATHSIG has sent TERM | L's end, then `dead` |
| Q7 stopping | token `stop`; `claim_stop` waits for the lock | the attempt's processes ending | `claim_stop` |
| Q8 stopped and confirmed | `claim_stop` exited 0; locks and token removed | nothing, and no ledger entry of any attempt's L after recovery | final |
| Q9 stop unconfirmed | `claim_stop` failed: the lock stayed held, recovery failed, the ledger names an attempt's L, a .held file, or the host did not answer in five minutes | something may hold; files kept | a person, as its lines say |
| Q10 left to expire | R did not stop it, by G11 | K, L holding while ID.alive is touched | Q4 ten minutes after the last touch |

## Transitions of a part

| # | Move | Trigger and actor | Guard | Leaves |
|---|---|---|---|---|
| T1 | P0 to P1 | S writes its nonce with noclobber | J2 | token |
| T2 | P0 to P1c | S finds a token: a stop came first | J2 | Obs writes `lost: it did not start` |
| T3 | P1 to P1c | S reads another ID in OUT/windows.current; it writes `stop` and .stopped | J3 | as T2 |
| T4 | P1 to P2 | S writes the script, builds the helper if missing, and calls Create with the nonce; it waits up to half a minute for the job or .out | J4 | the script |
| T5 | P2 to P3 | H makes the job, joins it, reads the nonce, starts B | J1, J2 | job |
| T6 | P2 to P3r | H reads a token not its own | J2 | the job ends with H |
| T7 | P3 to P4 | B's last line is `status N`; B writes .rc; H terminates the job; Obs reads the job absent | G4 | .rc |
| T8 | P3 to P4x | B ends without a status, or H is killed: the job ends with H | J1 | .out |
| T9 | P1 to P4u | Create failed, or the helper did not build; S writes `stop` and .stopped when the helper is missing | J2 | Obs writes `lost: it did not start` |
| T10 | any but P6, to P5 | C renames `stop` over the token, then `winjob stop`; callers: `windows_stop`, Prep's trap and its end without status, `windows` after cases without status, the gate, `prepare_windows_clean`, stop.sh with no release live | J2, G20 | token `stop` |
| T11 | P5 to P6 | `winjob stop` says `absent` | J1, G6 | .stopped |
| T12 | P5 to P7 | `winjob stop` says `active N` or `unknown`, or the alarm ends C | G14, G20 | C exits 1; its caller keeps the claims by G11 |
| T13 | P7 to P5 | a later C | as T10 | as T11 or T12 |
| T14 | R1 | C, once both parts are stopped, removes the run directory unless KEPT, retrying six times five seconds apart | G16 | run directory gone, or `cannot remove` and status 1 |
| T15 | R3 | Rp's `prepare_windows_clean` once C and `claim_stop` both confirmed; Y once the milestone closed | G11 | tokens and stopped files stay until Y |
| T16 | Obs polls | each minute: job `active` runs, with .rc or without; job absent is P4 with .rc, P4x with .out and P4u without; `unknown` or an ssh failure is no answer | J1, G4, G20 | after an hour without an answer, `lost`, and the caller runs C |
| T17 | the gate | `windows_start` and `prepare_all` call `windows_gate ID` before the run's first start: C's `-g ID` | J3, G8 | the earlier runs in P6, or exit 75 naming those left |

## Transitions of a claim

| # | Move | Trigger and actor | Guard | Leaves |
|---|---|---|---|---|
| U1 | Q0 to Q1 | `claim_start ID KIND N`, holding LOCKS/publish.lock, publishes `go N` by a link that fails when a token exists, and then removes .rc, .held, .started, .owner, .reaped and GOT and makes the locks, in one ssh; once that succeeded, it writes the runner and starts it with `setsid nohup`; the caller has confirmed every earlier attempt stopped | L2 | token, locks, .pl |
| U1f | none | the publication fails: a token exists, or its ssh failed | L2 | no runner; `claim_state` says `unstarted` |
| U2 | Q1 to Q2 | K takes the shared lock, reads `go N`, touches .started and starts L, which writes .owner and execs | L1, L2, L3 | .started, .owner |
| U3 | Q1 to Q1u | K read a token not its own, or never ran | L2 | `claim_state` says `unstarted` |
| U4 | Q2 to Q3 | admitted: G takes the lock and the grant lock and starts the holding shell, which touches GOT and ID.alive | the ledger's admission, L4 | GOT |
| U5 | Q2 to Q5 | L ends without GOT; K confirms the ledger and writes .rc | L3 | the step fails |
| U6 | Q3 to Q3e to Q4 | DONE, ID.alive ten minutes old, or the token; the holding shell ends and G with it, freeing the grant lock; L releases its admission and ends; K touches .reaped, confirms the ledger, writes .rc and exits | L3, L4, G11 for DONE | .reaped, .rc |
| U7 | Q2 or Q3 to Q6 | K killed | L5 | L ends on TERM, unless it waits on something; the lock stays held until it ends |
| U8 | Q3 to Q4d | WSL or the host restarts | none | GOT stays; L4 reads it as `dead` |
| U9 | any to Q7 | `claim_stop ID KIND` renames `stop` over the token under LOCKS/publish.lock, once any publication has ended; K sends L TERM, then KILL; the holding shell ends | L2, G14 | token `stop` |
| U10 | Q7 to Q8 | the exclusive lock is got, `etude status` recovers the ledger, which names no attempt's .owner, and no .held exists; `claim_stop` removes the locks and token | L1, L3 | Q8 |
| U11 | Q7 to Q9 | the lock is still held after two and a half minutes, recovery fails, the ledger cannot be read or names an attempt's L, a .held file, or the ssh call fails or outlasts five minutes | L1, L3, G14, G20 | lines for what stays |
| U12 | Q3 to Q10 | R, W or Rp finds the run not confirmed stopped and calls no `claim_stop` | G11 | the claim, until ten minutes after the last touch of ID.alive |
| U13 | none | stop.sh signals no process of a claim's session | G15 | the claim, as R left it |

`windows_prepare` and `windows_unhold` call `claim_stop` at every end, after a minute's wait for .rc, so that the lock and token are removed and the end is confirmed.

## Transitions of the release's processes, stop.sh and sweep.sh

| # | Move | Trigger and actor | Guard | Leaves |
|---|---|---|---|---|
| V1 | R or Rp starts | the manager | G13 | STATE/pid `PID START` |
| V2 | R starts on new arguments | `windows_halt` sends TERM to W's group; W is its group's leader, so its number is held between the check and the signal | G12 | STATE/windows.run removed |
| V3 | R or W is stopped | `windows_stop`: Prep and Obs with reap, `windows stopped ID`, C when it stopped a part, then `claim_stop` of the hold | G14, G11 | Prep's own trap runs C and `claim_stop` of the claim first |
| V3a | R is stopped before `windows_start` set its traps | TERM, INT or HUP end R with no `windows_stop` | none | nothing of R's own runs on Windows then |
| V4 | Rp is stopped or ends | `prepare_stop` | G14, G11 | tokens and stopped files |
| V5 | R, W or Rp killed, or the Mac reboots | no trap runs | none | parts go on in their jobs; claims stay while B touches ID.alive (Q10); the next gate stops the parts (J3) |
| V6 | the next run starts | the gate (T17) | J3, G4, G6 | the earlier runs in P6, or exit 75 |
| V7 | `mac_stop` and the Mac record's readers | `mac_stop` sends TERM to M's group only after G12's leader check | G12 | the record removed |
| V8 | stop.sh while a release is live, no -r | `releases` names a live R, Rp or W | G12 | Windows untouched; on the Mac every descendant of M's leader; -p's WSL copy; the claim sessions |
| V9 | stop.sh with no release live | C's `-a`: `none` to OUT/windows.current, then a stop of each run that `winclean.sh -l` lists; then each program under D:\rach2, the helpers among them, selected and killed in one PowerShell pass; a list or a stop that fails is counted as left | J1, J2, G19, G20 | parts in P6, or `stop: left: windows: unknown: ...` |
| V10 | stop.sh -r | TERM to each process `releases` names and to M's group by G12, up to ten minutes for them to end; a release still live then is left alone | G12 | R's traps have run V3, or the release stays |
| V11 | stop.sh on the Mac and in WSL | one pass selects what WHAT names, never for the text `release.sh`, and signals it | G19, G15 | none |
| V12 | stop.sh's `left` | each look that cannot be read, in WSL or on Windows, prints `MACHINE: unknown: WHY` and counts as left | G20 | status 1 |
| V13 | sweep.sh | a run directory is live while a part's job is not absent, or `winjob state` cannot say, while a part has a token and no stopped file by G4, and every one while the helper is missing; a live or kept run's named paths stay; a closed milestone's runs go whole | J1, G4, G20 | OUT files of open milestones |

## j79's findings

* 1, a child's number reused between CIM and a second open: no process is opened by number; `winjob stop` terminates the job (J1).
* 2, a replaced record loses a pending start: the gate needs no record; a start writes its token before it reads OUT/windows.current, which the gate writes before it lists (J3).
* 3, an ended holder reads as granted: `claim_state` says `granted` only while the lock is held (L4); the claim row `dead` fails without that test.
* 4, a launcher with no owner file: L holds the lock it inherited from K, so `claim_stop` cannot confirm while it lives (L1, L5); the claim row `orphan` kills K and finds the claim neither free nor dead until L ends.
* 5, the session guard goes stale in its loop: `claim_stop` signals nothing; K signals only L, its unreaped child, whose number cannot pass to another process (L2).
* 6, claim cleanup can wait for ever: `claim_stop`'s whole ssh runs under a five-minute alarm, and K's and `claim_stop`'s `etude status` under alarms of half a minute and a minute (G14).
* 7, stop.sh discards failures: `orphans` and C's status are kept in `unsure`, and `wsl` and `windows` print `unknown` lines for a failed look; `left` counts both (V9, V12, G20); the rows `stop_unknown_rows` check a host that does not answer and a list that fails.
* 8, the PID-reuse row fails with the fix: the row and the guard it tested are gone; the winclean rows check the token's `stop` at the time of each `winjob stop`.
* 9, the marked-member row passes a blind group kill: gone with the group guard.
* 10, the removal row passes under root: the row refuses removals through a mock `rm`, whatever the account, six times for the last-try pass and seven for the failure.
* 11, the claim-lifecycle rows are absent: `claim_rows` runs release.sh's claim functions on a real lock, with a launcher that grants, waits, ignores TERM or refuses: granted and stopped, holder ended (`dead`), waiting and stopped, refused, runner killed, and a late runner of a stopped attempt.

## j90's findings

* 1, a starter overwrites a completed stop: C renames a new token over the old and never truncates it, so a start that holds the old file open writes into the unlinked one (J2); the winclean row with a second link to the token finds the link keeping the nonce and the token holding `stop`.
* 2, a delayed setup rearms a stopped claim, or replaces a retry's token: the setup no longer writes the token; permission is published first, in an ssh that must succeed, only where no token exists, and K never creates the lock that `claim_stop` removed (L2, U1, U1f); the claim rows `late` and `retry` find a late runner starting nothing and a late start of attempt 1 failing beside attempt 2's token.
* 3, the cleanup lock permits a false grant: `granted` needs the grant lock, which only G holds while the holding shell runs, and no .reaped (L4); the claim row `cleanup` reads nothing, not `granted`, while the launcher outlives its command and while K checks the ledger.
* 4, a missing held marker falsely confirms cleanup: `claim_stop` recovers the ledger and looks up every attempt's .owner, which L writes before its exec (L3, U10); the claim row `ledger` finds it failing while the ledger names the launcher and while recovery fails.
* 5, stop misses a start before its first token: stop.sh runs C's `-a`, which writes `none` to OUT/windows.current before it lists (J3, V9); the winclean row for `-a` finds `none` and every listed run stopped, the last gate's among them.
* 6, a status file hides a job that still runs: .rc is the result only; C's list counts a part until .stopped, sweep.sh's live test likewise, and Obs reports a status only with the job absent, so R ends the claim after the job (G4, T7, T16); the winclean list row lists a run whose part has .rc.
* 7, the fixture's cleanup signals stale numbers: the claim driver signals nothing at its exit; the claim's stop token and the end files end every fake process, and the exit only waits. Since j102, no scenario signals by number either (j102 1).

## j102's findings

* 1, the orphan row can kill a reused number: the fake setsid of the row `orphan` stays the runner's parent and never reaps it before it has ended, and kills it, its own child, on a file; the launcher ignores TERM before it writes its number, so the TERM of its runner's death cannot end it first, and it ends on the end file, not by a signal.
* 2, a delayed publication removes a retry's GOT: a publication holds LOCKS/publish.lock for its whole ssh, and `claim_stop` takes it before it writes `stop`, so a publication held up between its token and its removal of GOT delays the stop instead of following it (L2, U1, U9); the claim row `publish` holds a publication there and finds the token still `go 1` while `claim_stop` waits, and the retry granted after it.
* 3, no row ran the observer's job test or sweep.sh's live test: the rows `observer_rows` run release.sh's `windows_detach` and sweep.sh's Windows script themselves, with a status file beside an active job, and find the observer giving no result until a poll reads the job absent, and sweep.sh keeping such a run's scratch until a stop has written .stopped (G4).
* 4, the claim driver's exit made only one of its end files: it touches all of them, and then waits a minute for the claim's lock and the supervisor's, which every fake process holds, printing a line that fails the row when either stays held.

## The earlier findings this model removes

* j55 1 and 2, the start and the bash's identity: J1, J2.
* j68 F1, a reused MSYS2 group: no group is signalled; J1.
* The audit's O3, a part launched but not past its PID write: J2, J3; O4, Windows PID reuse: J1; O8, a claim whose script never started: L2, U3; O13, sweep.sh's live test: V13; O14, `claim_stop` by number alone: L1, L3.
* Muse Code's 1.1, an etude that survives a failed cleanup: etude is in the job (J1, G6); 1.2, a start before its first file and a late runner beside a retry: J2, J3, L2; 1.3, a dead runner, a GOT that outlives its holder, a queued orphan: L4, L5; 1.4, the poll's reading of a lost start: T16; 1.5, stop.sh sees only running processes: V9 lists by token; 2.1, 2.2 and 2.4, kills by number, by command line and by group: none is made; 2.5, the claim session's empty branch: no session is signalled.
* Guards that went with them: the earlier G1, G2, G3, G5, G9, G10, G17 and G18, the etude clause of G6, the launch and PID files, and the poll's 300-second rule for an unstarted part.

## Limits

* A holder lost during the cases, Q4d after the step began, leaves WSL unheld until the cases end; the cases go on and may run slower, and nothing is written wrongly.
* An L killed outright leaves G running until etude's keeper ends the case's group, within its two-second grace, so the grant lock can outlast L by that much; `claim_state` reads K's .reaped, which K writes within a second of L's end, so a claim reads `granted` for at most that second after L is gone.
* K's death is reported to L only on x86-64 Linux, where the runner sets PR_SET_PDEATHSIG; elsewhere, as in the host case on the Mac, L ends by the token or by its own end.
* A run of the earlier model still running when this one is installed has no token and no job; no gate lists it. None ran at 84-122's landing.
