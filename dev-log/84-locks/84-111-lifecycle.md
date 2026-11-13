# 84-111: the lifecycle of a release's Windows parts and WSL claims

This is the model that the release's Windows parts and WSL claims are written from, since 84-122. It lists the states that a part of a Windows release run and a claim of WSL's admission can be in, which process and which file own each state, every transition with the guard it checks, and then places on it each finding that the earlier model left open: j79's eleven, and those of j55, j68, the audit in #4783 and Muse Code's review in 84-111b that the new model removes, the seven races that review j90 found in 84-122's first form, which 84-134 closed, the four faults that review j102 found in 84-134, which 84-139 closed, the two that review j110 found in 84-139, which 84-144 closed, and the six that review j116 found in 84-144, with four more of their classes, which 84-148 closed by replacing the claim's design, and the first of j132's findings and j141's five, with the seven mediums and the lows of the design round that followed them (Fable, on #4895), which 84-169 closed by giving each process that the tools may stop a run directory of its own that its holder owns whole (G12, G22). The earlier model, which identified processes by number and start time, killed them through handles and groups, and marked cancellation with files, is in git history before 84-122; 84-122's design round (Fable, on #4820) found that five rounds of review had not closed it, and replaced it with kernel objects that end with their processes; 84-148's design round (Fable, on #4866) found that four rounds of review of the claim's shared token and publication lock had not converged either, and replaced them with files of each attempt that are only created.

Words: OUT is Windows' output directory, `D:/rach2/build/release`, which WSL sees as `/mnt/d/rach2/build/release`; TREE is Windows' copy, `D:/rach2`; LOCKS is `build/flow/claims` of the owner's clone in WSL, `/home/testing/rach2/build/flow/claims`, which is ext4 and holds every file of a claim; ID is a run's ID; PART is `prepare` or `cases`; KIND is `claim`, the preparation's claim, or `hold`, the whole host's claim for the cases, and N its attempt, 1 to 3; STATE is `build/flow/release` of the main working tree.

## The model

Liveness is a kernel object that ends with its processes: a named Job Object, `Global\rach2-ID.PART`, for a Windows part, and for a claim in WSL an flock of a file of the attempt's own, LOCKS/ID.KIND.N.live, which its runner holds and its launcher inherits. A part's permission is a token file, OUT/ID.PART.token: a stopper replaces it whole, by rename, and then tests the job; a starter takes the job and then reads the token. A claim has no token: its files are only ever created, one set for each attempt, and a stop is a tombstone, LOCKS/ID.KIND.stop for the claim or LOCKS/ID.KIND.N.stop for one attempt, which a stopper creates before it probes the attempt's locks and which a runner or holder reads only once it holds its own lock; so a stop that comes late or twice changes nothing, and nothing is ever undone. A live grant is a lock of its own, LOCKS/ID.KIND.N.granted, which only the holder holds. No lock is shared between claims or attempts. A status file gives a part's result, never its end, and a free lock does not prove the ledger clean: a stop is confirmed only by the job absent, and a claim's end only by a recovered ledger that names none of its launchers. "Stopped" is the object absent or the lock free; every other answer, an ssh failure, a bound reached or a state Windows cannot read among them, is "cannot say" and fails. No process number read from a file is signalled anywhere, no process's life is read from its number, and no wait reads a clock: each counts turns of a sleep. The release's own processes follow the same rule through [hold.pl](../../tools/flow/hold.pl): each is born behind a gate in a run directory of its own, its holder's lock is its life, a create-only `stop` is its stop and a create-only `end` its end (G22).

## The precondition

One manager and one STATE own Windows' copy and OUT. The gate's right to stop any run of OUT rests on that ownership, not on a test that the run's release is gone: a release under another STATE, or from another clone, that used the same copy would have its parts stopped by this one's gate. G13's locks are per STATE, so this is a rule of use, which `tools/flow/machines` keeps by naming one copy for the one release process of the main working tree.

## Host facts

Measured on the testing host for 84-122 (#4820): a process that WMI's Win32_Process.Create started makes a `Global\` job, joins it, and is found by name from a separate ssh session; etude's own jobs, `lock tree`'s and the cases' (tools/etude/win/hold.c, winexec.c), nest inside it; no process of a part leaves it, plain, `setsid`, double-forked and `nohup` MSYS2 programs, `cmd`, and PowerShell's Start-Process among them, and none is left after a stop; LOCKS is ext4 (OUT, under `/mnt/d`, is 9p, where flock does not hold), and an exclusive non-blocking flock is refused there while a shared one is held; perl 5.40, flock and setsid are in WSL.

Measured on the testing host for 84-148: WSL's stat of a file under `/mnt/d`, drvfs over 9p with `cache=0x5`, saw the byte that a Windows process appended, `cmd.exe`'s `echo` or MSYS2 bash's `printf`, at its first look after that process had exited, 3 to 4 ms later, in 18 trials of 18. So the holder's looks at OUT/ID.alive and the runner's read of OUT/windows.current see Windows' writes as soon as they are made.

## Processes

| Name | Process | Where | Started by | Stopped by |
|---|---|---|---|---|
| R | `release.sh M SHA`, with or without `-r`: the release, which, once it holds STATE/lock, goes on as the job of a hold.pl in its run directory STATE/held/release-STAMP, which STATE/run names (G22) | Mac | the manager, in tmux | TERM, INT, HUP and EXIT run `windows_stop` once `windows_start` has set the traps; its hold.pl's TERM once the run directory's stop exists, and KILL an hour later; KILL; a Mac reboot |
| Rp | `release.sh -p M SHA`, held as R is | Mac | the manager | TERM, INT, HUP, EXIT (trap `prepare_stop`); its hold.pl's TERM and KILL, as R's; KILL |
| A | `release.sh -a windows M SHA` | Mac | the manager | ends once it has started W |
| W | `release.sh -w ID M SHA`: a Windows run of -a, in the process group of the `hold.pl -g` that leads it in the run directory STATE/held/ID, which STATE/windows.run names | Mac | A, by hold.pl and `nohup` | TERM and EXIT (trap `windows_stop`), KILL; R's `windows_halt` and stop.sh -r create its directory's stop, on which its hold.pl sends TERM to its group, and KILL an hour later (G22) |
| M | `release.sh -m ID M SHA` or `-p -m`: the Mac's run, in the group of the `hold.pl -g` that leads it in the run directory build/machines/mac/held/ID, which build/machines/mac/run names with ID | Mac | R or Rp | `mac_stop` of a later R, Rp's `prepare_stop` and stop.sh -r create its directory's stop, on which its hold.pl sends TERM to its group, and KILL half an hour later (G22) |
| Obs | `windows_detach`'s observer, a job of R, W or Rp that hold.pl holds | Mac | `windows_prepare`, `windows` | its hold.pl's TERM, asked for by the parent's trap (G22) |
| Prep | `windows_prepare`, a job of R, W or Rp that hold.pl holds in STATE/jobs/NAME.XXXXXX | Mac | `windows_start`, `prepare_all` | its hold.pl's TERM, asked for by `windows_stop` or `prepare_stop`, and KILL an hour later; its own TERM trap |
| Beat | `beat ID`: appends a byte to ID.alive in WSL each minute, a job that hold.pl holds | Mac | `windows_prepare`, `windows` | its hold.pl's TERM, asked for by its parent, or once its parent's shell has gone, which its hold.pl's `-P` reads as a stop (G22) |
| T | the command of `testing.sh run`, R's build and Rp's preparation in WSL, held by a `hold.pl -P` in the run directory build/held/NAME of the copy, as are testing.sh's own ssh on the Mac | WSL | R or Rp, through testing.sh | its hold.pl's TERM once its directory's stop exists, which testing.sh's exit creates, or once its ssh session is gone, and KILL a minute later |
| S | a part's start: MSYS2 `bash -l -s` in the ssh session of `windows_detach` | Windows, in the session's job | Obs | the end of its ssh session, which ends every process of that job |
| H | the part's helper, `OUT/winjob-SUM.exe run Global\rach2-ID.PART TOKEN NONCE bash.exe -l OUT/ID.PART.sh`, which winjob.c builds | Windows, in the part's job, which it made | WMI, for S | its command's end, after which it terminates the job; a stop's `winjob stop`, which ends it with the job |
| B | the part's bash and everything it starts, etude-ID.exe and QEMU among them | Windows, in the part's job | H | the job's end |
| C | `winclean.sh`, after winjob.sh's lines: stop and clean (no flag), `-s` state, `-l` list, `-g` gate, `-a` gate for no run | Windows, through ssh | `windows_clean` and `windows_gate` of R, W or Rp; stop.sh's `-a`; sweep.sh's own lines | ends by itself, within twenty minutes, which its callers' bound.pl bounds |
| K | a claim attempt's runner, `perl LOCKS/ID.KIND.N.pl run ...`, the attempt's copy of [claim.pl](../../tools/flow/claim.pl), leader of its own session (`setsid`) | WSL | `claim_start` | its launcher's end and its cleanup; on a tombstone it ends its launcher first |
| L | the attempt's launcher, `env ... etude-launch build env` or `alone`, K's child, which writes LOCKS/ID.KIND.N.owner before its exec and runs G once admitted; the ledger names it `owner PID START KNOWN` | WSL | K | K's TERM on a tombstone, KILL thirty turns later; G's end; TERM by PR_SET_PDEATHSIG when K dies first |
| G | the holder: L's command, `perl LOCKS/ID.KIND.N.pl hold ...`, one process with no children, which creates and holds LOCKS/ID.KIND.N.granted | WSL | L, once admitted | OUT/DONE, a tombstone, or OUT/ID.alive's size unchanged in 120 looks five seconds apart; etude's keeper ends the case's group when L dies |
| X | `stop.sh [-r]` | Mac | the manager or the chat | ends by itself; it stops each loop's session through its supervisor (G21) and the release's parts through their hold.pl (G22) |
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
| OUT/ID.alive | B's alive loop and Beat, each appending a byte each minute while it runs | while it grows, a claim of ID may hold | Y at close |
| OUT/etude-ID.exe and the run's logs, consoles and selection | B's body | the run's own files | Rp's cleanup; Y at close |
| TREE/build/tmp/ID | B's body (`mkdir`), its KEPT and record | the run's directory | B's body when it passed and kept nothing; C unless KEPT; Y unless KEPT or live, and whole at close |
| LOCKS/ID.KIND.N.pl | `claim_start`, with noclobber, failing when it exists | the attempt's copy of claim.pl, K's and G's program | Y at the milestone's close |
| LOCKS/ID.KIND.N.out | `claim_start`'s redirection | K's, L's and G's output | Y at close |
| LOCKS/ID.KIND.N.live | K creates it and holds an exclusive flock of it, which L inherits | while held, K or L of attempt N lives | Y at close |
| LOCKS/ID.KIND.N.go, .none | K, once it holds .live: .go when it read no tombstone and OUT/windows.current named ID, .none when it starts nothing | whether K started L | Y at close |
| LOCKS/ID.KIND.N.owner | L before its exec, whole, by a link: its number and its start | the ledger's owner that is the attempt's | Y at close |
| LOCKS/ID.KIND.N.granted | G creates it and holds an exclusive flock of it | while held, attempt N is admitted and G holds; once it exists, the attempt was granted | Y at close |
| LOCKS/ID.KIND.N.reaped, .held, .rc | K: it reaped L; L stayed in the ledger; L's status, last | the attempt's end | Y at close |
| LOCKS/ID.KIND.stop, LOCKS/ID.KIND.N.stop | `claim_stop`, empty, created once | the claim, or attempt N, is stopped for good | Y at close |
| OUT/ID.prepared, OUT/ID.end (DONE) | R, W or Rp | the claim may end; G ends on it | Y at close |
| DIR, a run directory: STATE/jobs/NAME.XXXXXX, STATE/held/release-STAMP, STATE/held/ID, build/machines/mac/held/ID, or build/held/NAME of a copy in WSL | its maker, by a mkdir that fails when it exists, before the record that names it and before the start | one run of R, Rp, W, M, T or a job of their shells | `job_wait`, `held_gone`, `hold.pl sweep` and the preparation's cleanup in WSL, each only once `hold.pl state` reads `ended`; a lost run's directory stays (G23) |
| DIR/live | the run's hold.pl: it creates the file and holds an exclusive flock of it, then links it to `live` | while held, the run's holder lives | with DIR |
| DIR/boot, DIR/group | the run's hold.pl, by a link, before the job can run: the boot's ID, and with -g the anchor's number | the evidence by which a looker proves a lost run ended (G23) | with DIR |
| DIR/go | `held`, a FIFO, which its subshell opens for reading and writing before it forks the job and keeps as hold.pl's descriptor 6; the job opens its own reader and only then closes its copy of 6 | the gate: the job runs only once it reads `go` there, which hold.pl writes after it has read DIR/stop; the FIFO lasts while either end is open, so a `go` written before the job's reader opens is kept | with DIR |
| DIR/stop | stop.sh -r, `hold.pl stop`, `job_ask`, testing.sh; created and never truncated | the run is stopped for good: hold.pl reads it before the gate and every tenth of a second after | with DIR |
| DIR/end | the run's hold.pl, by a link, once, after the job and, with -g or on Linux, what it left have ended; or `hold.pl state`, with 137, once it has proved a lost run ended (G23); or a person who has stopped what it held | the job's status | with DIR |
| STATE/run | R and Rp, under STATE/lock, by rename: R's DIR | the release's run | none; the next release replaces it |
| STATE/windows.run | A, by rename: W's DIR | W | `windows_forget`, from `windows_wait` and `windows_halt`, only once W's holder has ended; otherwise it stays and the caller fails (G23) |
| build/machines/mac/run | R or Rp, by rename: `DIR ID` | M's run directory and run | `mac_wait` and `mac_stop`, once M's holder has ended; `prepare_stop`, once a confirmed stop |
| STATE/loop.live, STATE/session.live | a loop and its session's `session.pl`, each holding an exclusive flock of its file for its whole life | while held, the loop, or its session's supervisor, lives | never |
| STATE/session | `session.pl`, by rename, before the session's go: `GROUP BOOT` | the session's group and the boot it runs in | `session.pl`, once the group is gone; with the lock free, only a `session.pl` or stop.sh's halt that proves the group gone (G21, G23) |
| STATE/record lines `windows started ID`, `windows stopped ID` | `note` under STATE/record.lock | a run that was started; a stop ended it, so it counts as none of the three | -r's rewrite |
| ledger `/var/lib/rach2-etude/ledger/claims` | L, through etude | a claim or request with `owner PID START KNOWN` | L at its end; `etude status`'s recovery once the owner is dead |

## Guards

* J1, the job: a part's processes are born in its job, since H joins the job before it starts B, and no process may break away; the job's processes end when its last handle closes, which is H's, so a killed H takes the part with it; `winjob stop` terminates the job and waits half a minute for its last process. A part runs while `winjob state` says `active N`, and has stopped once it says `absent`; `unknown` is "cannot say".
* J2, the part's token: S writes its nonce with noclobber before anything else, so a part that a stop reached first never starts; H reads the token only after it holds the job, and starts B only while it holds the nonce; C renames a new file holding `stop` over the token before it calls `winjob stop`, and never truncates it, since S may have created it and not yet written its nonce: that write then lands in the file the rename unlinked. H reads the token sharing delete, and C tries a refused rename again five times. So a start in flight holds a job that the stop ends, or reads `stop`.
* J3, the gate's run: C's `-g ID` writes ID to OUT/windows.current, then lists every run of OUT with a token and no end by G4, and stops each but ID; `-a`, which stop.sh runs, writes `none` and stops each. S reads OUT/windows.current after it wrote its token and before Create, and starts nothing for any other ID. So a start that the gate did not list wrote its token after the gate's list, hence after the gate's write, and reads the new ID or `none`; no record of started IDs is needed, and none is lost when the record is replaced.
* J4, the helper's identity: `winjob run` refuses a job name that exists, so two helpers never share a part; the helper's program is named by its source's checksum, so a running one is never replaced.
* L1, an attempt's locks: K creates LOCKS/ID.KIND.N.live and takes it exclusively, ten tries a second apart, before it reads anything, and clears close-on-exec so that L keeps it; G creates and takes LOCKS/ID.KIND.N.granted itself, since etude's keeper and bridge close inherited descriptors. Lookers, `claim_state` and `claim_stop`, probe a lock shared and non-blocking and never create its file, a missing file counting as free; so a looker that stalls holds at most a shared lock, which costs K its attempt, since K then creates .none, and keeps neither another look nor a stop waiting. No lock is shared between attempts or claims, so a process that a stall or a leak leaves holding one can leave only its own attempt's stop unconfirmed.
* L2, tombstones: `claim_stop` creates LOCKS/ID.KIND.stop, or LOCKS/ID.KIND.N.stop for one attempt before a retry, and only then probes each attempt's .live and then its .granted until both are free, a hundred and fifty one-second turns at most in all. K reads both tombstones once it holds .live, before it starts L, and each second while L runs, and on one sends TERM, then KILL thirty turns later, to L, its own unreaped child; G reads them once it holds .granted and at every look. So a K or G that read no tombstone holds a lock that the stopper waits for, and one that locks after the stopper's probe reads the tombstone. Nothing removes a tombstone or any other file of a claim before Y, so a confirmed stop is final, and a late `claim_start`, a late K or a second stop changes nothing.
* L3, the ledger: L writes LOCKS/ID.KIND.N.owner, its number and its start from `/proc/self/stat`, whole, before its exec, so before it can enter the ledger. K, once it has reaped L, creates .reaped, runs `etude status`, which drops a dead owner, under a bound of thirty counted seconds, and reads the ledger for L's `owner PID START` for a minute; it creates .held when L stays or the ledger cannot be read, and only then .rc, and frees .live by exiting. `claim_stop`, once the locks are free, has `etude status` recover the ledger under a bound of sixty counted seconds, and fails when recovery fails, the ledger cannot be read, it names the owner of any attempt's .owner, or a .held exists. So a restart of WSL, which leaves no lock and an entry until recovery, and a K killed before it wrote .held, while a bridge that closed the lock's descriptor still holds the ports, are not confirmed.
* L4, a live grant: `claim_state` says `granted` only while .granted is held and .reaped is missing. G holds .granted only while it holds the claim, and etude's launcher releases its admission only after its command, G, has ended, so .granted is free before the admission ends; .live is no test of a grant, since K holds it through its cleanup after L released its admission. While either lock is held otherwise it says nothing; with both free it says `dead` when .granted exists, since G has ended, `refused` when .rc exists, `unstarted` without .go, and `dead` otherwise. A lock it cannot probe gives nothing, as a wait does.
* L5, K's death: L carries PR_SET_PDEATHSIG of TERM, on which admission withdraws its request, so a K killed while L waits leaves no request in the ledger; L holds .live until it ends, so the attempt reads neither free nor dead before.
* L6, the current run: K starts L only while OUT/windows.current names ID, which `windows_gate` wrote before the run's first start and stop.sh's `-a` makes `none`; so once the gate has moved on, and after Y has removed a closed milestone's claim files, which it does for no run that OUT/windows.current names, a late K starts nothing (J3).
* L7, the holder's expiry: G looks at OUT/ID.alive's size every five seconds and ends after 120 looks in a row in which it did not change; B and Beat each append a byte each minute. Only G's own count decides, so no clock of either machine, nor the skew between them, moves the expiry.
* G4, the status file: OUT/ID.PART.rc gives a part's status and not its end: B writes it before it exits, and H terminates the job only after that, so a paused H leaves a live job beside it. A part has ended once its job is absent; C's list and gate count it ended only once OUT/ID.PART.stopped exists, which a stop of an absent job writes at once, and Obs reports a status only once the job is absent too, so that R ends the claim in WSL only after the job.
* G6, a confirmed stop is final: C writes OUT/ID.PART.stopped only once the job is absent; etude runs inside the part, so no separate check of etude is made.
* G7, IDs are not reused: each run gets a new ID from `stamp`, and only W's first run takes the ID that A recorded.
* G11, claims follow the parts: R, W and Rp end a claim of ID only after C confirmed ID stopped, or when no part of ID was started; otherwise the claim stays and G ends it once OUT/ID.alive has not grown in 120 looks (L7).
* G12, a run of release.sh: a run lives while its hold.pl holds DIR/live, as `hold.pl state` reads it, a shared non-blocking probe: R and Rp in the directory that STATE/run names, W in STATE/windows.run's, M and -p's Mac preparation in build/machines/mac/run's. `starting`, a directory made and not yet locked, counts as live, and so does a lock that cannot be probed; `ended CODE` is the end; `lost`, a free lock without `end`, is reported and its directory and record kept, and blocks as G23 says. stop.sh and release.sh read no number, no `ps` and no `kill -0` for it, and a new Mac or Windows run starts only once the last one's holder has ended. A command line that merely holds release.sh is never one of the processes stop.sh lists. Beat ends once R's shell has gone, which its hold.pl's `-P` reads as a stop.
* G13, locks: one R or Rp at a time per STATE, by STATE/lock; one A or Rp at a time, by STATE/again.lock; `note` and -r's rewrite hold STATE/record.lock.
* G14, bounds: [bound.pl](../../tools/flow/bound.pl) forks its command, counts one-second turns and sends KILL, or a HUP, INT or TERM that it took, to it only from the one loop that reaps it and only after a look in the same turn found it not reaped, its handlers only recording a signal, so that it reaches no process that took the number and ends ssh, which ignores SIGALRM. It bounds every C, at twenty minutes, every `claim_stop`'s ssh at five, every `claim_start`'s and `claim_state`'s at two, and the ssh of stop.sh and sweep.sh; `tssh` and these ssh connect within ten seconds. Inside claim.pl the same counted bound ends K's `etude status` after thirty seconds and `claim_stop`'s after sixty. A call that a bound ends counts as unconfirmed. No wait in the claim path, stop.sh or sweep.sh reads a clock: K's KILL comes thirty turns after its TERM, G's expiry after 120 looks, and `windows_detach` gives up after sixty polls in a row without an answer, each a minute's sleep apart. A suspended host stretches each and hastens none.
* G15, the claim sessions: stop.sh never signals a process whose session is a claim's: each session whose leader, in the same reading of ps, is a perl running LOCKS/ID.KIND.N.pl with `run`.
* G16, removal by result: C decides that a run's scratch is gone from the result of its last `rm -rf`, not from the count of tries.
* G19, strays only through a handle: stop.sh stops what the sessions left only where a handle names the process, and never a loop. In WSL [procs.pl](../../tools/flow/procs.pl) opens a pidfd on each process before it reads its command line, session, start, user and directory, checks with a signal 0 through it that the process it read still runs, and sends TERM through it; any error but ESRCH, a TERM's among them, fails the whole scan, which then counts as a look that cannot be read. On Windows winjob's `strays` opens each process once, reads its image's path through that handle, and for one under D:\rach2 terminates it and waits through the same handle, the helpers among them, whose death ends their parts' jobs. On the Mac, which has no such handle, a process that no hold.pl holds is listed, `stop: left: mac: PID ARGS`, and counted, and never signalled.
* G20, cannot say is not stopped: C, `claim_stop`, stop.sh's `left` and sweep.sh take an ssh failure, a timeout, an unreadable state or a missing helper as "may still run": C exits 1, `claim_stop` fails, `left` prints `MACHINE: unknown: WHY` and counts it, and sweep.sh keeps the run's directory.
* G21, a loop's session: stop.sh stops a loop's session only through its supervisor, the `session.pl` whose command line names STATE/session, which, once STATE/session.stop exists, stops the group it made, whose leader, the anchor, is a child of its own that ignores TERM and that it reaps only once ps shows nothing else in the group, and exits once the group is gone; the loop holds STATE/loop.live and `session.pl` STATE/session.live for their whole lives, and stop.sh waits ninety counted seconds for both locks to be free, probing each shared and non-blocking, a lock that cannot be probed counting as held; the loop ends by itself once STATE/stop exists and its session has ended, and stop.sh never signals it or reads its number. `session.pl` refuses to start while another holds STATE/session.live, and starts nothing when a stop came before its go. Its handlers only record a signal; one loop reaps the session and signals the group. A group that STATE/session names and that has no supervisor is never signalled, since its number may now be another's; only KILL can leave it so, since `session.pl` traps HUP, INT and TERM. Its record stays, and `session.pl` starts no session beside it until it proves the group gone as G23 says, by the record's boot or by ps, and exits 125 meanwhile, which the loop's wait for a short session retries; `wait_session` never removes it, and sweep.sh removes no output of a clone while it exists. stop.sh's halt removes it on the same proof, and otherwise reports it as left; it, a supervisor or a loop that outlives the ninety seconds and a loop whose host did not answer each count in stop.sh's final count, and the status is 1 whenever that count is not 0.
* G22, held runs: only an unreaped child is signalled, or the group of an anchor that is the signaller's unreaped child, and only by the process that holds it; the signaller is never in the group it signals. [hold.pl](../../tools/flow/hold.pl) holds each job of R's, W's and Rp's shells, each of R, Rp, W and M, and T, each in a run directory DIR of its own that its maker creates once, before the record that names it. The holder takes DIR/live for its whole life; the job waits at a gate, a pipe in command mode or the FIFO DIR/go that `held`'s subshell opens before it execs hold.pl, and runs only once the holder, having checked that the job is its unreaped child, has read DIR/stop and found none, so that a stop that came first runs nothing and the job ends 143, and a job cannot end before its holder holds it. `held`'s subshell opens DIR/go before it forks the job, and the job opens its own reader before it closes its copy, so the FIFO is held open from before the holder exists until the job's reader is open, and the holder's `go` cannot be lost. With -g the holder leads a group of its own and forks the anchor, which leads another, ignores HUP, INT and TERM and exits when the holder's pipe to it ends; the job joins the anchor's group. The holder sends TERM once DIR/stop exists, or with `-P` once its parent has gone, to its job only after a look in the same turn found it unreaped, or with -g to the anchor's group, whose number no other group can take while the anchor is its unreaped child, and KILL a counted bound later; once the job has ended it drains what the job left, with -g the rest of the anchor's group and on Linux, where it is a subreaper, every process that reparented to it: on each turn it reaps, sends TERM to each child and to the group that has had none, and from the KILL's turn on, counted from the stop, KILL to each and to the group, reaps the anchor last, once ps shows the group empty, and ends the drain only when waitpid finds no child; only then does it create DIR/end, by a link that fails when it exists.
* G23, lost permits nothing: only `ended CODE` or `absent` permits removing a run's record, directory or output, starting another run in its place, or cleaning up what it made. A lost run, a free lock without `end`, blocks all of these until its end is proved and written: `hold.pl state` creates `end` with 137 once DIR/boot names another boot, or, with DIR/group, a ps that succeeds shows nothing in that group, whose number cannot pass to another group while a member remains; otherwise a person stops what stop.sh lists and creates it. A nested holder's orphans pass to the outer holder, whose end covers them; an outermost one in WSL, a run under build/held of -p's copy, is confirmed by procs.pl's strict scan of the copy finding nothing and by the ledger, as far as Linux shows without a cgroup. A `-p` job of a shell has no group and no tree: its proof is the boot or a person. So `job_wait` keeps a lost job's directory and records it, and `windows_stop` and `prepare_stop` then clean nothing of their part; `windows_forget` keeps STATE/windows.run; a release, -p and -a refuse to start while STATE/run, STATE/windows.run or the Mac's run file names a lost run; stop.sh sweeps with `-a` only while `releases` lists nothing and every loop's stop is confirmed; testing.sh keeps its run directories; and a STATE/session without its supervisor stays as G21 says. A shell asks for a stop by creating DIR/stop and reads the end from the lock and DIR/end, never by a number, and removes DIR only once `hold.pl state` reads `ended`.

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
| P7 stop unconfirmed | token `stop`, no .stopped; job active or unknown, or C's bound ended it | may still run | the next C |

The run ID as a whole is cleaned, R1, once both parts are in P4 or P6 and C has removed TREE/build/tmp/ID and the tooling paths its record says were made; it is kept, R2, while TREE/build/tmp/ID/KEPT exists, until Y at the milestone's close; and its files are removed, R3, by Rp's cleanup, all but the tokens and stopped files, or by Y at close.

## The states of a claim

| State | Shown by | What runs | Moved on by |
|---|---|---|---|
| Q0 absent | no file of attempt N | nothing | `claim_start` |
| Q1 sent | .pl; no .go or .none | maybe a K about to lock .live | K, or `claim_stop` |
| Q1u unstarted | .none, or no .go after `claim_start`'s half minute; .live free | nothing, and nothing will start | `claim_state` says `unstarted`; `claim_stop ID KIND N`, then attempt N+1 |
| Q2 waiting | .live held, .go, no .granted | K, L as a request in the ledger | admission |
| Q3 granted | .granted held, no .reaped | K, L, G | DONE, a tombstone, or OUT/ID.alive unchanged in 120 looks |
| Q3e ending | .live held; .granted free, or .reaped | L releasing its admission, or K checking the ledger | K's .rc and exit |
| Q4 released | both locks free, .granted, .rc | nothing | `claim_stop` confirms the ledger |
| Q4d holder lost | both locks free, .go, no .rc, as a WSL restart leaves it, or .granted after the step began | nothing holds WSL | `claim_state` says `dead`; `claim_stop ID KIND N`, then attempt N+1 |
| Q5 refused | both locks free, .rc, no .granted | nothing | `claim_state` says `refused`; the step fails |
| Q6 K killed | .live held by L, no .rc | L, which PR_SET_PDEATHSIG has sent TERM | L's end, then `dead` |
| Q7 stopping | a tombstone; `claim_stop` probes the locks | the attempt's processes ending | `claim_stop` |
| Q8 stopped and confirmed | the tombstone; `claim_stop` exited 0 | nothing, no ledger entry of any attempt's L after recovery, and nothing that can start | final |
| Q9 stop unconfirmed | the tombstone; `claim_stop` failed: a lock stayed held, recovery failed, the ledger names an attempt's L, a .held file, or the host did not answer within the bound | something may hold | a person, as its lines say; the tombstone keeps every late start out |
| Q10 left to expire | R did not stop it, by G11 | K, L, G holding while OUT/ID.alive grows | Q4 once it has not grown in 120 looks |

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
| T12 | P5 to P7 | `winjob stop` says `active N` or `unknown`, or bound.pl ends C | G14, G20 | C exits 1; its caller keeps the claims by G11 |
| T13 | P7 to P5 | a later C | as T10 | as T11 or T12 |
| T14 | R1 | C, once both parts are stopped, removes the run directory unless KEPT, retrying six times five seconds apart | G16 | run directory gone, or `cannot remove` and status 1 |
| T15 | R3 | Rp's `prepare_windows_clean` once C and `claim_stop` both confirmed; Y once the milestone closed | G11 | tokens and stopped files stay until Y |
| T16 | Obs polls | each minute: job `active` runs, with .rc or without; job absent is P4 with .rc, P4x with .out and P4u without; `unknown` or an ssh failure is no answer | J1, G4, G20 | after an hour without an answer, `lost`, and the caller runs C |
| T17 | the gate | `windows_start` and `prepare_all` call `windows_gate ID` before the run's first start: C's `-g ID` | J3, G8 | the earlier runs in P6, or exit 75 naming those left |

## Transitions of a claim

| # | Move | Trigger and actor | Guard | Leaves |
|---|---|---|---|---|
| U1 | Q0 to Q1 | `claim_start ID KIND N`, in one ssh that bound.pl ends after two minutes, creates LOCKS/ID.KIND.N.pl with noclobber, starts it as K with `setsid nohup`, and waits half a minute at most for .go or .none; the caller has confirmed every earlier attempt stopped | L2 | .pl |
| U1f | none | the ssh fails, reaches its bound, or finds the .pl | G14 | `claim_state` says `unstarted` |
| U2 | Q1 to Q2 | K locks .live, reads no tombstone and its ID in OUT/windows.current, creates .go and starts L, which writes .owner and execs | L1, L2, L3, L6 | .go, .owner |
| U3 | Q1 to Q1u | K could not lock .live, read a tombstone or another run, and created .none; or K never ran | L1, L2, L6 | `claim_state` says `unstarted` |
| U4 | Q2 to Q3 | admitted: G creates and locks .granted and reads no tombstone | the ledger's admission, L4 | .granted |
| U5 | Q2 to Q5 | L ends without a grant; K confirms the ledger and writes .rc | L3 | the step fails |
| U6 | Q3 to Q3e to Q4 | DONE, OUT/ID.alive unchanged in 120 looks, or a tombstone; G ends, freeing .granted; L releases its admission and ends; K creates .reaped, confirms the ledger, writes .rc and exits | L3, L4, L7, G11 for DONE | .reaped, .rc |
| U7 | Q2 or Q3 to Q6 | K killed | L5 | L ends on TERM, unless it waits on something; .live stays held until it ends |
| U8 | Q3 to Q4d | WSL or the host restarts | none | .granted stays; L4 reads it as `dead` |
| U9 | any to Q7 | `claim_stop ID KIND [N]` creates the tombstone; K sends L TERM, then KILL thirty turns later; G ends at its next look | L2, G14 | the tombstone |
| U10 | Q7 to Q8 | each attempt's .live and .granted probe free, `etude status` recovers the ledger, which names no attempt's .owner, and no .held exists | L1, L2, L3 | Q8 |
| U11 | Q7 to Q9 | a lock is still held after a hundred and fifty turns, recovery fails, the ledger cannot be read or names an attempt's L, a .held file, or the ssh fails or reaches its bound of five minutes | L1, L3, G14, G20 | lines for what stays |
| U12 | Q3 to Q10 | R, W or Rp finds the run not confirmed stopped and calls no `claim_stop` | G11 | the claim, until OUT/ID.alive has not grown in 120 looks |
| U13 | none | stop.sh signals no process of a claim's session | G15 | the claim, as R left it |
| U14 | none | a late `claim_start` or K of any attempt after a tombstone, or of a run that OUT/windows.current no longer names | L2, L6 | .none; nothing starts |

`windows_prepare` and `windows_unhold` call `claim_stop ID KIND` at every end, after a minute's wait for .rc, so that the end is confirmed; on `unstarted` or `dead` they stop that attempt alone, with `claim_stop ID KIND N`, before the next.

## Transitions of the release's processes, stop.sh and sweep.sh

| # | Move | Trigger and actor | Guard | Leaves |
|---|---|---|---|---|
| V1 | R or Rp starts | the manager | G13 | STATE/run naming R's run directory, made new |
| V2 | R starts on new arguments | `windows_halt` runs `hold.pl stop` on W's directory, and W's hold.pl sends TERM to the group it leads, KILL an hour later | G12, G22 | STATE/windows.run removed, with W's directory once its holder has ended |
| V3 | R or W is stopped | `windows_stop`: Prep and Obs with reap, `windows stopped ID`, C when it stopped a part, then `claim_stop` of the hold | G14, G11 | Prep's own trap runs C and `claim_stop` of the claim first |
| V3a | R is stopped before `windows_start` set its traps | TERM, INT or HUP end R with no `windows_stop` | none | nothing of R's own runs on Windows then |
| V4 | Rp is stopped or ends | `prepare_stop` | G14, G11 | tokens and stopped files |
| V5 | R, W or Rp killed, or the Mac reboots | no trap runs | none | parts go on in their jobs; claims stay while B appends to ID.alive (Q10); the next gate stops the parts (J3) |
| V6 | the next run starts | the gate (T17) | J3, G4, G6 | the earlier runs in P6, or exit 75 |
| V7 | `mac_stop` and the Mac record's readers | `mac_stop` runs `hold.pl stop` on M's directory for five minutes, and M's hold.pl sends TERM to the group it leads, KILL half an hour later; the readers take M as running while its holder holds the lock, and read RUNS/ID.times only once it has ended | G12, G22 | the record and the directory removed once M's holder has ended; a lost or running holder ends R for a person |
| V8 | stop.sh while a release is live, no -r | `releases` names a run of R, Rp, W or M whose holder is running or starting | G12 | Windows untouched; -p's WSL copy; the claim sessions; each run a line `stop: left: mac: KIND DIR: STATE` that counts |
| V9 | stop.sh with no release live | C's `-a`: `none` to OUT/windows.current, then a stop of each run that `winclean.sh -l` lists; then each program under D:\rach2, the helpers among them, stopped through winjob's `strays`, one handle each; a list or a stop that fails is counted as left | J1, J2, G19, G20 | parts in P6, or `stop: left: windows: unknown: ...` |
| V10 | stop.sh -r | the stop of each run directory that STATE/run, STATE/windows.run and build/machines/mac/run name, again on each turn for a run started meanwhile, on which each hold.pl sends TERM, up to ten minutes for `releases` to name none running or starting; a run still live then is left alone and counted | G12, G22 | R's traps have run V3, or the release stays |
| V11 | stop.sh on the Mac, in WSL and on Windows | in WSL and on Windows one pass selects what WHAT names, or what runs under D:\rach2, never for the text `release.sh` and never a loop, and signals each through a handle, a pidfd or winjob's; on the Mac it lists them and signals none | G19, G15 | `stop: left: mac: PID ARGS` for each on the Mac, which counts |
| V12 | stop.sh's `left` | each look that cannot be read, in WSL or on Windows, prints `MACHINE: unknown: WHY` and counts as left | G20 | status 1 |
| V13 | sweep.sh | a run directory is live while a part's job is not absent, or `winjob state` cannot say, while a part has a token and no stopped file by G4, and every one while the helper is missing; a live or kept run's named paths stay; a closed milestone's runs go whole, and so do its claim files in LOCKS, but those of the run that OUT/windows.current names and those of a claim whose lock is held, and none while OUT/windows.current cannot be read | J1, G4, G20 | OUT files of open milestones |

## j79's findings

* 1, a child's number reused between CIM and a second open: no process is opened by number; `winjob stop` terminates the job (J1).
* 2, a replaced record loses a pending start: the gate needs no record; a start writes its token before it reads OUT/windows.current, which the gate writes before it lists (J3).
* 3, an ended holder reads as granted: `claim_state` says `granted` only while the attempt's grant lock, .granted, is held (L4); the claim row `dead` fails without that test.
* 4, a launcher with no owner file: L holds the attempt's .live, which it inherited from K, so `claim_stop` cannot confirm while it lives (L1, L5); the claim row `orphan` kills K and finds the claim neither free nor dead until L ends.
* 5, the session guard goes stale in its loop: `claim_stop` signals nothing; K signals only L, its unreaped child, whose number cannot pass to another process (L2).
* 6, claim cleanup can wait for ever: `claim_stop`'s whole ssh runs under bound.pl's five minutes, and K's and `claim_stop`'s `etude status` under counted bounds of half a minute and a minute (G14).
* 7, stop.sh discards failures: `orphans` and C's status are kept in `unsure`, and `wsl` and `windows` print `unknown` lines for a failed look; `left` counts both (V9, V12, G20); the rows `stop_unknown_rows` check a host that does not answer and a list that fails.
* 8, the PID-reuse row fails with the fix: the row and the guard it tested are gone; the winclean rows check the token's `stop` at the time of each `winjob stop`.
* 9, the marked-member row passes a blind group kill: gone with the group guard.
* 10, the removal row passes under root: the row refuses removals through a mock `rm`, whatever the account, six times for the last-try pass and seven for the failure.
* 11, the claim-lifecycle rows are absent: `claim_rows` runs release.sh's claim functions, and claim.pl, on real locks, with a launcher that grants, waits, ignores TERM or refuses: granted and stopped, holder ended (`dead`), waiting and stopped, refused, runner killed, and a late runner of a stopped attempt.

## j90's findings

* 1, a starter overwrites a completed stop: C renames a new token over the old and never truncates it, so a start that holds the old file open writes into the unlinked one (J2); the winclean row with a second link to the token finds the link keeping the nonce and the token holding `stop`.
* 2, a delayed setup rearms a stopped claim, or replaces a retry's token: since 84-148 no setup writes shared state: every file of a claim is per attempt and only created, a stop is a tombstone, and K starts nothing after one or for a run that OUT/windows.current no longer names (L2, L6, U14); the claim rows `late` and `retry` find a late runner starting nothing after the claim's stop and after its attempt's, a new attempt after the claim's stop reading `unstarted`, and the retry running.
* 3, the cleanup lock permits a false grant: `granted` needs the grant lock, which only G holds while the holding shell runs, and no .reaped (L4); the claim row `cleanup` reads nothing, not `granted`, while the launcher outlives its command and while K checks the ledger.
* 4, a missing held marker falsely confirms cleanup: `claim_stop` recovers the ledger and looks up every attempt's .owner, which L writes before its exec (L3, U10); the claim row `ledger` finds it failing while the ledger names the launcher and while recovery fails.
* 5, stop misses a start before its first token: stop.sh runs C's `-a`, which writes `none` to OUT/windows.current before it lists (J3, V9); the winclean row for `-a` finds `none` and every listed run stopped, the last gate's among them.
* 6, a status file hides a job that still runs: .rc is the result only; C's list counts a part until .stopped, sweep.sh's live test likewise, and Obs reports a status only with the job absent, so R ends the claim after the job (G4, T7, T16); the winclean list row lists a run whose part has .rc.
* 7, the fixture's cleanup signals stale numbers: the claim driver signals nothing at its exit; the claim's tombstone and the end files end every fake process, and the exit only waits. Since j102, no scenario signals by number either (j102 1).

## j102's findings

* 1, the orphan row can kill a reused number: the fake setsid of the row `orphan` stays the runner's parent and never reaps it before it has ended, and kills it, its own child, on a file; the launcher ignores TERM before it writes its number, so the TERM of its runner's death cannot end it first, and it ends on the end file, not by a signal.
* 2, a delayed publication removes a retry's GOT: since 84-148 there is no publication and no GOT; nothing of a claim is removed before Y, and a retry is a new attempt with files of its own (L2).
* 3, no row ran the observer's job test or sweep.sh's live test: the rows `observer_rows` run release.sh's `windows_detach` and sweep.sh's Windows script themselves, with a status file beside an active job, and find the observer giving no result until a poll reads the job absent, and sweep.sh keeping such a run's scratch until a stop has written .stopped (G4).
* 4, the claim driver's exit made only one of its end files: it touches all of them and the claim's tombstone, and then waits a minute for every attempt's locks and the supervisor's, which every fake process holds, printing a line that fails the row when one stays held.

## j110's findings

* 1, a stalled publication can keep a stop from finishing: since 84-148 there is no publication and no lock shared between claims; a stop waits only for its own attempts' locks (L1, L2).
* 2, the claim driver's exit could return while its background jobs ran: its scenarios start none that outlives them but the `probe` row's looker, which it waits for.

## j116's findings

* 1, a kill reaches a reused group: no group is made or signalled; the publication and its supervisor are gone.
* 2, a killed supervisor leaves the publication lock held: no lock is shared between claims; a killed K leaves an L that takes TERM by PR_SET_PDEATHSIG and holds only its own attempt's .live, and G reads the tombstone itself (L1, L2, L5).
* 3, a setsid grandchild keeps the lock: it can keep only its attempt's .live; that attempt's stop reports it and nothing else waits, and G has no children (L1).
* 4, a wall-clock deadline: no `time`, `date +%s` or `-mmin` is in the claim path; each wait counts turns (G14).
* 5, ssh ignores SIGALRM: bound.pl stays ssh's parent and sends it KILL (G14); the claim row `bound` finds a child that ignores TERM and ALRM killed at its bound, with status 137.
* 6, the rows miss the deadlines: the claim rows run release.sh's claim functions, and so claim.pl, on real locks, and the `bound` row runs bound.pl on a child that ignores TERM and ALRM.

## j132's findings

* 1, an `ours` check of a number and then a kill, and a signal to the Mac run's group read from a file: no signal by number remains outside a holder; the release's runs are stopped through their hold.pl (G22), and what no holder holds on the Mac is listed and counted (G19). The rows `stop` and `drain` run stop.sh and the Mac run's readers on real holders.
* 2, a count that wraps at 256: stop.sh exits 1 whenever its count is not 0, which 84-155 fixed before this model.

## j141's findings

* 1, signals to processes that nothing holds, stop.sh's Mac pass and the Lenovo's console login: stop.sh's `mac_procs` only lists (G19); the Lenovo's login is ended by its number only in the boot whose ID `screen_free` read, and only while it is init's child with the name read then, since Rach2 gives no number twice within a boot. The rows `stop` and `lenovo` show both.
* 2, a TERM that came before the job started was discarded: the gate reads DIR/stop before the job runs, and a TERM to the holder counts as a stop (G22); the hold rows `early` and `gated`.
* 3, an old holder wrote its end into a recreated directory: each run has a directory of its own, made once, its end is created by a link that fails when it exists, and nothing removes a directory before its holder has ended (G22); the hold rows `again` and `drain`.
* 4, a fast job reaped by the shell before hold.pl held it: the FIFO gate keeps the job from running until its holder holds it, and `-p` refuses a job that is not its unreaped child (G22); the hold rows `fast` and `jobfast`.
* 5, a soft scan and then an unconfirmed removal of -p's copy in WSL: the build and the preparation run under hold.pl in WSL, procs.pl is strict, the ledger is matched by number and start, and the copy is removed only once each held run in it has ended and the scan found nothing left (G19, G22); the hold row `prepare` and the WSL row of a scan out of descriptors.

## j154's findings

84-179 closes these by Fable's four rules, A to D, which G21 to G23 now state.

* 1, a lost session supervisor let another session start in the clone and sweep.sh clean its output: `session.pl` traps HUP too, so only KILL loses it; its record stays, `wait_session` no longer removes it, the next `session.pl` starts nothing until it proves the group gone, and sweep.sh skips a clone whose record stays (G21, G23). The hold rows `session` and `lostsession`; `lostgroup` proves a -g run ended by its empty group.
* 2, the group KILL killed its own holder: the job's group is the anchor's, which the holder is not in (G22); the hold row `groupkill`.
* 3, the gate lost its `go`: the job's side holds the FIFO open from before the holder exists until its reader is open (G22); the hold row `jobwriter`, where held's job opens its reader a second late.
* 4, a child adopted after the last drain signal outlived the drain: every turn signals each new child, KILL every turn from the bound, and the drain ends only on ECHILD (G22); the hold row `adopt`, on Linux.
* 5, the preparation's cleanup succeeded without the ledger confirmed: the recovery always runs, and its failure, a ledger that cannot be read, or an owner that is gone or that the scan stopped keeps the copy (G23); the hold row `recovery`.
* 6, the Windows run's record discarded before a confirmed drain: `windows_forget` keeps it unless the holder has ended, and `windows_wait` waits on the holder's lock, not on the record's last line (G23).
* 7, an unbounded wait on the sync connection: testing.sh's `release_copy` waits half a minute after closing the connection's input, then stops its holder, whose KILL is bounded, and fails, keeping its run directories, when the end is not confirmed.
* 8, fixtures without hold.pl: the testing and launch fixtures stage hold.pl and bound.pl beside testing.sh, and in the copy where no sync reaches it.

## The mediums and lows of 84-169's design round

* Medium 1, a stop written into a held directory that the next start removed: no start removes a run directory, and a stop names a directory made before its record (G22).
* Medium 2, the Mac run never counted: `releases` reads the Mac's run from its holder's lock, and stop.sh -r counts it while it lives (V8, V10); the row `stop` runs -r beside a Mac run that ignores TERM.
* Medium 3, no KILL and no bound: every holder sends KILL a counted bound after its TERM, and `job_wait` reads the lock; the hold rows `kill` and `jobstop`.
* Medium 4, unbounded ssh in stop paths: each ssh of release.sh's `wsl`, of the preparation's cleanup in WSL and of stop.sh goes under bound.pl (G14).
* Medium 5, the Windows run's record removed before its holder drained: `windows_forget` removes STATE/windows.run and W's directory only once W's holder has ended, and otherwise keeps both and fails (V2, G23).
* Medium 6, the Mac preparation's output removed while its group may run: `prepare_stop` removes it only on a confirmed stop.
* Medium 7, the ledger matched by number alone: the cleanup in WSL matches `owner PID START` as procs.pl prints them, or an owner whose start the ledger does not know.
* The lows: a pgrep that failed read as stopped, and an unescaped pattern, are gone with pgrep (G21); a look before the holder's group existed reads `starting` (G12); `wait_session` waits on a lock, never on a group number; the early TERM to a session's group is read before the go; Beat ends by `-P`; `status.sh` probes the loop's lock, and test_status.py reads a group's members from /proc, both for display; RUNS/ID.times is read only once M's holder has ended; sweep.sh judges a loop's session by its supervisor's lock. testing.sh's processes are held (T); what is left of its looks by number signals nothing: its probe of the run lock's owner only refuses a sync, and a number that passed to another process refuses one that could have gone, and `wait`'s pgrep only says whether a run still shows. The installer's status file and winjob.c's `absent` are limits, below.

## The classes that 84-148's design round found unreported

* 1, the hold's expiry compared the clocks of two machines, a 9p mtime against WSL's: G counts its own looks at OUT/ID.alive's size (L7).
* 2, probes took a claim's lock exclusively, so a stalled probe kept `claim_state` silent: lookers probe shared and non-blocking (L1); the claim row `probe` holds an attempt's .live shared and finds a look and a stop answering at once, the attempt `unstarted` and the retry granted.
* 3, stop.sh signalled a process group read from STATE/session with no test of who owns it: it signals only the session's supervisor, by its command line (G21).
* 4, two more wall-clock deadlines, K's half minute between TERM and KILL and `windows_detach`'s hour: both count turns (G14).

## The earlier findings this model removes

* j55 1 and 2, the start and the bash's identity: J1, J2.
* j68 F1, a reused MSYS2 group: no group is signalled; J1.
* The audit's O3, a part launched but not past its PID write: J2, J3; O4, Windows PID reuse: J1; O8, a claim whose script never started: L2, U3; O13, sweep.sh's live test: V13; O14, `claim_stop` by number alone: L1, L3.
* Muse Code's 1.1, an etude that survives a failed cleanup: etude is in the job (J1, G6); 1.2, a start before its first file and a late runner beside a retry: J2, J3, L2; 1.3, a dead runner, a GOT that outlives its holder, a queued orphan: L4, L5; 1.4, the poll's reading of a lost start: T16; 1.5, stop.sh sees only running processes: V9 lists by token; 2.1, 2.2 and 2.4, kills by number, by command line and by group: none is made; 2.5, the claim session's empty branch: no session is signalled.
* Guards that went with them: the earlier G1, G2, G3, G5, G9, G10, G17 and G18, the etude clause of G6, the launch and PID files, and the poll's 300-second rule for an unstarted part.

## Limits

* A holder lost during the cases, Q4d after the step began, leaves WSL unheld until the cases end; the cases go on and may run slower, and nothing is written wrongly.
* An L killed outright leaves G running until etude's keeper ends the case's group, within its two-second grace, so the grant lock can outlast L by that much; `claim_state` reads K's .reaped, which K writes within a second of L's end, so a claim reads `granted` for at most that second after L is gone.
* K's death is reported to L only on x86-64 Linux, where the runner sets PR_SET_PDEATHSIG; elsewhere, as in the host case on the Mac, L ends by the tombstone or by its own end.
* A run of the earlier model still running when this one is installed has no token and no job; no gate lists it. None ran at 84-122's landing. Likewise a claim of 84-144's design has no tombstone; none ran at 84-148's landing.
* Counted sleeps stretch while the host is suspended, which delays an expiry or a bound and never hastens one.
* Three looks that stall while they hold an attempt's .live cost three attempts: a failed claim, never a false one.
* A claim's files stay in LOCKS until Y removes those of a closed milestone; a release that is never closed leaves them, a few hundred bytes for each attempt.
* A hold.pl killed outright reads `lost` until its boot ends or, with -g, its group is empty: nothing stops what it held, and its directory and record stay, so that stop.sh lists the run and nothing starts in its place; a person stops what it held and creates its `end`. On Linux its orphans reparent to the next subreaper or to init. A holder killed between taking its lock and writing DIR/boot started nothing, and still reads lost.
* XNU was not read for two facts G22 and G23 use: that a zombie anchor keeps its group's number, and that a live group's number is not given out as a process's.
* On the Mac a process that no hold.pl holds, one that left its holder's group or outlived a holder killed outright, is only listed and counted; a person stops it by its number. This is the one place where stopping what the sessions left needs a person.
* The Lenovo's login is named by its number and the boot's ID, which is an identity only while Rach2's `pid_next` gives no number twice within a boot ([rach2/sys/proc/Readme.md](../../rach2/sys/proc/Readme.md)).
* The installer's status on the Lenovo is the fixed path `/dev/shm/install.status`, which a second install in the same boot would share; the release runs one install at a time under STATE/lock.
* `winjob state` reads a named job whose last handle has closed as `absent` while its processes may still be ending; `winjob stop` waits for the job's last process, so a stop confirms the end, and only a look reads early.
* A loop or a session started before 84-169 holds neither STATE/loop.live nor STATE/session.live, so stop.sh and status.sh read it as ended until it restarts; the next restart of each loop takes the locks.
