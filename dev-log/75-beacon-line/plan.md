# Milestone 75: beacon-loss line

## Goal

#2407: remove the temporary beacon-loss log line that milestone 74 added to judge the pattern of the Wi-Fi drops, with its call in the MLME's operations, the field only it used, its host test rows, the wireless Readme's mention and the soak's count of it. The beacon-loss monitor and the station dump's count of beacon losses stay.

## Done

The change builds on both architectures, Codex finds nothing in it, the Wi-Fi host tests it touches pass on the Mac, and the Lenovo boots it on a trial, passes metal-accept and renews its key five times on Wi-Fi without a drop. The owner waived the full release for this change.

## The chat's failure on #2407

The owner had ordered everything finished. The chat left #2407 open to the end and then swung three times between removing the line and keeping it. Times are UTC, from the chat's session.

* 08:43: the owner ordered all the work finished, on master and off the board.
* 08:47: the chat reported the work finished apart from one old card, #2407.
* 08:49: the chat called #2407 an old note that could stay in the backlog, outside any milestone.
* 08:50 and 08:52: the chat ordered the manager to remove the line in milestone 75 and take it through a full release preparation and release.
* 08:54: the chat stopped the manager before anything ran, to spare the testing host another run of some three hours.
* 08:55: the chat cancelled the removal. The manager saved it as a patch, removed its worktree and branches and closed milestone 75, and the chat committed b7092d27c, which kept the line and reworded its comment from temporary to diagnostic, and closed #2407. That put master one commit ahead of the Lenovo's installed system without saying so.
* 09:24: the line was still in the kernel, 37 minutes after the chat first raised it.
* 09:26: the owner ordered it removed, reviewed, built, tested on the Lenovo and released, which this milestone does.
