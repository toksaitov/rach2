# 73 wifi steady

The goal as a user sees it: the Lenovo's Wi-Fi signal reads true at all times, a short silence from the AP no longer ends the connection, and the radio switch leaves the system quiet and rejoins by itself. It is also a test of the factory: the accounts' loops and the manager's take the tasks from the board, and the milestone runs to the Lenovo through the usual pipeline.

Evidence: `build/flow/coordinator/lenovo-hotspot-20261104/findings.md`.

## The owner's decisions

* The three fixes below, ordered on 2026-11-05 as the approval of this scope.
* No release on the Mac or on Windows, this once. The end's checks run only in WSL, then one full run of every suite on both architectures in WSL, under TCG. Its wall time and failures go to the owner, with the last release's WSL run beside them, to decide whether TCG stays the default there.
* If that run passes, the release installs on a trial boot, runs the native cases and the acceptance, commits, moves master, closes the milestone and clears the board. If the code proves worthless, the installed image stays.
* Left out by the owner's choice: the 11 s `wpa_cli reassociate` and the small `iw` gaps.

## Tasks

The tasks touch different code. Tasks 2 and 3 both change `rach2/sys/network/wireless/Readme.md` and may touch `tests/host/wlan_join_hosttest.c`, in different paragraphs and rows, so a rebase settles them.

### 1. Signal: read the noise level, not the raw floor

`rach2/sys/network/driver/ath9k/recv.c:317` adds `ah->cal.noise_floor`, the raw floor, where `ath9k_process_rssi` adds `ah->noise`, which here is `ah->cal.noise`. Once the 30 s clock gives its first result, every reading falls by 21 to 23 dB. `xmit.c:518` makes the ACK's signal the same way.

Files: `rach2/sys/network/driver/ath9k/recv.c`, `xmit.c`, its Readme, `tests/host/ath9k_*`.

Subtasks:

* Use the noise level for received frames and for the ACK's signal.
* A host test against the chip model: a frame's signal is the same before and after the first noise-floor result.
* Keep the driver's Readme true.

Done: the host test passes, alone; the reading no longer depends on `cal.noise_floor`.

### 2. Patience: wait about 1.7 s for a silent AP

`rach2/sys/network/wireless/mlme.c` leaves after `WLAN_MLME_BEACON_LOSS`, 7 beacon intervals, 717 ms, plus two null frames in about 20 ms. The value becomes 17, which is 1741 ms at 100 TU, more than Linux waits.

A temporary log line at each beacon loss shows why the AP went silent: the last beacon's age, the retries and the signal at least. A code comment says it is temporary, and `#TASK-CARD` is the card for its removal.

Files: `rach2/sys/network/wireless/mlme.c`, `mlme.h`, `sta.c`, `Readme.md`, `man/man4/wlan.4`, `tests/host/wlan_mlme_hosttest.c`, `wlan_join_hosttest.c` if its row needs it.

Subtasks:

* The new timing, and a host test that fails at the old one: no poll before 1741 ms at 100 TU, and one after it.
* The temporary log line, with a host test of its text.
* Keep the reason codes and the Readme true: a leave after the polls stays local reason 4, and the Readme's "Seven beacon intervals" changes.

Done: the host tests pass, alone; the line shows enough to judge the 28 drops' pattern.

### 3. Radio switch: close the interface, as Linux does

After `rfkill block wifi`, `wlan_rfkill_apply` (`rach2/sys/network/wireless/wiphy.c:222`) stops the radio and leaves the interface up, so wpa_supplicant scans once a second and fails with -100: 1964 lines in 32 minutes, which overflowed the service log. Linux's `cfg80211_rfkill_set_block` closes the interface. wpa_supplicant's nl80211 driver then sees the interface go down over rtnetlink and reports it disabled, and on an rfkill unblock event it sets the interface up itself (`wpa_driver_nl80211_rfkill_unblocked`), so that it rejoins. Linux also refuses to bring a blocked interface up with `-ERFKILL`, where Rach2 lets it come up and start at the release.

Check this against the vendored wpa_supplicant's source first, and do what it needs.

Files: `rach2/sys/network/wireless/wiphy.c`, `Readme.md`, `man/man4/rfkill.4`, `man/man8/rfkill.8`, `tests/host/wlan_dev_hosttest.c`, `wlan_join_hosttest.c`, `airplane_hosttest.c`, `tests/cases/machine/` for the wifi suite's rfkill case.

Subtasks:

* Close the interface on a block, on the worker, without a deadlock against `wireless_set_up`'s wait, for the soft and the hard block. Refuse to bring a blocked interface up as Linux does, if wpa_supplicant needs that.
* Host tests: a block leaves the interface down, a release lets wpa_supplicant's way of bringing it up work.
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

An issue with no milestone and the label `bug`, in Backlog, so that it stays on the board after this milestone closes. It is opened with the milestone; its number stands in for `#TASK-CARD` above.

## The end

`release.sh` takes a new option that leaves out the Mac and Windows, for `-p` and for the release, so that the install, the record, moving master, closing the milestone and archiving the cards stay the script's. It is a process fix, on master by fast-forward once it works.
