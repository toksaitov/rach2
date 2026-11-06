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

## Open

* `wpa_cli reassociate` takes 11 s on the first join after a scan: the supplicant sends reason 2 and joins in 5 ms, one frame fails to decrypt, and the handshake waits out its 10 s. Rejoining after a disconnect takes 20 ms. Open, and held for this milestone: it stays out only if Linux on the Lenovo shows the same, measured.
* Reason 4 at 12552.11 stays a counted drop: the access point moved channel with no announcement seen, and no new search is made before leaving. That mac80211 acts the same comes from its source and was not measured here.
