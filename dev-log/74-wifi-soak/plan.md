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
* The queue now stops TCP data at 14 and bare ACKs at 18, and a full receive queue leaves frames in the chip's FIFOs. The queue holds 144 frames, 132 KiB to 597 KiB by allocation, whole pages taken at attach. After them a pure download on channel 2442, kernel before them, moved 3.88 MB/s for reference; the result on this kernel is to come.
* An associated scan left the channel with frames in flight and the AP uninformed. It now stops the queue, tells the AP it sleeps, and goes away 125 ms at a time, 200 ms home between trips, as mac80211 does.

## Open

* All 5 silences of over 60 s or drops by the AP before 17d2ddd85 came out of renewals the logger timed at 0.5 s or more. Those times are the logger's. On Rach2 they are now read from the kernel's stamps and on Linux from `wpa_supplicant -t`.
* The worst renewal of the last trial had message 1 wait 312 ms for the worker, whose wake was late with the big lock free. wpa_supplicant answered within 9 ms.
* Reason 4 at 12552.11 stays a counted drop: the access point moved channel with no announcement seen, and no new search is made before leaving. It is not closed as Linux's behaviour: that comes from its source, not measured here.
* Message 3 late rests on three tries, 4.7 s to 21.7 s after the freeze, and its trigger is unknown.
