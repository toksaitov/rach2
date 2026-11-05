# 73 wifi steady

The goal as a user sees it: the Lenovo's Wi-Fi signal reads true at all times, a short silence from the AP no longer ends the connection, and the radio switch leaves the system quiet and rejoins by itself. It is also a test of the factory: the accounts' loops and the manager's take the tasks from the board, and the milestone runs to the Lenovo through the usual pipeline.

Evidence: `build/flow/coordinator/lenovo-hotspot-20261104/findings.md`.

## The owner's decisions

* The three fixes below, ordered on 2026-11-05 as the approval of this scope.
* No release on the Mac or on Windows, this once. The end's checks run only in WSL, then one full run of every suite on both architectures in WSL, under TCG. Its wall time and failures go to the owner, with the last release's WSL run beside them, to decide whether TCG stays the default there.
* If that run passes, the release installs on a trial boot, runs the native cases and the acceptance, commits, moves master, closes the milestone and clears the board. If the code proves worthless, the installed image stays.
* Left out by the owner's choice: the 11 s `wpa_cli reassociate` and the small `iw` gaps.

## Tasks

The tasks touch different code. Tasks 2 and 3 share `rach2/sys/network/wireless/Readme.md`, `tests/host/Readme.md` and `tests/host/wlan_join_hosttest.c`: task 2 owns the paragraphs and rows on beacon loss and task 3 those on the switch, and a rebase settles the rest.

### 1. Signal: read the noise level, not the raw floor

`rach2/sys/network/driver/ath9k/recv.c:317` adds `ah->cal.noise_floor`, the raw floor, where `ath9k_process_rssi` adds `ah->noise`, which here is `ah->cal.noise`. Once the 30 s clock gives its first result, every reading falls by 21 to 23 dB. `xmit.c:518` makes the ACK's signal the same way.

`chan_noise` (`calib.c:168`) raises the level when the floor is 6 dB or more above the default, as Linux does, so on a noisy channel the reading moves by that much and no more.

Files: `rach2/sys/network/driver/ath9k/recv.c`, `xmit.c`, its Readme (line 79), `tests/host/ath9k_*`.

Subtasks:

* Use the noise level for received frames and for the ACK's signal.
* A host test against the chip model, for received frames and ACKs: with a quiet floor the signal is the same before and after the first noise-floor result.
* Keep the driver's Readme true.

Done: the host test passes, alone; the reading no longer depends on `cal.noise_floor`.

### 2. Patience: wait about 1.7 s for a silent AP

`rach2/sys/network/wireless/mlme.c` leaves after `WLAN_MLME_BEACON_LOSS`, 7 beacon intervals, 717 ms, plus two null frames in about 20 ms. The value becomes 17, which is 1740 ms at 100 TU by `beacons_ms`'s integer arithmetic (7 gives 716 ms), more than Linux waits.

A temporary log line at each beacon loss shows why the AP went silent: the last beacon's age, the retries and the signal at least. `m->last_beacon` cannot give the age, since `monitor_answered` (`mlme.c:789`) overwrites it after an answered poll, ACK included, so the line needs a time of its own that only a heard beacon sets. A code comment says it is temporary, and #2407 is the card for its removal.

Files: `rach2/sys/network/wireless/mlme.c`, `mlme.h`, `sta.c`, `Readme.md`, `tests/host/Readme.md`, `tests/host/wlan_mlme_hosttest.c` (880 to 904, and the channel-switch rows at 1116 and 1156), `wlan_join_hosttest.c:905`. `ath9k_trial_hosttest.c:1653` stands: its 5 s outage still ends in a leave, 1740 + 2 x 500 < 5000.

Subtasks:

* The new timing, and a host test that fails at the old one: no poll at 1739 ms at 100 TU, and one at 1740 ms.
* The temporary log line, with a host test of its text.
* Keep the reason codes and the Readme true: a leave after the polls stays local reason 4, and the Readme's "Seven beacon intervals" changes.

Done: the host tests pass, alone; the line shows enough to judge the 28 drops' pattern.

### 3. Radio switch: close the interface, as Linux does

After `rfkill block wifi`, `wlan_rfkill_apply` (`rach2/sys/network/wireless/wiphy.c:222`) stops the radio and leaves the interface up, so wpa_supplicant scans once a second and fails with -100: 1964 lines in 32 minutes, which overflowed the service log. Linux's `cfg80211_rfkill_set_block` closes the interface. wpa_supplicant's nl80211 driver then sees the interface go down over rtnetlink and reports it disabled, and on an rfkill unblock event it sets the interface up itself (`wpa_driver_nl80211_rfkill_unblocked`), so that it rejoins. Linux also refuses to bring a blocked interface up with `-ERFKILL`, where Rach2 lets it come up and start at the release.

The review settled the points of wpa_supplicant 2.12: `rtm_newlink` turns the fall of IFF_UP into `EVENT_INTERFACE_DISABLED` (`driver_nl80211.c:1548`), and `wpa_driver_nl80211_rfkill_unblocked` (`driver_nl80211.c:2363`) sets the interface up once and gives up on a refusal. Two traps follow.

* `wlan_rfkill_apply` runs on the worker, and `netconf_set_up(nd, 0)` calls `wireless_set_up`, which queues on the worker and waits (`wiphy.c:200`): a deadlock. The close must tear down inline and clear the interface's up state without that wait. `tests/host/wlan_join_hosttest.c:188` runs queued work inside its wait and would hide the deadlock, so the test must forbid running the worker again from inside it.
* `set_soft` (`rfkill.c:183`) queues the worker and then sends `CHANGE`, so wpa_supplicant may set the interface up before the worker has cleared `wlan->blocked`. A refusal of `-ERFKILL`, if the task adds one, must judge by `soft_blocked || hw_blocked`, never by `wlan->blocked`, or the rejoin is lost for good. Linux refuses; Rach2 now lets the interface up and starts at the release. Keep that if wpa_supplicant's start and the unblock both work with it, and say which in the Readme.

Files: `rach2/sys/network/wireless/wiphy.c`, `Readme.md` (the rfkill section and the Limit at line 226), `man/man4/wlan.4`, `man/man4/rfkill.4`, `man/man8/rfkill.8`, `tests/host/Readme.md`, `tests/host/wlan_dev_hosttest.c`, `wlan_join_hosttest.c`, `airplane_hosttest.c`, `tests/cases/machine/` and `machine.wifi.json` for the wifi suite's rfkill case.

Subtasks:

* Close the interface on a block, on the worker, without a deadlock against `wireless_set_up`'s wait, for the soft and the hard block. Refuse to bring a blocked interface up as Linux does, if wpa_supplicant needs that.
* Host tests: a block leaves the interface down with no reentry of the worker, and an interface-up that arrives right after the release's `CHANGE`, before the worker runs, still ends in a running radio.
* The wifi suite's rfkill case proves it against the real wpa_supplicant: a few log lines, no repeating scan, and a rejoin by itself after the unblock.
* Keep the Readme's rfkill section and Limits and the manual pages true.

Done: the host tests pass, alone; the wifi suite's rfkill case passes on one architecture.

### 4. Prove them on the Lenovo (`owner-only`, blocked by 1, 2 and 3)

Silent, volume 1, no taking the screen, nobody at the machine, by counters and logs. Takes the Lenovo's lock and tries a kernel built from the milestone branch on a trial boot, as the `machine` skill says, after the go the skill requires. wlan0 is not joined now: join TDA-NET with `wpa_cli`, and never print the passphrase, which the Lenovo already holds.

Subtasks:

* Signal, stationary: four rejoins, readings over at least a minute after each, across the 30 s noise-floor result. `wifi status`, `iw` and `/proc/net/wireless` agree and show no 21 to 23 dB fall.
* Radio switch: `rfkill block wifi` gives at most a few log lines and no repeating scan, and `rfkill unblock wifi` rejoins by itself.

Done: both results on the issue, and the release back in both slots with the Lenovo's lock removed.

### Card for later: remove the temporary line

#2407, an issue with no milestone and the label `bug`, in Backlog, so that it stays on the board after this milestone closes.

## The end

`release.sh` takes a new option that leaves out the Mac and Windows, for `-p` and for the release, so that the install, the record, moving master, closing the milestone and archiving the cards stay the script's. It is a process fix, on master by fast-forward once it works.

## The reviews

Codex and Fable each reviewed this plan on its own. Neither changed what the three fixes are. Each point below was checked against the code and is folded in above.

* Both: closing the interface from the worker deadlocks, so the close is inline; the unblock's order against `CHANGE` can lose the rejoin; 7 and 17 intervals are 716 and 1740 ms, not 717 and 1741; the "same reading" test holds only on a quiet floor; more tests and documents depend on the old time than the first draft listed.
* Codex: `last_beacon` is not the last beacon's time after an answered poll; the join test's model of the worker can hide the deadlock; ACK signals need a test too.
* Fable: `man/man4/wlan.4` belongs to task 3, not task 2; `release.sh`'s option can override `has`, which it does.
