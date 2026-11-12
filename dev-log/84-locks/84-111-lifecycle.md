# 84-111: the lifecycle of a release's Windows parts and WSL claims

This is the model that 84-111's code is written from. It lists every state that a part of a Windows release run and a claim of WSL's admission can be in, which process and which file own each state, every transition with the guard it checks and what it leaves behind, and then places on it each path of the audit in #4783 and each of j68's seven findings. A change of 84-111 names the transition and the guard it implements. Line numbers are those of master 5d6107804, whose `tools/flow` is the same on milestone/76-display; the host case's are those of `tests/cases/host/flow_release.c` on milestone/76-display at 44f6cbabb.

Words: OUT is Windows' output directory, `D:/rach2/build/release`, which WSL sees as `/mnt/d/rach2/build/release`; TREE is Windows' copy, `D:/rach2`; ID is a run's ID; PART is `prepare` or `cases`; KIND is `claim`, the preparation's claim, or `hold`, the whole host's claim for the cases; STATE is `build/flow/release` of the main working tree.

## Processes

| Name | Process | Where | Started by | Stopped by |
|---|---|---|---|---|
| R | `release.sh M SHA`, with or without `-r`: the release | Mac | the manager, in tmux | TERM, INT, HUP (trap `windows_stop`), KILL, a Mac reboot |
| Rp | `release.sh -p M SHA` | Mac | the manager | TERM, INT, HUP, EXIT (trap `prepare_stop`), KILL |
| A | `release.sh -a windows M SHA` | Mac | the manager | ends once it has started W |
| W | `release.sh -w ID M SHA`: a Windows run of -a, leader of its own process group | Mac | A, by `perl setpgrp` and `nohup` (release.sh:2528) | TERM (trap `windows_stop`), KILL; `windows_halt` of R sends TERM to its group |
| M | `release.sh -m ID M SHA` or `-p -m`: the Mac's run, leader of its own group | Mac | R or Rp (release.sh:1202, 2335) | `mac_stop` of a later R; Rp's `prepare_stop` |
| Obs | `windows_detach`'s observer: a background subshell of R, W or Rp (`wd_obs`, `wobs`) | Mac | `windows_prepare`, `windows` | `kill` and `reap` from the parent's trap |
| Prep | `windows_prepare`: a background subshell of R, W or Rp (`wprep`, `pwin`) | Mac | `windows_start`, `prepare_all` | `kill` and `reap` from `windows_stop` or `prepare_stop`; its own TERM trap (release.sh:1724) |
| Beat | `beat ID`: touches `ID.alive` in WSL each minute (`beater`, `wbeat`) | Mac | `windows_prepare`, `windows` | `kill`; ends by itself once its release is gone |
| S | a part's start: MSYS2 `bash -l -s` in the ssh session of `windows_detach`, with its PowerShell that calls Win32_Process.Create | Windows, in the session's Job Object | Obs (release.sh:1340-1377) | the end of its ssh session, which ends every process of the session's job |
| B | a part's bash: `C:/msys64/usr/bin/bash.exe -l OUT/ID.PART.sh`, in no job | Windows | WMI, for S | its script's end; `winclean.sh` |
| G | the MSYS2 process group of B, with B's alive loop and the body's MSYS2 programs (git, make, cp, sh) | Windows | B | `winclean.sh` |
| E | `OUT/etude-ID.exe`, a native program of the body; QEMU runs in its Job Object, which Windows ends with it (tools/etude/win/hold.c:595-616) | Windows | B's body | its end; `winclean.sh` by image name |
| C | `winclean.sh TREE OUT ID...` | Windows, through ssh | `windows_clean` of R, W or Rp; stop.sh | ends by itself, status 0 or 1 |
| K | a claim's script, `sh OUT/ID.KIND.sh`, leader of its own session (`setsid`) | WSL | `claim_start` (release.sh:1541) | its launcher's end; `claim_stop` |
| L | a claim's launcher, `etude-launch build` or `alone`, which execs `etude launch`; the ledger names it `owner PID START KNOWN` | WSL | K | the end of its holding loop; `claim_stop`; the ledger's recovery drops a dead owner |
| X | `stop.sh [-r]` | Mac | the manager or the chat | ends by itself |
| Y | `sweep.sh [-a]` | Mac, with ssh to WSL and Windows | R at its close, stop.sh, the manager | ends by itself |

## Files

| File | Written by | Says | Removed by |
|---|---|---|---|
| OUT/ID.PART.launch | S: made empty first (`: >`), then `P T` by rename, P being the number and T the start time as a FILETIME of the bash that Create made | the part's start ran; once it holds `P T`, which process B is | S, when it starts nothing (cancel or a failed Create); Rp's cleanup; Y at the milestone's close |
| OUT/ID.PART.sh | S, after its cancel check and before Create | the part's script; its presence lists the part for `windows_gate` | Rp's cleanup; Y at close |
| OUT/ID.PART.pid | B, first line of its script, `W GRP` by rename: its Windows number and MSYS2 process group | B runs and has said what it is | S at the start of the same part (`rm -f`); Rp's cleanup; Y at close |
| OUT/ID.PART.cancel | C, first thing for each part it stops | no start or bash of the part does work from now on | Y at close only; Rp's cleanup keeps it |
| OUT/ID.PART.out | B (`exec >`) | the part's output | Rp's cleanup; Y at close |
| OUT/ID.PART.rc | B, by rename, once its body's last line was `status N` | the part ended with status N | Rp's cleanup; Y at close |
| OUT/ID.PART.stopped | C, once it confirmed that nothing of the part runs | the part is stopped for good | Rp's cleanup; Y at close |
| OUT/ID.alive | B's alive loop each minute while B runs; Beat each minute while its release runs | a claim of ID may hold | Y at close |
| OUT/etude-ID.exe and the run's logs, consoles and selection | B's body | the run's own files | Rp's cleanup; Y at close |
| TREE/build/tmp/ID | B's body (`mkdir`), its KEPT and record | the run's directory | B's body when it passed and kept nothing; C unless KEPT; Y once a day unless KEPT or live, and whole at close |
| OUT/ID.KIND.sh, .pid, .started, .owner, .rc, OUT/ID.KIND | `claim_start`'s script and K: the script, K's session, K started, `L START` (field 22 of L's stat), L's status, K's output | the claim's state | `claim_start` of the same ID and KIND (`rm -f rc started pid owner`); Rp's cleanup after `claim_stop`; Y at close |
| OUT/ID.claimed, OUT/ID.held (GOT) | L's holding shell, once admitted | the claim is granted | Y at close |
| OUT/ID.prepared, OUT/ID.end (DONE) | R, W or Rp | the claim may end | Y at close |
| STATE/pid | R and Rp, under STATE/lock (release.sh:2689) | the release that holds the lock | none; a stale number is rejected by guard G12 |
| STATE/windows.run | A (release.sh:2530) | W's number | W's `windows_stop` when it names W; `windows_wait`; `windows_halt` |
| build/machines/mac/run | R or Rp: `PGRP ID` | M's group and run | `mac_wait`, `mac_stop`, `prepare_stop` |
| STATE/record lines `windows started ID`, `windows stopped ID` | `note` under STATE/record.lock | a run began; a stop ended it, so it counts as none of the three | -r's rewrite |
| ledger `/var/lib/rach2-etude/ledger/claims` | L, through etude | a claim or request with `owner PID START KNOWN` | L at its end; `etude status`'s recovery once the owner is dead |

## Guards

Each transition below names the guard it checks. G5, G6, G12's second half, G15 and G16 are new in 84-111; the rest are in master.

* G1, a Windows process by number and start time: the process numbered P is the part's B only while its start time, as a FILETIME, is T; PowerShell holds its handle while it compares and kills, so that the number cannot pass to another process in between (`windows_part` with T, winclean.sh:90-113; the poll, release.sh:1394-1404).
* G2, a Windows process by number and command line: with no start time, the process numbered W is B only while its command line names OUT/ID.PART.sh, held the same way (`windows_part` with no T).
* G3, the cancel mark: C touches OUT/ID.PART.cancel before it reads OUT/ID.PART.launch or OUT/ID.PART.pid; S reads the mark after it made the launch file and before it writes the script and calls Create; B reads it after it wrote its PID file and before any work. So when C finds no launch file, S starts nothing; and when C finds no PID file, any B of the part, made before or after, reads the mark and exits before its alive loop and its body. A part whose PID file C did not find after touching the mark does no work, whatever its launch file says.
* G4, the status file: a part has ended once OUT/ID.PART.rc exists; a line of its output that begins `status ` does not count, but for a part started before release.sh made launch files.
* G5, the group (new): C signals MSYS2 group GRP only when, in the same shell and just before `kill`, the group's leader GRP is B: its `/proc/GRP/cmdline` has OUT/ID.PART.sh as its last argument. While that leader runs, no other process holds the number GRP, so the group is the part's. When the leader is gone, C signals no group; a process of the group counts as the part's, both for a kill and for what is left, only while its `/proc/N/environ` holds the part's mark `RACH2_PART=ID.PART`, which the script exports before its body, and C kills such a process by its own number right after it read the mark. A reused group number thus takes no signal and keeps no part running.
* G6, a confirmed stop is final (new): a part with OUT/ID.PART.stopped has ended, for C as for `windows_gate`. C writes it only once nothing of the part runs, and with the cancel mark present G3 keeps any later start or bash of the part from work, so the file stays true.
* G7, IDs are not reused: each run gets a new ID from `stamp`, and only W's first run takes the ID that A recorded (release.sh:1876-1884).
* G8, the gate: no run and no -p preparation starts in Windows' copy while `windows_gate` lists a part that has not ended by G4 or G6 and C cannot confirm it stopped; the release then exits 75 (release.sh:1815-1838, 1873, 2322).
* G9, a claim's session: `claim_stop` signals the session named by OUT/ID.KIND.pid only while its leader's arguments name OUT/ID.KIND.sh (release.sh:1594-1600).
* G10, a claim's owner: `claim_stop` takes the ledger's claim as the run's only when it reads `owner PID START`, or `owner PID N 0` where the ledger does not know the start, for a member it saw (release.sh:1615-1624).
* G11, claims follow the parts: R, W and Rp end a claim of ID only after C confirmed that nothing of ID runs, or when the part never started; otherwise the claim stays and ends by itself ten minutes after the last touch of OUT/ID.alive (`windows_stop`, `windows_prepare`'s trap, `prepare_windows_clean`).
* G12, a release.sh process: a number from STATE/pid is the release only while `ps` shows it running tools/flow/release.sh, and one from STATE/windows.run only while it runs it with ` -w ` (`releases` in stop.sh:118-127, `windows_runner` in release.sh:1510-1517). New: M's group from build/machines/mac/run is the Mac's run only while its leader runs tools/flow/release.sh with ` -m ` and the ID on the same line; and no process is signalled because its command line merely holds `release.sh`.
* G13, locks: one R or Rp at a time per STATE, by STATE/lock; one A or Rp at a time, by STATE/again.lock; `note` and -r's rewrite hold STATE/record.lock.
* G14, reap: a trap that stops a child waits until `kill -0` fails, since a second signal ends the `wait` builtin early (release.sh:1266-1272).
* G15, the claim sessions are words (new): stop.sh tests a process's session against the list of claim sessions as space-separated words, whatever their number.
* G16, removal by result (new): C decides that a run's scratch is gone from the result of its last `rm -rf`, not from the count of tries.

## The states of a part

| State | Shown by | What runs | Moved on by |
|---|---|---|---|
| P0 absent | no OUT/ID.PART.* | nothing | Obs |
| P1 launching | launch file empty; the script may exist | S, in its ssh session | S |
| P1c start cancelled | cancel mark; no launch file; S printed `start: cancelled` | nothing | final |
| P1f create failed | no launch file; S printed `start: status N` | nothing | final; Obs reads it as unstarted |
| P1x start lost | launch file empty; S gone: TERM to the start's ssh, a lost link, R killed, or no number from Create | possibly a B that has not written its PID file | C |
| P2 created | launch file `P T` | B, before its PID write | B |
| P3 said what it is | PID file `W GRP` | B, at its cancel check | B |
| P3c bash cancelled | PID file and cancel mark, no status file | B exits at once, before its alive loop | final once B is gone |
| P4 running | PID file, no cancel mark when B read it; ID.alive fresh | B, G, E, QEMU in E's job | B |
| P5 ended with status | status file | B ends; its alive loop ends within a minute | final |
| P5x ended without status | no status file; P with T, or W, gone | possibly members of G, or E | C |
| P6 cancel marked | cancel mark, from any state but P5 and P8 | as in the state it was in | C |
| P7 stopping | C between its cancel mark and its verdict | as before, being killed | C |
| P8 stopped and confirmed | stopped file | nothing of the part works, and nothing will, by G3 | final |
| P9 stopped unconfirmed | cancel mark, no stopped file; C exited 1 | may still run | the next C |

The run ID as a whole is cleaned, R1, once both parts are in P5, P8 or never started and no E of ID runs, and C has removed TREE/build/tmp/ID and the tooling paths its record says were made; it is kept, R2, while TREE/build/tmp/ID/KEPT exists, until Y at the milestone's close; and its files are removed, R3, by Rp's cleanup, all but the cancel marks, or by Y at close.

## The states of a claim

| State | Shown by | What runs | Moved on by |
|---|---|---|---|
| Q0 absent | no OUT/ID.KIND.* | nothing | `claim_start` |
| Q1 sent | the script written; no .started and no .rc | maybe nothing: a link lost before the `setsid` line leaves it so | `claim_state` says `unstarted` |
| Q2 started | .pid, .started; no .owner | K | K |
| Q3 waiting | .owner `L START` | K, L as a request in the ledger | L |
| Q4 granted | GOT | K, L holding, its claim in the ledger | DONE or the age of ID.alive |
| Q5 released | .rc after GOT | nothing; the ledger's entry gone with L | final |
| Q6 refused | .rc without GOT | nothing | final; the step fails |
| Q7 stopping | `claim_stop` running | K's session being signalled | `claim_stop` |
| Q8 stopped and confirmed | `claim_stop` exited 0 | nothing, and no ledger entry of a member seen | final |
| Q9 stop unconfirmed | `claim_stop` exited 1, or 255 for a lost link | members or a ledger entry may stay; files kept | a person, as its lines say |
| Q10 left to expire | R did not stop it, by G11 | K, L holding while ID.alive is touched | Q5 ten minutes after the last touch |

## Transitions of a part

| # | Move | Trigger and actor | Guard | Leaves |
|---|---|---|---|---|
| T1 | P0 to P1 | Obs sends the start (release.sh:1335-1379); S removes .out, .pid, .rc and makes the empty launch file | G7 | empty launch file |
| T2 | P1 to P1c | S finds the cancel mark | G3 | cancel mark; Obs writes `lost: it did not start, since winclean.sh had stopped the run` |
| T3 | P1 to P1f | Create returns non-zero | none | the script; Obs polls `unstarted` for 300 s, then writes `lost: it did not start` |
| T4 | P1 to P2 | Create returns P; S renames `P T` into the launch file | records G1's identity | launch file `P T` |
| T5 | P1 to P1x | S ends before writing: TERM to `wd_start` (trap at release.sh:1331), a lost link (ssh 255), R killed, `start: no process number` | none | empty launch file, maybe the script, maybe a B |
| T6 | P2 or P1x to P3 | B renames `W GRP` into its PID file | none | PID file |
| T7 | P3 to P3c | B finds the cancel mark | G3 | no status file |
| T8 | P3 to P4 | no cancel mark; B starts its alive loop and its body | G3 | output, ID.alive, run directory |
| T9 | P4 to P5 | the body's last line is `status N`; B renames N into the status file | G4 | status file |
| T10 | P4 to P5x | B ends without one: killed from outside, by a person, a Windows restart | none | possibly G's members or E |
| T11 | P1 to P5x to P6 | C touches the cancel mark (winclean.sh:137); callers: `windows_stop`, `windows_prepare`'s trap and its end without status (release.sh:1785), `windows` after cases without status (1975), `windows_gate`, `prepare_windows_clean`, stop.sh with no release live | G4, and G6 (new) for a part already stopped | cancel mark |
| T12 | P6 to P7 | C reads the launch and PID files (`launched`, winclean.sh:115-132) | G3 | none |
| T13 | P7 to P8 | C kills P by G1, W by G2, the group by G5, and finds none of them left; it writes the stopped file | G1, G2, G3, G5 (new) | stopped file |
| T14 | P7 to P9 | something still matches after half a minute | as T13 | C exits 1; its caller keeps the claims by G11 |
| T15 | P9 to P7 | a later C: the next start's gate, `windows_stop`, stop.sh | as T13; the group by G5 again, never by a number read once | as T13 or T14 |
| T16 | P8 to P8 | any later C | G6 (new) | nothing; no signal |
| T17 | R1 | C, once no part and no E of ID is left, removes the run directory unless KEPT, retrying six times five seconds apart | G16 (new) | run directory gone, or `cannot remove` and status 1 |
| T18 | E stopped | C: `taskkill /F /T /IM etude-ID.exe`, then tasklist for up to half a minute (winclean.sh:201-205) | the image name holds the ID, so it names no other run | QEMU ends with E's job |
| T19 | R3 | Rp's `prepare_windows_clean` once C and `claim_stop` both confirmed; Y once the milestone closed | G11 | cancel marks stay until Y |

For T12, master's `launched` waits up to half a minute for an empty launch file to name a process and then fails unless a PID file appeared. By G3 the wait is not needed: a part whose PID file C did not find after its mark does no work, and a launch file that names `P T` only adds P for G1. A part in P1x with no B, which a lost link while S ran leaves, therefore goes to P8 at once instead of failing every later C (hole H1 below).

## Transitions of a claim

| # | Move | Trigger and actor | Guard | Leaves |
|---|---|---|---|---|
| U1 | Q0 to Q1 | `claim_start` writes and starts the script (release.sh:1541-1568) | none | .sh |
| U2 | Q1 to Q2 | `setsid nohup sh ID.KIND.sh`; K writes .pid and .started | none | .pid, .started |
| U3 | Q1 stays | the link was lost before the `setsid` line; `claim_state` says `unstarted` | none | the caller runs `claim_stop`, which exits 0 with no files, then U1 again, three tries in all |
| U4 | Q2 to Q3 | K starts L and writes `L START` to .owner | none | .owner |
| U5 | Q3 to Q4 | admitted: L's shell touches GOT and ID.alive | the ledger's admission | GOT |
| U6 | Q3 to Q6 | L ends without GOT; K writes .rc | none | the step fails: `claim refused` or `fail windows` |
| U7 | Q4 to Q5 | DONE touched by R, W or Rp (release.sh:1727, 1789, 1498, 1997), or ID.alive ten minutes old | G11 for DONE | .rc |
| U8 | Q2, Q3 or Q4 to Q7 | `claim_stop` (release.sh:1588-1645): TERM to the session's members, KILL after half a minute, then `etude status` and the ledger for a minute | G9, G10 | Q8, or Q9 with a line for what stays |
| U9 | Q4 to Q10 | R, W or Rp finds the run not confirmed stopped and does not call `claim_stop` | G11 | the claim, until ten minutes after the last touch of ID.alive |
| U10 | none | stop.sh never signals a process of a claim's session, whose leader runs `sh OUT/ID.claim.sh` or `OUT/ID.hold.sh` | G15 (new) | the claim, as R left it |

## Transitions of the release's processes, stop.sh and sweep.sh

| # | Move | Trigger and actor | Guard | Leaves |
|---|---|---|---|---|
| V1 | R or Rp starts | the manager | G13; STATE/pid written under the lock | STATE/pid |
| V2 | R starts on new arguments | `windows_halt` sends TERM to W's group (release.sh:2036-2050) | G12 | STATE/windows.run removed |
| V3 | R or W is stopped | TERM, INT or HUP (W: TERM), and EXIT: `windows_stop` kills Prep and Obs with reap, notes `windows stopped ID`, runs C when it stopped a part, then `claim_stop` of the hold (release.sh:1471-1505) | G14, G11 | Prep's own trap runs C and `claim_stop` of the claim first; W removes STATE/windows.run when it names W |
| V4 | Rp is stopped or ends | `prepare_stop`: Prep with reap, then `prepare_windows_clean`, then the WSL copy (release.sh:2236-2282) | G14, G11 | cancel marks |
| V5 | R, W or Rp killed, or the Mac reboots | no trap runs | none | parts go on detached, P4 to P5 or P5x; claims stay while B touches ID.alive (Q10); stale STATE/pid and STATE/windows.run, which G12 rejects; the next start's gate stops the parts (G8) |
| V6 | the next run starts | `windows_start` and `prepare_all` run `windows_gate` before a new ID | G8, G4, G6 | the earlier parts in P8, or exit 75 |
| V7 | stop.sh while a release is live, no -r | `releases` names a live R, Rp or W | G12 | Windows, M's group, -p's WSL copy and the claim sessions untouched |
| V8 | stop.sh with no release live | `stragglers`: Stop-Process for each program under D:\rach2 but the parts' bashes, then C on each ID that an E or a bash names | G12 to decide that none is live | parts in P8 or P9; claims left to expire |
| V9 | stop.sh -r | TERM to each process `releases` names, up to ten minutes for them to end, then `stragglers` | G12; new: M through its group record, and no process for its command line's `release.sh` alone | R's traps have run V3 |
| V10 | sweep.sh | a run directory is live while E or a bash naming /ID.PART.sh runs, and every one while Windows cannot say; a live or kept run's named paths stay; a closed milestone's runs go whole (sweep.sh:98-144) | live and kept rules | OUT files of open milestones, cancel marks among them |

## The audit's open paths

* O1, no cleanup before a new ID: V6 with G8; the retries in `windows` go through `windows_start`, so through the gate. Closed in master.
* O2, stop.sh without -r stops a live release's parts: V7 with G12 and U10. Closed in master; the claim sessions' test is F3.
* O3, a part launched but not past its PID write: T2, T5, T7, T11 and T12 with G3. Closed in master; master's half-minute wait in T12 leaves H1.
* O4, Windows PID reuse: T13 and T15 with G1 and G2 for B, and the poll with G1. Closed in master for B; for the group, master sends KILL before any check, which is F1, closed by G5.
* O5, `pkill -f 'release\.sh'`: V9 with G12. The pkill is gone in master; the same selection by text survives in `running` and `stragglers`, which is F2.
* O6, a stale STATE/windows.run: V2, V3 and V5 with G12's first half. Closed in master.
* O7, TERM before `wholding=1`: V3; `wholding=1` precedes `claim_start` of the hold (release.sh:1925), and U3's `claim_stop` exits 0 with no files. Closed in master.
* O8, a claim whose script never started: U3. Closed in master.
* O9, stops counted as starts: V3 notes `windows stopped ID`, which `starts` subtracts. Closed in master.
* O10, setup copies no winclean.sh: the host case copies it and mocks PowerShell; F4, F5 and F7 are faults in its rows.
* O11, `cannot remove` with status 0: T17 exits 1. Closed in master; F6 is a fault in that fix, closed by G16.
* O12, a `status ` line in the output: T9 with G4. Closed in master.
* O13, sweep.sh's live test: V10. Closed in master.
* O14, `claim_stop` by number alone: U8 with G10. Closed in master.

The paths the audit found closed:

* Polls ended by a lost link, and whole replies: Obs reads no poll that ssh ended with a status but 0 (release.sh:1416-1430).
* An hour's silence: Obs gives up and writes `lost:`; the caller runs C, T11.
* A part ended without status: Obs reads `ended` by G1 and the caller runs C, T11.
* WSL PIDs: C reads no OUT/ID.KIND.pid; only `claim_stop` does, by G9.
* The stop order for a started part: V3, C before `claim_stop`, by G11.
* TERM at the preparation's wait: Prep's trap, V3, with G14.
* Detached claims: U2, K in its own session.
* A queued claim's stop: U8 from Q3.
* WSL PID reuse of a claim's leader: G9.
* KILL of the release: V5 and V6.
* Unhold after a lost link: `windows_unhold` stops the hold with `claim_stop` when its .rc has not come in a minute (release.sh:1995-2007).
* tasklist failure: C's `stopped` fails when tasklist cannot say, T18, so C exits 1.
* sweep.sh's keep rules: V10.
* Two runs from one STATE: G13.
* -r's take-over of a live -a run: `windows_detached` and `windows_wait` with G12.

The questions the audit left, which 84-98 settled by reading: E's Job Object ends QEMU however E ends (T18); the launcher's command line holds `/etude `, so stop.sh's WHAT matched the claim launchers, hence U10; B is the process of `/proc/$$/winpid`, and the code does not depend on it, since T13 checks P by G1 and W by G2; MSYS2 group-number reuse is now G5; the append of a -w process racing -r's rename holds STATE/record.lock, G13; a second TERM inside a trap is G14.

## j68's seven findings

* F1, cleanup can kill a reused MSYS2 group (winclean.sh:144): T13 and T15 send `kill -KILL -GRP` before any identity check, and T16 does not exist in master, so a second C, such as `windows_stop`'s after Prep's trap, kills the group again. Closed by G5 on T13 and T15, and by G6, which adds T16: `ended` takes the stopped file as an end.
* F2, stop.sh kills processes selected by `release.sh` text (stop.sh:139, 177-180): V9's `running` lists every command line holding `release.sh`, and with -r `stragglers` signals that list on the Mac and in WSL. Closed by G12's second half: `stragglers` signals only what WHAT selects, never a line naming release.sh; R, Rp and W get TERM only through `releases`, and M through its group record with its leader checked; `left` and the choice of `sweep.sh -a` (stop.sh:266) ask `releases`, not the text.
* F3, several claim sessions defeat the exclusion (stop.sh:146-149): U10's list holds one session per line, and `case " $claims "` needs spaces on both sides, so with two sessions neither matches. Closed by G15.
* F4, the gate mock cuts the first byte of each ID (flow_release.c:304): the row that checks V6's exit 75 for a part that C cannot stop reads `ilestone...`. The mock advances by the length of the prefix it matched.
* F5, the claim-refusal row rejects the gate's probe (flow_release.c:1816): U6 for the preparation's claim comes after V6, whose probe runs on Windows' shell. The row allows the gate's probe and asserts that no part's start ran, T1.
* F6, a final removal that succeeds returns failure (winclean.sh:240): T17 reads the count of tries. Closed by G16.
* F7, the PID-reuse row also passes the earlier cleanup (flow_release.c:1967): T13 for a stale part. The row gives the stale launch record, a PID file of the same number and a process group of its own, which the fixture makes and checks after C, so that a cleanup that kills by number or signals the group before checking its leader fails the row.

## Holes the table found

* H1, a start lost before Create stops every later run (T5, T12): an empty launch file with no PID file, as a lost link or a TERM while S ran before Create leaves, makes `launched` fail after half a minute on every C, so `windows_gate` exits 75 on every later start until a person removes the files. Closed by G3 as T12 states it: C needs no wait for the launch file, and takes a part whose PID file it did not find after its mark as doing no work; it still kills a P that the launch file names, by G1.
* H2, the gate misses a part between its launch file and its script (T1): `windows_gate` lists parts by OUT/ID.PART.sh, which S writes after the launch file and its cancel check. A start that lives on after its release, as a Mac that reboots or loses its link can leave until sshd ends the session, could write the script and call Create after a gate that missed it. The gate lists a part by its launch file too.
* H3, `left` reports any command line holding `release.sh`, an editor's among them, as a release, and stop.sh then runs `sweep.sh` without -a (stop.sh:266): part of F2, closed by G12's second half.
* Limit, not a hole: once B is gone, a member of its group whose environment dropped `RACH2_PART` is not the part's by G5, and C neither kills it nor counts it. Only a kill of B alone from outside leaves such a member, and while B runs the group kill reaches every member.
