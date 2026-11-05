# 73 wifi steady

Three small Wi-Fi fixes, run through the factory as a test of it. The GitHub milestone exists because `release.sh` needs one.

* Signal: `recv.c` and `xmit.c` add the noise level, not the raw floor, so a reading does not fall 21 to 23 dB at the first noise-floor result (#2408).
* Patience: wait 17 beacon intervals, 1740 ms, for a silent AP, with a temporary log line at each loss, which #2407 removes (#2412).
* Radio switch: an rfkill block closes the interface as Linux does, and an unblock rejoins by itself (#2416).
* The Lenovo proves the signal and the switch (#2421).

Skipped by the owner, this once: the release on the Mac and on Windows. The full run is WSL only, under TCG (`release.sh -o`). Left out by the owner: the 11 s `wpa_cli reassociate` and the small `iw` gaps.

Evidence: `build/flow/coordinator/lenovo-hotspot-20261104/findings.md`.
