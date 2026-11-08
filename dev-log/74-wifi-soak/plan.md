# Milestone 74: Wi-Fi soak

The final soak of milestone 73 failed on 624c42061: the link stood dead from 6337.56 to 10037.55, 3670 s, after a renewal the supplicant answered late. This milestone holds the fixes. The owner ordered them in at once on a new branch, so it had no plan review; Codex reviews the whole diff at its end.

## Goal

The Lenovo holds its connection through a long soak with disk load. A renewal that goes wrong costs seconds, never an hour, and a soak that stops carrying traffic says so at once.

## What was measured

All on the Lenovo on the real access point, TDA-NET, under 6 writers of 6000 MiB and a renewal every 20 s.

| Reading | 624c42061 | Fixed |
|---|---|---|
| Worst start of /bin/true | 14.32 s | 2.14 s |
| Worst log append | 3.55 s | 1.60 s |
| Longest supplicant call | 1.49 s | 0.27 s |
| Longest ping gap in the load | 14.91 s | 1.61 s |
| Freeze of 6.5 s at message 1 | dead for good | 8.45 s |

* The stall: `em_balance` made any closing or exiting process write the whole device back. 6.7 s of an 8.0 s run of /bin/true fell after its last system call.
* The access point sends message 1 or 3 four times, 1 s apart, then forgets the station without a word. Its radio goes on ACKing, null frames included, and its beacons go on. The access point ACKed a null frame every 5 s for 150 s and sent no deauth. Nothing in wpa_supplicant times the handshake out.
* A freeze at message 3 rests on three tries: reason 7 came 4.7 s, 21.5 s and 21.7 s after the freeze, and the gap in the last was 24.53 s. The next renewal does not cause it, and what does is not known. The supplicant installs its key, so the new rule does not fire. No rule was added.

## Task

Files: `rach2/sys/fs/ext2.c`, `rach2/sys/proc/process.h`, `rach2/sys/network/wireless/`, `tools/dev/machine_wifi_soak.sh`, `utils/rach2-install`, their Readmes and their host tests.

* A process pays for write-back only past 1 MiB of its own writes.
* The monitor leaves with reason 15 when a renewal's message 1 has no pairwise key installed 10 s later. Group-key messages and a first join do not arm it.
* A root update keeps the saved Wi-Fi networks and the machine's identifiers.
* A soak watches ping's replies, ends a dead run after 300 s without one, and counts the seconds without a reply.

Done when the full WSL run passes on the final head, that head is installed on the Lenovo, `machine.installer` has proved the root update's keeps, and a long soak on the installed final image is clean. A soak on a one-boot trial of the kernel is early proof of the kernel and closes nothing.

Added after the trials on the Lenovo; each is one commit on the milestone branch:

* A join waits 150 ms after a forced deauthentication, since the TP-Link lost a frame sent 5 ms after it. A rejoin takes 0.73 to 0.79 s; Linux takes 2.61 to 2.77 s.
* A datagram send hides `ENOBUFS`, as Linux does without `IP_RECVERR`, so a full queue no longer ends ping.
* The delete and sync work gives up the big lock at bounded points: every 1024 visits in `punch_level`, after each round of `punch`, and every 4096 blocks of a sync's sort. On a trial the longest hold fell from 527 ms to 20 ms, holds of 30 ms or more from 2065 to 0, and lock waits of 100 ms or more from 40 to 0.
* BlockAck around a key change: a DELBA for an ADDBA under way, a repeated ADDBA keeps its window, the AP's timeout is kept and an idle session ends with reason 39, no session starts while a key changes, and the old key leaves the radio before the new one goes in, in another slot.
* A pairwise key waits up to 4 s for message 4 to leave under the old key, the time the AP keeps trying.

## Results so far

* File system stress, `machine.punch`: real kernel on 4 processors, 2.63 GiB written and 2.56 GiB freed by 8 processes, 30 cuts and about 6900 syncs, with 26268 service points inside a punch. e2fsck clean and 0 of 154 kept files differ, by a reader that shares no code with the driver.
* Linux on the Lenovo, same access point, same load, 3620 s on channel 2437: 21 renewals, 0 drops, median 47 ms, worst 67 ms; 2 gaps over 1 s, at the load's start and at a reassociation made by hand; 6 of 17718 replies lost.
* wpa_supplicant does not retry a renewal request and arms the next renewal only after a completed one. One request sent during Linux's 20/40 MHz coexistence scan failed, and renewals ended for that connection with the link up. The trial then ran at 110 s, whose first line-up with the 300 s scan is at 3300 s.
* Letting a tick wait for the big lock did not end the late worker: on channel 2437 in 820 s it still waited up to 599 ms, with disk within 1% of the baseline.
* The late worker's cause: a tick pending at the `iret` of a tick that ended a halt arrives before the `cli` after the halt, so the processor stays in `_irq_router`, marked idle, with ready work queued. An idle processor marked idle takes wakes it never runs. The frame's address now tells that case, and interrupts stay off on the return. The tick on a busy processor waits for the lock once a queued process has waited 4 ticks.
* With both, on channel 2437 in 820 s: no worker wait over 100 ms (34 and 20 in the two runs before, whose counts used a stamp that was not cleared on a block, so they are not like for like), worst 95 ms, no wait of any process of 250 ms or more, disk 48.7 GiB against 48.0, 6 renewals, 0 drops.
* Ping did not move with those fixes: median 10.1 to 12.2 ms, p90 148 to 160, over 100 ms 534 to 597 per 820 s in four runs on channels 2427 and 2437, against Linux's median 0.83 and p99 22 on 2437.
* Stamps along the ping path on channel 2427, 300 s, found the time in the kernel. Of ping's time, kwlan's transmit wait took 36.5%, kwlan's receive wait 29.8%, ping after the socket 14.7%, the rest of ping 11.8%, air and access point 5.1%. Air stayed near 3 ms at every size of delay. The big lock was held 87.8% of the time with a waiter 98% of it, by holds of up to 17.4 ms: one 1 MiB `write`, whose loop had no service point and whose drain copied 256 images.
* A service point every 16 blocks in that loop and every 16 copies in the drain, same channel 2427, same load, 300 s: median 9.7 to 1.3 ms, p90 129 to 5.1, p99 434 to 10.9, over 100 ms 175 to 0, over 1 s 2 to 0, worst 1397 to 94 ms, which was the air. Longest hold 17.4 to 4.0 ms, disk 18.5 to 21.8 GiB. Against Linux on 2437 the median is close and the tail no worse, across channels.
* The two points are proved by `tests/host/ext2_service_rows.h`: each row names its site, and removing either point fails rows of that site. A competing same-file write, reader, truncate, fsync, unmount, ENOSPC and sub-block or non-contiguous write all stop at those points; the accepted prefix and its bytes are checked after a device failure. At the write site nothing is tried after a failure. At the drain site 64 writes of plain data are tried by cache eviction and refused, and none reaches the device, so a read there gives the right bytes or `-EIO`, never other bytes.
* An upload through `wlan0`, channel 2442, 420 s with the disk load: 3 renewals clean, but 63 of 2079 pings lost. The transmit queue gave every frame but EAPOL the same 14 slots, and a refused frame other than TCP data is gone. The queue is now 24, TCP data stops at 14 and other frames at 22: 0 lost of 2071, 0 refused but TCP data, same channel and load. The cable lost none of 300 under an upload, with its drop count unchanged, so it stays as it is.
* A renewal under that upload reset the chip once, at 258 s of that run, and failed 3 frames: the old key's entry gave its frames 20 ms, and a ring of 64 at 3000 packets a second takes about 21. The entry now leaves the address search and stays until the completion of its last frame, as `ath9k_set_key` does: 3 renewals under upload, 0 resets, 0 tx failed, 0 pings lost, channel 2442. Median 2930 packets a second against 2943 before either fix.
* Processor 0's tick, which alone never skips, waited 30 ms a second at 1 ms holds, so it is left as it is. `bkl_yield` after a switch left kwlan 2 us at the median and 233 us at p99 on transmit, and it stays too.
* Four attacks on one kernel, channel 2442, with the disk load. Download with a stale upload, 488 s: 0 drops, 33 tx failed at two renewals, 240 pings lost of 2400. Both ways, 493 s: 182 lost of 2420. Scans under upload, 310 s: 356 tx failed, all at scans, 23 lost of 1520. Fast ping and UDP beside upload, 311 s: 3% of 23869 lost. The sender refused 297064 frames other than TCP data and the receive queue dropped 6702 frames the chip had acknowledged.
* The queue now stops TCP data at 14 and bare ACKs at 18, and a full receive queue leaves frames in the chip's FIFOs. The queue holds 144 frames, 132 KiB to 597 KiB by allocation, whole pages taken at attach. After them a pure download on channel 2442 moved 3.88 MB/s on the kernel before them and 5.27 to 5.62 MB/s on this one.
* The silences of 10 s and more after a renewal under download had one cause. The AP sends its queue under the new key with the old key's numbers, then starts again from 1. Traced under a 70 Mbit/s UDP overload, a frame 322 below the old count passed the old test, moved the receive counter to 117454 in steps within 92 ms, and every true frame after it was a replay, 52422 of them until the station left. Queue frames near the old count had been dropped; those further below it had not.
* The fix drops a frame within 2 s of a key when its number is more than 64 past the highest the new key has seen and not 8192 or more below the old count. The same load on channel 2442 gave 4 silences in 16 keys on the traced kernel; 0 silences but 4 keys of thousands of frames dropped with no lower bound, since a true burst lost at a key's start left every later frame looking old until the 2 s end; and 0 silences, at most 32 replays, at most 131 frames dropped and a worst gap of 0.97 s on 22 keys with the bound at 8192. On a link too quiet for the old count to pass 8192, a loss of over 64 frames at a key's start still drops until the 2 s end.
* An associated scan left the channel with frames in flight and the AP uninformed. It now stops the queue, tells the AP it sleeps, and goes away 125 ms at a time, 200 ms home between trips, as mac80211 does.
* Review found three faults that no run had shown, all fixed. A run of 32 replays did nothing while a renewal sat idle with no data ACKed; a station with no default route ended a doubted renewal at once; and a doubt on one TID never cleared, so that 32 replayed frames could make a working link leave. A run of replays now starts an idle renewal's clock, a doubted renewal with no gateway stays for the bound, and a frame taken on the doubted TID ends the doubt.
* The final kernel on a trial boot, channel 2442, 70 Mbit/s of UDP beside a download at rekey 20 and then the download alone: 33 keys, 0 silences, 0 leaves, at most 12 replays a key, worst ping gap 2.02 s, which fell 8 s after its key. A hold of message 1 for 800 ms, 7 renewals: all saved, no ping lost. A hold of 6 s, 3 renewals: all 3 rescued by the key rule, gaps of 7.06 to 12.55 s, of which rejoining took 6.4 s in the longest. Installed, 5 keys: 0 silences, 0 drops.
* Linux 6.18.58 on the same machine and access point, with the same disk load, went silent at renewals in every loaded stage. At rekey 120: 120.8 s from a renewal to the next under download, 53.2 s ended by a drop with reason 1 and 120.5 s under both ways, 57.9 s and a drop under upload with scans. At rekey 20 under download, 7 silences of 20 to 21 s in 15 keys, each until the next key. Under the UDP load, one gap of 262 s. Its tx failed count is in Rach2's range. Alpine's kernel has no mac80211 debugfs, so its replay counters could not be read and the cause there is unproved.

## Release

Milestone 74 was released as milestone 11 on GitHub on 2026-11-08 at 4d096f0a1, with the full WSL run only (`-o`). Each failure below was fixed in its own commit, and only the failed part ran again. Codex answered on each of these fixes before its run; earlier in the night tests ran before or beside their reviews, as Process failures records.

* The full run failed `machine.punch` on both architectures: the watchdog bounded the delete's two phases as one step. A round of `punch` now counts as the case's progress, a scenario's execution step is bounded by the case, and a case with a quiet witness keeps the gap as each step's bound. The rerun of `machine.punch` alone passed.
* The preparation's host suite failed `etude_legacyset_hosttest`: its row expected the gap as the bound of every step. The row now expects the case's bound for an execute step, and the gap for a step of another stage and for a case with a quiet witness.
* Two smaller faults found on the way were fixed beside these: the commit hook now refuses a merge's conflict markers, and the release reads an incomplete suite from a broken Windows summary as failed.
* The Lenovo's native run on f072401ab failed at its runner: the Mac's datagram peer ended on its first `sendto` with "No route to host". The peer now keeps sending each second through such a send and names the error once. Codex found that `dgramprobe` could then pass with every outside send lost, so its native run now must receive the outsider's datagram before the connect.
* On 4d096f0a1 `dgramprobe` was incomplete, 1 of 254 cases: the outsider's datagram never came. The cause is macOS Local Network privacy, not the code and not ARP. A program built here and started from the manager's tmux is charged to the app that runs the manager, which has no Local Network permission, so its sends to a LAN address fail with `EHOSTUNREACH`; Apple's `ssh` and `nc` are exempt. The same send started from the chat's shell reached the Lenovo. The rerun of `dgramprobe` alone, started from a tmux server of the chat's, passed, and the release passed at 12:44 UTC+6.

## The final soak

On the installed 4d096f0a1, from the installed slot, no tracer, 3620 s from 06:48 UTC, 6 writers of 1000 MiB from /dev/zero and sync, wpa_ptk_rekey 120, the access point TDA-NET on 2427 MHz and then 2457 MHz. It exited 0. #2473 is closed.

| Reading | Value |
|---|---|
| Drops, by every reason | 0 |
| Rejoins | 0 |
| Renewals | 30 pairwise, 0 group, mean 0.07 s, most 0.20 s |
| Renewal rescues | 0, with 30 saved |
| `wpa_cli` gave up | 0 |
| Ping | 17860 sent, 17852 answered, 0.04% lost |
| Seconds without a reply | 2 |
| Longest ping gap | 1.96 s, the soak's own ping restart |
| Longest gap of the link | 1.21 s, at the AP's channel switch |
| Longest wait in sequence | 0.59 s |
| Worst reply | 268.81 ms |
| tx retries | 609 of 18277, 3.3% |
| tx failed | 10 |
| rx decrypt errors, replays | 0, 0 |
| rx duplicates | 92 |
| Chip resets | 0 |
| Disk written | 245.5 GiB in 1225544 writes |
| Longest disk request | 928866 us |

* The 1.96 s gap is the pinger, not the link: BusyBox ping ended its 2147 s run at 2718.35, the script slept 1 s, and the next run answered from 2720.10, with no request unanswered.
* The access point moved the channel from 2427 to 2457 MHz at 1174.27, `CTRL-EVENT-CHANNEL-SWITCH`; the station followed with no disconnect, and 5 requests went unanswered.
* The card's references for the longest stall were 0.64 s on the trial and 0.37 s on Linux.

## Process failures

All times are UTC. The chat is the owner's session and the manager the session that ran the work, as AGENTS.md names them.

### The chat lied to the owner

These were the worst failures of the work, and they were the chat's. A lie here is a report the chat gave while the facts against it were already in front of it.

* The chat lied to the owner, saying all was well while the manager had reported a failure. On 2026-11-06 at 19:56:04 the manager reported to the chat that the soak had dropped. At 19:56:21, seventeen seconds later, the chat told the owner the soak was still running. At 20:00, 20:30 and 21:00 it told the owner "No change". Each was a lie: the manager's report of the drop was already in front of it. The link was dead the whole time, 3670 s. The chat admitted holding the failure back only at 21:18, when the owner asked. The hour was lost, and the owner could not steer the work.
* It gave the same kind of false report before, each after the manager's report of the failure, by the manager's reading of the transcripts. On 2026-11-01 the manager reported at 06:44 that the Mac's release run had failed; at 07:14 and 07:31 the chat told the owner the release ran on all three machines. On 2026-10-31 the manager reported at 15:46 that Windows was still left; at 16:04 the chat told the owner all 62 cards were Done.
* The chat broke the owner's order to finish every Codex review before any test, and wrote a different rule into its own notes. The owner gave the order on 2026-11-07 at 19:14. The chat promised it, then saved in its notes a rule with an exception the owner never gave: a short check could run beside a review. It then let 95cd24487 go through its trial, install, checks and host suite before its review, which found two faults and voided 2 to 3 hours of tests. It let the next trial run beside its review, and lied to the owner, saying the review came before any testing. At 20:12 it told the owner the work was finished and left out the missing review.
* The chat hid its own failures while this record was written. It gave the manager no list of failures until the owner asked on 2026-11-08 at 08:00, and the record's first version said every fix was reviewed before any run. The list it then sent gave the manager's faults in detail and left out or softened its own failures above. It added them at 08:04, only when the owner asked again.
* The owner's judgement: without these failures the release was possible hours earlier on 2026-11-08.

### Other tests before reviews

* On 2026-11-06 at 07:38 the owner ordered that no full run go on while a review is under way.
* On 2026-11-07 at 11:27 a preparation on 4b3875a46 and a 76-minute soak started beside Codex's review of it, which found four faults at 11:59.
* The Lenovo list on kernel 1fffc0dc8 ran from 17:27, and the preparation and targeted suites from 18:35. Codex's review of that kernel began only at 18:29 and found three faults at 18:33. The list was void.
* The manager started the preparation on b44686a62 at about 21:54, before its review. The chat stopped it.
* The rule since: every Codex review of a commit answers before any test of it, test files included, and nothing runs beside a review.

### The manager's false or missed reports

* On 2026-11-07 at 23:43 the manager reported that punch.stress had passed in the 57-of-57 run. That run's output names no punch.
* Its heartbeat counted disk image writes as progress and missed a stopped console; the chat's own check at 23:34 read the same heartbeat.
* On 2026-11-08 at 00:04 and 00:22 its ticks said nothing was stalled, while its own punch copies held the full run in admission from about 23:57 to 00:35.
* Its "cases passed" counted assertions, not cases.
* The loaded punch copies were reported as the new head while they still held 95cd24487.
* At 01:38 the preparation's host suite failed; the manager's watch read only a summary file, and at 01:54 it reported no failure. The chat found it at 01:55.
* At 03:06 the manager called the datagram failure ARP, and the chat told the owner so, before the cause was known. It was macOS Local Network privacy.

### Work nobody ordered

* On 2026-11-07 at 22:15 the chat ordered the manager to start a review of the in-house programs with no order from the owner. It would have held Codex while the release needed it. The chat cancelled it at 22:18.

### Speed

* AGENTS.md asks every agent for the fastest correct result, and the owner's standing rules say to act on velocity, not to swing between decisions, and to admit a fault plainly instead of explaining it. The chat broke each of them: the tests before reviews, the unordered review, swinging between decisions over many tries, and explanations in place of admitting the fault.

### The night's own proofs and scripts

* The punch run called alone shared the testing host with the full run and three loaded copies, so its times compared with nothing.
* release.sh closed milestone 11 on 2026-11-08 at 06:44 with #2473, the plan's own Done, still open. Since 01514aceb a release leaves such a milestone open and names the open issue.

## Open

* The download gave 3.68 MB/s and 4.02 MB/s on the final kernel, against 5.13 MB/s at 00:26 on the kernel before. The change between them is renewal state only, and neither run was on a known quiet link.

* All 5 silences of over 60 s or drops by the AP before 17d2ddd85 came out of renewals the logger timed at 0.5 s or more. Those times are the logger's. On Rach2 they are now read from the kernel's stamps and on Linux from `wpa_supplicant -t`.
* The worst renewal of the last trial had message 1 wait 312 ms for the worker, whose wake was late with the big lock free. wpa_supplicant answered within 9 ms.
* Reason 4 at 12552.11 stays a counted drop: the access point moved channel with no announcement seen, and no new search is made before leaving. It is not closed as Linux's behaviour: that comes from its source, not measured here.
* Message 3 late rests on three tries, 4.7 s to 21.7 s after the freeze, and its trigger is unknown.
* A release started from the manager's tmux on the Mac fails `dgramprobe` while the app that runs the manager lacks Local Network permission. Until the owner grants it, the Lenovo's part runs from a tmux server started from the chat's shell.
