# 84-111: the lifecycle of a release's Windows parts and WSL claims

This is the model that 84-111's code is written from. It lists every state that a part of a Windows release run and a claim of WSL's admission can be in, which process and which file own each state, every transition with the guard it checks and what it leaves behind, and then places on it each path of the audit in #4783, each of j68's seven findings and each point of Muse Code's review in 84-111b. A change of 84-111 names the transition and the guard it implements. Line numbers are those of master 5d6107804, whose `tools/flow` is the same on milestone/76-display; the host case's are those of `tests/cases/host/flow_release.c` on milestone/76-display.

Words: OUT is Windows' output directory, `D:/rach2/build/release`, which WSL sees as `/mnt/d/rach2/build/release`; TREE is Windows' copy, `D:/rach2`; ID is a run's ID; PART is `prepare` or `cases`; KIND is `claim`, the preparation's claim, or `hold`, the whole host's claim for the cases, and N its attempt, 1 to 3; STATE is `build/flow/release` of the main working tree. A check followed by a signal by number is two steps; where the table accepts the interval between them, it says so and says what keeps it short.

## The precondition

One manager and one STATE own Windows' copy and OUT. The gate's right to cancel any part in OUT that has not ended rests on that ownership, not on a test that the part's release is gone: a release under another STATE, or from another clone, that used the same copy would have its parts stopped by this one's gate. G13's locks are per STATE, so this is a rule of use, which `tools/flow/machines` keeps by naming one copy for the one release process of the main working tree.

## Processes

| Name | Process | Where | Started by | Stopped by |
|---|---|---|---|---|
| R | `release.sh M SHA`, with or without `-r`: the release | Mac | the manager, in tmux | TERM, INT, HUP and EXIT run `windows_stop` once `windows_start` has set the traps (release.sh:1888-1891); KILL; a Mac reboot |
| Rp | `release.sh -p M SHA` | Mac | the manager | TERM, INT, HUP, EXIT (trap `prepare_stop`), KILL |
| A | `release.sh -a windows M SHA` | Mac | the manager | ends once it has started W |
| W | `release.sh -w ID M SHA`: a Windows run of -a, leader of its own process group | Mac | A, by `perl setpgrp` and `nohup` (release.sh:2528) | TERM and EXIT (trap `windows_stop`), KILL; R's `windows_halt` sends TERM to its group |
| M | `release.sh -m ID M SHA` or `-p -m`: the Mac's run, leader of its own group; etude's held commands each get a group of their own (tools/etude/posix/hold.c:308) | Mac | R or Rp (release.sh:1202, 2335) | `mac_stop` of a later R; Rp's `prepare_stop`; stop.sh -r |
| Obs | `windows_detach`'s observer, a background subshell of R, W or Rp | Mac | `windows_prepare`, `windows` | `kill` and `reap` from the parent's trap |
| Prep | `windows_prepare`, a background subshell of R, W or Rp | Mac | `windows_start`, `prepare_all` | `kill` and `reap` from `windows_stop` or `prepare_stop`; its own TERM trap (release.sh:1724) |
| Beat | `beat ID`: touches `ID.alive` in WSL each minute | Mac | `windows_prepare`, `windows` | `kill`; ends once its release is gone, by G12 |
| S | a part's start: MSYS2 `bash -l -s` in the ssh session of `windows_detach`, with its PowerShell that calls Win32_Process.Create | Windows, in the session's Job Object | Obs (release.sh:1340-1377) | the end of its ssh session, which ends every process of the job |
| B | a part's bash: `C:/msys64/usr/bin/bash.exe -l OUT/ID.PART.sh`, in no job, leader of its MSYS2 group | Windows | WMI, for S | its script's end; `winclean.sh` |
| G | B's MSYS2 process group: B's alive loop and the body's MSYS2 programs, each with `RACH2_PART=ID.PART` in its environment | Windows | B | `winclean.sh` |
| E | `OUT/etude-ID.exe`, a native program of the body; QEMU runs in its Job Object, which Windows ends with it (tools/etude/win/hold.c:595-637) | Windows | B's body | its end; `winclean.sh` by image name |
| C | `winclean.sh TREE OUT ID...`, or `winclean.sh -l TREE OUT [ID...]`, which only lists | Windows, through ssh | `windows_clean` and `windows_gate` of R, W or Rp; stop.sh | ends by itself, within a bound its caller sets |
| K | a claim attempt's script, `sh OUT/ID.KIND.N.sh`, leader of its own session (`setsid`) | WSL | `claim_start` (release.sh:1541) | its launcher's end; its cancel mark; `claim_stop` |
| L | the attempt's launcher, `etude-launch build` or `alone`, which execs `etude launch` (tools/etude/launch.sh:14) and runs the holding shell; the ledger names it `owner PID START KNOWN` | WSL | K | the end of its holding shell; `claim_stop`; the ledger's recovery drops a dead owner |
| X | `stop.sh [-r]` | Mac | the manager or the chat | ends by itself |
| Y | `sweep.sh [-a]` | Mac, with ssh to WSL and Windows | R at its close, stop.sh, the manager | ends by itself |

## Files

| File | Written by | Says | Removed by |
|---|---|---|---|
| OUT/ID.PART.launch | S: made empty first (`: >`), then `P T` by rename, P being the number and T the start time, as a FILETIME, of the bash that Create made; T is 0 when S could not read it | the part's start ran; with `P T`, which process B is; with T 0, B's number only | S, when it starts nothing (cancel or a failed Create); Rp's cleanup; Y at the milestone's close |
| OUT/ID.PART.sh | S, after its cancel check and before Create | the part's script | Rp's cleanup; Y at close |
| OUT/ID.PART.pid | B, first line of its script, `W GRP` by rename: its Windows number and MSYS2 group | B runs and has said what it is | S at the start of the same part; Rp's cleanup; Y at close |
| OUT/ID.PART.cancel | C, first thing for each part it stops | no start or bash of the part does work from now on | Y at close only; Rp's cleanup keeps it |
| OUT/ID.PART.out | B (`exec >`) | the part's output | Rp's cleanup; Y at close |
| OUT/ID.PART.rc | B, by rename, once its body's last line was `status N` | the part ended with status N | Rp's cleanup; Y at close |
| OUT/ID.PART.stopped | C, once nothing of the part works and no E of ID runs | the part is stopped for good, in G3's sense | Rp's cleanup; Y at close |
| OUT/ID.alive | B's alive loop each minute while B runs; Beat each minute while its release runs; L's holding shell once at its grant | a claim of ID may hold | Y at close |
| OUT/etude-ID.exe and the run's logs, consoles and selection | B's body | the run's own files | Rp's cleanup; Y at close |
| TREE/build/tmp/ID | B's body (`mkdir`), its KEPT and record | the run's directory | B's body when it passed and kept nothing; C unless KEPT; Y unless KEPT or live, and whole at close |
| OUT/ID.KIND.N.sh, .pid, .started, .owner, .cancel, .rc, OUT/ID.KIND.N | `claim_start`, K and `claim_stop`: the attempt's script, K's session, K started, `L START` by rename (field 22 of L's stat, empty when L had already ended), the attempt's cancel mark, L's status, K's output | the attempt's state | Rp's cleanup after `claim_stop`; Y at close |
| OUT/ID.claimed, OUT/ID.held (GOT) | L's holding shell, once admitted | some attempt was granted | `claim_start`, before each attempt, once every earlier attempt is stopped; Y at close |
| OUT/ID.prepared, OUT/ID.end (DONE) | R, W or Rp | the claim may end | Y at close |
| STATE/pid | R and Rp, under STATE/lock: `PID START`, START being `ps -o lstart=` | the release that holds the lock | none; G12 rejects a stale one |
| STATE/windows.run | A: `PID START` of W | W | W's `windows_stop` when it names W; `windows_wait`; `windows_halt` |
| STATE/prepare/windows | Rp, before its preparation's start: the ID | an ID that Rp may have started on Windows | `prepare_windows_clean` once C confirmed it |
| build/machines/mac/run | R or Rp: `PGRP ID` | M's group and run | `mac_wait`, `mac_stop`, `prepare_stop` |
| STATE/record lines `windows started ID`, `windows stopped ID` | `note` under STATE/record.lock, before the start is sent | a run that may have been started; a stop ended it, so it counts as none of the three | -r's rewrite |
| ledger `/var/lib/rach2-etude/ledger/claims` | L, through etude | a claim or request with `owner PID START KNOWN` | L at its end; `etude status`'s recovery once the owner is dead |

## Guards

Each transition names the guards it checks. Those marked new are written in 84-111; the rest are in master.

* G1, a Windows process by number and start time, checked and killed through one handle (new for the kill): PowerShell opens the process numbered P with `Get-Process` and holds the handle in its Process object; the process is B only while its start time, as a FILETIME, is T; the kill is that object's `Kill()`, which uses the same handle, so the number cannot pass to another process between the check and the kill. B's descendants are killed the same way: each process whose ParentProcessId is P and whose start time is not before P's, opened and checked through its own handle, so that a stale parent number names no child. T of 0 is no start time: the check is G2's. The poll reads the same test without a kill.
* G2, a Windows process by number and exact command line (new): with no start time, the process numbered W is B only while the CIM instance of that number has the held process's start time as its creation date, so that both name one process, and its command line is exactly `C:/msys64/usr/bin/bash.exe -l OUT/ID.PART.sh`, the line Create was given. The kill is G1's, through the held handle.
* G3, the cancel mark: C touches OUT/ID.PART.cancel before it reads the part's launch or PID file; S reads the mark after it made the launch file and before it writes the script and calls Create; B reads it after it wrote its PID file and before its alive loop and its body. So when C finds no launch file, S starts nothing; and when C finds no PID file, any B of the part, made before or after, exits at its cancel check. "Stopped" in this table means that no process of the part does work and none will: a bash that Create makes after C's mark may run its login profile and then exits at its check. G6, G8, G11 and P8 use this meaning.
* G4, the status file (narrowed, new): a part has ended once OUT/ID.PART.rc or OUT/ID.PART.stopped exists. A `status ` line in its output does not count; master's branch for parts made before launch files is dropped, since no such part can still run and C confirms one at most once.
* G5, the group (new): C signals MSYS2 group GRP only when, just before the `kill`, the group's leader is B: `/proc/GRP/winpid` is W and the Windows process W is the part by G1, when W is P, or by G2. While B runs it holds the number GRP, so the group is the part's; the interval between that read and the `kill` builtin, in the same shell, is the one this table accepts, since B would have to end and its number pass to another group in it. When the leader is gone, C signals no group: a process N counts as the part's, for a kill and for what is left, only while `/proc/N/pgid` is GRP and `/proc/N/environ` holds `RACH2_PART=ID.PART`, which the script exports before its alive loop; C kills N by number right after that read, the same accepted interval. winclean.sh reads `/proc` under `${WINCLEAN_PROC:-/proc}`, so that the host case gives it a tree of its own. MSYS2 on the testing host has `/proc/N/winpid`, `pgid` and `environ`, and its numbers are its own, apart from Windows' (read on the testing host for this table).
* G6, a confirmed stop is final (new): C takes a part with OUT/ID.PART.stopped as ended and touches it no more. C writes the stopped files of a run only after it confirmed that no E of the run runs, so that a stopped part never hides a live etude from the gate.
* G7, IDs are not reused: each run gets a new ID from `stamp`, and only W's first run takes the ID that A recorded (release.sh:1876-1884).
* G8, the gate (widened, new): no run and no -p preparation starts in Windows' copy until C has confirmed every run ID that `winclean.sh -l` lists: each ID of OUT with a part's script or launch file and no end by G4, and each ID that the record's `windows started` lines or STATE/prepare/windows name and that has not ended, save the ID being started. The second source covers a start that was sent and has made no file yet: C's mark keeps it from work when it comes. A list that cannot read OUT fails, and the release exits 75 (release.sh:1815-1838, 1873, 2322).
* G9, a claim's session (narrowed, new): `claim_stop` signals the session that OUT/ID.KIND.N.pid names only while its leader's arguments name OUT/ID.KIND.N.sh. With the leader gone it signals no session; it signals L alone, by number, only while L's start is the one in OUT/ID.KIND.N.owner, the accepted interval.
* G10, a claim's owner: `claim_stop` takes a ledger claim as the attempt's when it reads `owner PID START` for a member it saw, or `owner PID N 0` where the ledger does not know the start. An .owner whose start is empty matches by number alone, which can only keep a claim reported, Q9, never end one.
* G11, claims follow the parts: R, W and Rp end a claim of ID only after C confirmed ID stopped, or when no part of ID was started; otherwise the claim stays and ends by itself ten minutes after the last touch of OUT/ID.alive.
* G12, a release.sh process (new but the first clause): a number from STATE/pid or STATE/windows.run is the release only while `ps` gives it the start the file records and shows it running tools/flow/release.sh, with ` -w ` for STATE/windows.run; M's group from build/machines/mac/run is the Mac's run only while its leader runs tools/flow/release.sh with `-m ID` on the same line, the ID being unique. stop.sh and release.sh use the one test. No process is signalled because its command line holds `release.sh`. Beat ends once R's number no longer has R's start.
* G13, locks: one R or Rp at a time per STATE, by STATE/lock; one A or Rp at a time, by STATE/again.lock; `note` and -r's rewrite hold STATE/record.lock.
* G14, reap with a bound (new for the bound): a trap that stops a child waits until `kill -0` fails; every C that a trap runs is bounded by an alarm, so the wait ends, and a C that the alarm ends counts as unconfirmed.
* G15, the claim sessions (new): stop.sh never signals a process whose session is a claim's: the sessions of live `sh OUT/ID.KIND.N.sh` leaders and the sessions that each OUT/ID.KIND.N.pid names, tested as space-separated words.
* G16, removal by result (new): C decides that a run's scratch is gone from the result of its last `rm -rf`, not from the count of tries.
* G17, a claim attempt's cancel mark (new): `claim_stop ID KIND N` touches OUT/ID.KIND.I.cancel for each attempt I up to N before it reads that attempt's .pid; K writes .pid and .started, then exits before starting L when its cancel mark exists. So an attempt whose .pid `claim_stop` did not find never starts L, and a retry, attempt N+1, has files of its own that no late K of an earlier attempt writes.
* G18, a live holder (new): `claim_state` says `granted` only while GOT exists and the attempt's K leader runs its script, by G9's test; it says `dead` when the attempt started, has no .rc and its leader no longer runs, as a killed K or a WSL restart leaves it. Callers treat `dead` as `unstarted`: `claim_stop`, then another attempt, three in all.
* G19, stop's kills at the kill site (new): stop.sh selects and kills in one pass on each machine: on the Mac and in WSL one script reads the processes and signals them at once, and on Windows one PowerShell command selects each program under D:\rach2 and kills it through its own handle; no list of numbers waits across an ssh call.

## The states of a part

| State | Shown by | What runs | Moved on by |
|---|---|---|---|
| P0 absent | no OUT/ID.PART.*; the record or STATE/prepare/windows may name ID | nothing | Obs |
| P0s start sent | as P0, while S's script waits in its ssh session | S, before its first line | S; by G8 a later gate's C marks the part cancelled |
| P1 launching | launch file empty; the script may exist | S | S |
| P1c start cancelled | cancel mark; no launch file; S printed `start: cancelled` | nothing | final |
| P1f create failed | no launch file; S printed `start: status N` | nothing | final; Obs reads it as unstarted |
| P1x start lost | launch file empty; S gone: TERM to the start's ssh, a lost link, R killed, or no number from Create | maybe a B | C |
| P2 created | launch file `P T`, or `P 0` | B, before its PID write | B |
| P3 said what it is | PID file `W GRP` | B, at its cancel check | B |
| P3c bash cancelled | PID file and cancel mark, no status file | B exits at once | final once B is gone |
| P4 running | PID file, no cancel mark when B read it; ID.alive fresh; the launch file `P T`, `P 0` or, after P1x, empty | B, G, E, QEMU in E's job | B |
| P5 ended with status | status file | B ends; its alive loop ends within a minute | final |
| P5x ended without status | no status file; P, or W, gone | possibly members of G, or E | C |
| P6 cancel marked | cancel mark, from any state but P5 and P8 | as in the state it was in | C |
| P7 stopping | C between its mark and its verdict | as before, being killed | C |
| P8 stopped and confirmed | stopped file | nothing of the part works, and no E of ID runs | final |
| P9 stopped unconfirmed | cancel mark, no stopped file; C exited 1, or its alarm ended it | may still run | the next C |

The run ID as a whole is cleaned, R1, once both parts are in P5 or P8 and C has removed TREE/build/tmp/ID and the tooling paths its record says were made; it is kept, R2, while TREE/build/tmp/ID/KEPT exists, until Y at the milestone's close; and its files are removed, R3, by Rp's cleanup, all but the cancel marks, or by Y at close. E is a state of the run, not of a part: C kills and confirms it before it writes either part's stopped file (G6).

## The states of a claim attempt

| State | Shown by | What runs | Moved on by |
|---|---|---|---|
| Q0 absent | no OUT/ID.KIND.N.* | nothing | `claim_start` |
| Q0s sent | the script written or on its way; no .started, no .rc | maybe nothing yet, maybe a K about to start | K, or G17's mark from `claim_stop` |
| Q1 unstarted | as Q0s after `claim_start`'s half minute | as Q0s | `claim_state` says `unstarted`; `claim_stop` marks it cancelled |
| Q2 started | .pid, .started; no .owner | K, at its cancel check, then starting L | K |
| Q2c cancelled | .pid, .started and the cancel mark | K exits before L | final |
| Q2d dead | .started, no .rc, K's leader gone: K killed, WSL restarted | maybe L, if only K was killed | `claim_state` says `dead`; `claim_stop` |
| Q3 waiting | .owner `L START` | K, L as a request in the ledger | L |
| Q4 granted | GOT, and K's leader runs | K, L holding, its claim in the ledger | DONE or the age of ID.alive |
| Q4d holder lost | GOT, but K's leader gone, as a WSL or host restart leaves it | nothing holds WSL | `claim_state` says `dead` before the step starts; during the step, nothing (Limits) |
| Q5 released | .rc after GOT | nothing; the ledger's entry gone with L | final |
| Q6 refused | .rc without GOT | nothing | final; the step fails |
| Q7 stopping | `claim_stop` running | the attempt's session being signalled | `claim_stop` |
| Q8 stopped and confirmed | `claim_stop` exited 0 | nothing of the attempt, and no ledger entry of a member seen | final |
| Q9 stop unconfirmed | `claim_stop` exited 1, or 255 for a lost link | members or a ledger entry may stay; files kept | a person, as its lines say |
| Q10 left to expire | R did not stop it, by G11 | K, L holding while ID.alive is touched | Q5 ten minutes after the last touch |

## Transitions of a part

| # | Move | Trigger and actor | Guard | Leaves |
|---|---|---|---|---|
| T0 | P0 to P0s | R or W notes `windows started ID`, or Rp writes STATE/prepare/windows, then Obs sends the start | G7 | the record's line or the prepare file |
| T1 | P0s to P1 | S removes .out, .pid, .rc and makes the empty launch file (release.sh:1335-1379) | none | empty launch file |
| T2 | P1 to P1c | S finds the cancel mark | G3 | cancel mark; Obs writes `lost: it did not start, since winclean.sh had stopped the run` |
| T3 | P1 to P1f | Create returns non-zero | none | the script; Obs polls `unstarted` for 300 s, then writes `lost: it did not start` |
| T4 | P1 to P2 | Create returns P; S renames `P T` into the launch file, T 0 when Get-Process found nothing | records G1's identity | launch file |
| T5 | P1 to P1x | S ends before writing: TERM to `wd_start` (release.sh:1331), a lost link, R killed, `start: no process number` | none | empty launch file, maybe the script, maybe a B |
| T6 | P2 or P1x to P3 | B renames `W GRP` into its PID file | none | PID file |
| T7 | P3 to P3c | B finds the cancel mark | G3 | no status file |
| T8 | P3 to P4 | no mark; B exports `RACH2_PART`, starts its alive loop and its body | G3 | output, ID.alive, run directory |
| T9 | P4 to P5 | the body's last line is `status N`; B renames N into the status file | G4 | status file |
| T10 | P4 to P5x | B ends without one: killed from outside, a Windows restart | none | possibly G's members or E |
| T11 | any but P5 and P8, to P6 | C touches the cancel mark (winclean.sh:137); callers: `windows_stop`, Prep's trap and its end without status (release.sh:1785), `windows` after cases without status (1975), the gate, `prepare_windows_clean`, stop.sh with no release live | G4, G6 | cancel mark |
| T12 | P6 to P7 | C reads the launch and PID files once, with no wait | G3 | none |
| T13 | P7 to P8 | C kills P by G1 or G2, W by G2 when W is not P, and the group or its marked members by G5; finds none of them left; then kills E by name and confirms it gone (T18); then writes the stopped file | G1, G2, G3, G5, G6 | stopped file |
| T14 | P7 to P9 | something still matches after half a minute, E still runs, or Windows cannot say | as T13 | C exits 1; its caller keeps the claims by G11 |
| T15 | P9 to P7 | a later C: the next gate, `windows_stop`, stop.sh | as T13, every check made again | as T13 or T14 |
| T16 | P8 to P8 | any later C | G6 | nothing; no signal |
| T17 | R1 | C, once both parts are ended, removes the run directory unless KEPT, retrying six times five seconds apart | G16 | run directory gone, or `cannot remove` and status 1 |
| T18 | E stopped | C: `taskkill /F /T /IM etude-ID.exe`, then tasklist for up to half a minute (winclean.sh:201-205) | the image name holds the ID | QEMU ends with E's job |
| T19 | R3 | Rp's `prepare_windows_clean` once C and `claim_stop` both confirmed; Y once the milestone closed | G11 | cancel marks stay until Y |
| T20 | Obs reads P4 | the poll takes P from the launch file and checks it by G1, or by G2 when T is 0; with an empty launch file and a PID file, it checks W by G2; it says `unstarted` only with neither | G1, G2 | a part that a lost start left running is polled, not cancelled after 300 s |
| T21 | Obs gives up | no whole answer for an hour, or `unstarted` for 300 s; the caller runs C | G14 | as T11 |

For T12, master's `launched` waits up to half a minute for an empty launch file to name a process and then fails unless a PID file appeared, on every later C too. By G3 the wait is not needed: a part whose PID file C did not find after its mark does no work, and a launch file that names P only adds P for G1. A part in P1x with no B therefore goes to P8 at once (H1).

## Transitions of a claim attempt

| # | Move | Trigger and actor | Guard | Leaves |
|---|---|---|---|---|
| U1 | Q0 to Q0s | `claim_start ID KIND N` removes GOT and writes and starts the attempt's script (release.sh:1541-1568); the caller has stopped every earlier attempt | G17 | .sh |
| U2 | Q0s to Q2 | `setsid nohup sh ID.KIND.N.sh`; K writes .pid and .started | none | .pid, .started |
| U3 | Q0s to Q1 | half a minute without .started; `claim_state` says `unstarted` | none | the caller runs `claim_stop ID KIND N`, which marks the attempt cancelled, then U1 with N+1, three attempts in all |
| U4 | Q2 to Q2c | K finds its cancel mark | G17 | no L |
| U5 | Q2 to Q3 | no mark; K starts L and renames `L START` into .owner | G17 | .owner |
| U6 | Q2 or Q3 to Q2d | K killed, or WSL restarted | none | `claim_state` says `dead`; the caller runs U3's path |
| U7 | Q3 to Q4 | admitted: L's holding shell touches GOT and ID.alive | the ledger's admission | GOT |
| U8 | Q3 to Q6 | L ends without GOT; K writes .rc | none | the step fails: `claim refused` or `fail windows` |
| U9 | Q3 or Q4 to Q5 | DONE touched by R, W or Rp (release.sh:1727, 1789, 1498, 1997), or ID.alive ten minutes old; DONE while Q3 ends the holder at its grant | G11 for DONE | .rc |
| U10 | Q4 to Q4d | WSL or the host restarts | none | GOT stays; G18 reads it as `dead` |
| U11 | Q0s to Q4 to Q7 | `claim_stop ID KIND N` (release.sh:1588-1645): for each attempt up to N, the cancel mark, then TERM to the session's members, KILL after half a minute, then `etude status` and the ledger for a minute | G17, G9, G10 | Q8, or Q9 with a line for what stays |
| U12 | Q4 to Q10 | R, W or Rp finds the run not confirmed stopped and calls no `claim_stop` | G11 | the claim, until ten minutes after the last touch of ID.alive |
| U13 | none | stop.sh signals no process of a claim's session | G15 | the claim, as R left it |

The final stops, `windows_stop`, Prep's trap, `windows_unhold` and `prepare_windows_clean`, pass N 3, so that every attempt, started or on its way, is marked and stopped.

## Transitions of the release's processes, stop.sh and sweep.sh

| # | Move | Trigger and actor | Guard | Leaves |
|---|---|---|---|---|
| V1 | R or Rp starts | the manager | G13 | STATE/pid `PID START` |
| V2 | R starts on new arguments | `windows_halt` sends TERM to W's group (release.sh:2036-2050); W is its group's leader, so its number is held between the check and the signal | G12 | STATE/windows.run removed |
| V3 | R or W is stopped | `windows_stop` (release.sh:1471-1505): Prep and Obs with reap, `windows stopped ID`, C when it stopped a part, then `claim_stop` of the hold | G14, G11 | Prep's own trap runs C and `claim_stop` of the claim first; W removes STATE/windows.run when it names W |
| V3a | R is stopped before `windows_start` set its traps | TERM, INT or HUP end R with no `windows_stop` | none | nothing of R's own runs on Windows then: a live W is its own process, and R's Prep, Obs and Beat exist only after the traps |
| V4 | Rp is stopped or ends | `prepare_stop` (release.sh:2236-2282) | G14, G11 | cancel marks |
| V5 | R, W or Rp killed, or the Mac reboots | no trap runs | none | parts go on detached; claims stay while B touches ID.alive (Q10); a queued attempt may still be admitted later, and then holds ten minutes from its grant; stale PID records, which G12 rejects |
| V6 | the next run starts | `windows_start` and `prepare_all` run the gate before a new ID | G8, G4, G6 | the earlier runs in P8, or exit 75 |
| V7 | `mac_stop` and the Mac record's readers | `mac_stop` sends TERM to M's group only after G12's leader check; with the leader gone it signals nothing, removes the record and checks the ledger as now. `mac_alive`, `mac_wait`, `prepare_all`, `restart` and stop.sh probe the group with signal 0, which harms nothing | G12 | the record removed |
| V8 | stop.sh while a release is live, no -r | `releases` names a live R, Rp or W | G12 | Windows untouched; on the Mac every descendant of M's leader, whatever its group; -p's WSL copy; the claim sessions |
| V9 | stop.sh with no release live | the gate's list, from OUT, the record and STATE/prepare/windows, and the IDs that an E or a part's bash names; C on them; then each program under D:\rach2 but a part's bash, selected and killed in one PowerShell pass | G12, G8, G19 | parts in P8 or P9; claims left to expire |
| V10 | stop.sh -r | TERM to each process `releases` names and to M's group by G12, up to ten minutes for them to end; a release still live then is left alone, reported as left, and the rest runs as V8 | G12 | R's traps have run V3, or the release stays |
| V11 | stop.sh on the Mac and in WSL | one pass selects what WHAT names, never for the text `release.sh`, and signals it | G19, G15 | none |
| V12 | sweep.sh | a run directory is live while E or a part's bash runs, while a part of the ID has not ended by G4, and every one while Windows cannot say; a live or kept run's named paths stay; a closed milestone's runs go whole (sweep.sh:98-144) | G4, the live and kept rules | OUT files of open milestones, cancel marks among them |

## The audit's open paths

* O1, no cleanup before a new ID: V6 with G8, now also over a start sent but not begun (P0s) and a list that cannot read OUT.
* O2, stop.sh without -r stops a live release's parts: V8 with G12 and G15. Master spares only M's own group; etude's held commands have groups of their own, so V8 spares M's descendants, and G15 spares a claim's session whose K is gone.
* O3, a part launched but not past its PID write: T2, T5, T7, T11 and T12 with G3, and T0 and G8 for a start before its first file; T20 for the poll's reading of a lost start.
* O4, Windows PID reuse: T13 and T15 with G1 and G2, which now kill through the checked handle and compare the whole command line, and G5 for the group (F1).
* O5, selection by `release.sh` text: G12, V10 and V11.
* O6, a stale STATE/windows.run: G12 in both release.sh and stop.sh, with the start and ` -w `.
* O7, TERM before `wholding=1`: closed in master by the order at release.sh:1925; a late K after the stop is G17's.
* O8, a claim whose script never started: U3 with G17, so a retry cannot share files with a late K.
* O9, stops counted as starts: V3. Closed in master; V3a is its edge.
* O10, setup copies no winclean.sh: closed in master; F4, F5 and F7 are faults in its rows, and H1 and H2 change two rows and the gate mock.
* O11, `cannot remove` with status 0: T17 with G16 (F6).
* O12, a `status ` line in the output: T9 with G4, now without the legacy branch.
* O13, sweep.sh's live test: V12, now also for a part not ended.
* O14, `claim_stop` by number alone: U11 with G10; .owner by rename.

The paths the audit found closed, corrected where the review showed they were not:

* Polls ended by a lost link, whole replies, an hour's silence, a part ended without status: T21 and T20.
* WSL PIDs: C reads no claim file; only `claim_stop` does.
* The stop order for a started part, TERM at the preparation's wait: V3 with G11 and G14, now bounded.
* Detached claims, a queued claim's stop: U2 and U11.
* WSL PID reuse of a claim's leader: master's empty-leader branch signals a reused session; G9 narrowed.
* KILL of the release: V5 and V6, with T0 for a start before its first file and G17 and G18 for its claims.
* Unhold after a lost link: `windows_unhold` stops the hold after a minute without .rc.
* tasklist failure: T18; the two `cd` failures are H4.
* sweep.sh's keep rules, two runs from one STATE, -r's take-over of a live -a run: V12, G13, G12.
* A second TERM inside a trap: G14, with C bounded, and V10 for a release still live after stop.sh's ten minutes.

## j68's seven findings

* F1, a reused MSYS2 group (winclean.sh:144): T13, T15 and T16. G5 checks the leader first, G6 takes the stopped file as an end; the accepted interval is stated in G5.
* F2, kills selected by `release.sh` text (stop.sh:139, 177-180): V10 and V11 with G12 and G19; `left` and the choice of `sweep.sh -a` (stop.sh:266) ask `releases`.
* F3, several claim sessions (stop.sh:146-149): U13 with G15.
* F4, the gate mock cuts the first byte (flow_release.c:304): advance by the prefix's length.
* F5, the claim-refusal row rejects the gate's probe (flow_release.c:1816): allow it; assert no part's start, T1.
* F6, a final removal that succeeds returns failure (winclean.sh:240): G16.
* F7, the PID-reuse row passes the earlier cleanup (flow_release.c:1967): the row gives the stale launch record, a PID file of the same number and a group the fixture made; `powershell_mock` answers a command line that is not the part's for a number it is told is reused, and the fixture checks that the group survives.

## Muse Code's review, 84-111b

Taken, each checked against the code:

* 1.1, an E that survives a failed cleanup behind a stopped part: G6, T13, T14.
* 1.2, a start before its first file, and a late K beside a retry: P0s, T0, G8; Q0s, G17, per-attempt files.
* 1.3, a dead K, a GOT that outlives its holder, a queued orphan: Q2d, Q4d, G18, U6, U10; V5 for the orphan, bounded by ten minutes.
* 1.4, the poll's reading of a lost start: T20.
* 1.5, stop.sh sees only running processes: V9 lists like the gate.
* 1.6: `mac_stop`, V7; R before its traps, V3a; the precondition; `P 0`, in the Files table and G1; DONE while Q3, U9.
* 2.1, G1's kill by number: the kill through the held handle.
* 2.2, G2's substring: the exact command line and one process by creation date.
* 2.3, `mac_stop`'s blind group: V7; `windows_halt`, V2.
* 2.4, G5 as a proof: the accepted interval stated, the comparison by `winpid`, which needs no path spelling, and a `/proc` root the host case can give. Its limit covers `setsid` and native children (Limits).
* 2.5, G9's empty branch: narrowed.
* 2.6, `releases` against `windows_runner`: one test, G12.
* 2.7, stop's snapshot lists: G19.
* 2.8, the two `cd` failures: H4.
* 2.9: G10's empty start, stated; G4's legacy branch, dropped; one meaning of stopped, G3.
* 3, the rows: F7's mock, H1's reversed row, H2's gate mock, and rows for stop.sh's F2 and F3 (Rows).
* 4: the unbounded reap, G14; Beat's bare number, G12; sweep's members-only miss, V12; V10 for a release still live.

Set aside, with the reason:

* stop.sh's WHAT matching an editor that holds `tests/run` on its command line, and V9's stop of every program under D:\rach2: stop.sh exists to leave nothing of the factory running on its machines, and D:\rach2 is the factory's copy by the machines file; neither is a path of the release's lifecycle.
* A closed milestone's runs removed with no live test (sweep.sh:141): release.sh refuses a milestone that is not open (release.sh:2386-2395), so no run of a closed milestone starts, and the precondition keeps any other release out of the copy.
* The poll and `windows_wait` on a part or a W that hangs: unbounded by policy, since a suite sets no time limit of its own; a person stops it.

## Holes the table found

* H1, a start lost before Create stops every later run (T5, T12): closed by G3 as T12 states it.
* H2, the gate misses a part between its launch file and its script, and before its first file (T0, T1): G8 lists by the launch file and by the record.
* H3, stop.sh's `left` and its choice of `sweep.sh -a` read `release.sh` text: G12.
* H4, "cannot say" taken as "confirmed": `winclean.sh` exits 0 when it cannot enter OUT (winclean.sh:59), and the gate's list does (release.sh:1818); both fail now, the gate exiting 75.

## Limits

* Once B is gone, a member of its group whose environment dropped `RACH2_PART`, one that called `setsid`, and a native child that MSYS2 shows no environment for, are not the part's by G5; C neither kills nor counts them. Only a kill of B alone from outside leaves them, and while B runs the group kill reaches every MSYS2 member; E is found by its name.
* A holder lost during the cases, Q4d after the step began, leaves WSL unheld until the cases end; the cases go on and may run slower, and nothing is written wrongly.
* The checks that G5 and G9 make before a signal by number, and V2's, leave the interval G5 states; G1 and G2 leave none.

## Rows

The host case, compile only, Codex judging the rows on reading:

* F4: `gate_mock` advances by the prefix's length, and matches the gate's new list (H2), which `windows_mock` recognises by its new first line.
* F5: the claim-refusal row allows the gate's probe and checks that no part's start ran.
* F7: as above.
* H1: the row that expected a failure while a start had not said what it created expects the part stopped.
* G5: a row in which a verified leader's group is killed, and F7's in which a stale one survives, both through `WINCLEAN_PROC`.
* G16: a row whose removal succeeds on its last try.
* G17 and G18: the mocks follow the attempts' file names; a row in which a dead K's attempt is stopped and another started.
* F2 and F3: rows that run stop.sh with mocks for ssh and gh: a process whose command line holds `tools/flow/release.sh` and that no record names survives `stop.sh -r`, and two claim sessions are both spared in WSL.
