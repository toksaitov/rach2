# Upstream faults of milestone 74

Faults this milestone found in another project's code, one section each, in the format of [`dev-log/upstream.md`](../upstream.md), which indexes them. The access point is the TP-Link of the soak on the Lenovo; its model and firmware version are missing from the records.

## wpa_supplicant arms a pairwise rekey only after a completed one

* Project: wpa_supplicant, as Alpine Linux ships it on the Lenovo, whose version is missing; the code is read in Rach2's wpa_supplicant 2.12, `vendor/hostap/wpa_supplicant-2.12.tar.gz`.
* Fails: with `wpa_ptk_rekey` set, a renewal request that fails is not retried and no later renewal is asked for while the link stays up: `wpa_sm_rekey_ptk` sends one request (`src/rsn_supp/wpa.c:1233-1238`), and the timer is armed again only when a new pairwise key is installed (`src/rsn_supp/wpa.c:1337-1341`).
* Found: by 2026-11-08, in milestone 74, under Linux on the Lenovo: one request sent during Linux's 20/40 MHz coexistence scan failed, and renewals ended for that connection with the link up.
* Reproduction: set `wpa_ptk_rekey` to a few seconds, make one EAPOL-Key request fail, for example during an associated scan, and watch for the next renewal.
* Evidence: `dev-log/74-wifi-soak/plan.md`, Results so far; the soak's logs are not named in the record.
* Rach2: nothing in wpa_supplicant, which Rach2 runs unmodified too. The Linux trial then ran at a rekey of 110 s, whose first line-up with the 300 s scan is at 3300 s.
* Status: unreported.
* Next: name the Alpine package's version, keep the log of a failed request, check hostap's current `wpa.c`, and decide whether a retry belongs on the failed send or on a timer.

## The access point sends its queue under a new key with the old key's numbers

* Project: the TP-Link access point of the soak; model and firmware are missing.
* Fails: after a pairwise key renewal the access point sends the frames it had queued under the new key with the old key's packet numbers, then starts again from 1, so a frame far below the old count passes a station's replay test and every true frame after it reads as a replay. Under a 70 Mbit/s UDP overload one such frame moved the receive counter to 117454 within 92 ms, and 52422 true frames were replays until the station left.
* Found: by 2026-11-08, in milestone 74, on the Lenovo under Rach2, as silences of 10 s and more after a renewal under download.
* Reproduction: renew the pairwise key every 20 s while the access point sends a download or a UDP overload, and log each frame's packet number against the key that opened it.
* Evidence: `dev-log/74-wifi-soak/plan.md`, Results so far; `rach2/sys/network/wireless/data.h:52-60`; commits `dfbe279e7`, `b1088fc6b`, `1fffc0dc8`. The trace's files are not named in the record.
* Rach2: within 2 s of a key, a frame is dropped when its number is more than 64 past the highest the new key has seen and not 8192 or more below the old count (`WLAN_DATA_REKEY_SETTLE`, `WLAN_DATA_REKEY_JUMP`, `WLAN_DATA_REKEY_BACK`). On 22 keys under the same load there were 0 silences.
* Status: unreported.
* Next: name the model and firmware, capture the frames over the air with a monitor-mode radio, and look for a newer firmware.

## The access point forgets a station without a deauthentication

* Project: the TP-Link access point of the soak; model and firmware are missing.
* Fails: when a station's reply to a renewal is late, the access point sends message 1 or 3 four times, 1 s apart, then forgets the station without a deauthentication: its radio goes on ACKing, null frames included, and its beacons go on. It ACKed a null frame every 5 s for 150 s and sent no deauthentication.
* Found: by 2026-11-08, in milestone 74, on the Lenovo under Rach2, after a 6.5 s stall at message 1.
* Reproduction: hold the station's reply to message 1 for more than 4 s and watch for a deauthentication.
* Evidence: `dev-log/74-wifi-soak/plan.md`, the stall's findings at the top; `rach2/sys/network/wireless/Readme.md`, the renewal's limit.
* Rach2: when a renewal's message 1 reaches a connection with a pairwise key and no pairwise key goes in for 10 s, the station leaves with reason 15 (`WLAN_MLME_KEY_WAIT`); commit `361e634d2`.
* Status: unreported.
* Next: name the model and firmware and capture the frames over the air.

## Linux 6.18.58 goes silent at key renewals on the Lenovo

* Project: Linux 6.18.58-0-lts, Alpine's kernel, with ath9k on the Lenovo's QCA9565 (`dev-log/70-wifi-chip/lenovo.md`).
* Fails: on the same machine and access point under Rach2's disk load, Linux went silent at renewals in every loaded stage: at rekey 20 under download, 7 silences of 20 to 21 s in 15 keys, each until the next key; at rekey 120, silences of 53.2 to 120.8 s, some ended by a drop; under the UDP load one gap of 262 s. The cause is unproved.
* Found: 2026-11-08, in milestone 74, on the Lenovo.
* Reproduction: the soak's load and rekey intervals under Linux on the same machine and access point.
* Evidence: `dev-log/74-wifi-soak/plan.md`, the last item of Results so far; commit `8f83496ea`.
* Rach2: nothing; Rach2's station holds under the same load with the replay rule above.
* Status: unreported, and not a fault of Linux until its cause is traced. Alpine's kernel has no mac80211 debugfs, so its replay counters could not be read; if they show the access point's old numbers above, the fault is the access point's.
* Next: rerun on a kernel with mac80211 debugfs and read the replay counters at a silence before anything is reported.
