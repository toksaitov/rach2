# Milestone 75: beacon-loss line

## Goal

#2407: remove the temporary beacon-loss log line that milestone 74 added to judge the pattern of the Wi-Fi drops, with its call in the MLME's operations, the field only it used, its host test rows, the wireless Readme's mention and the soak's count of it. The beacon-loss monitor and the station dump's count of beacon losses stay.

## Done

The change builds on both architectures, Codex finds nothing in it, the Wi-Fi host tests it touches pass on the Mac, and the Lenovo boots it on a trial, passes metal-accept and renews its key five times on Wi-Fi without a drop. The owner waived the full release for this change.
