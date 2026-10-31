# Incidents of milestone 64's release, 2026-10-31

The faults that the process showed in milestone 64's release, each with its cause, what it cost and its fix. Times are UTC+6, the owner's local time.

## The style checks

* The end-of-milestone run checked style on the testing host alone, where it passed. Late fixes followed it, and the release's preparation runs the style suite again on the Mac and on Windows, each in its own way. Nobody ran that preparation on those machines before the release.
* 00:00: the release started at 496d0efe0. GitHub reported an incident with Actions at 00:06; the testing host's runners got no answer from GitHub's job service, the job was never taken, and at 00:11 its run was recorded as failed with `runner`. The Mac and Windows failed in their preparation on style at 00:28: the Mac because the clang case found no ACPICA headers, Windows on a comment over 80 columns. A style run on the testing host before the next start found a third fault, a function over its size limit.
* 01:09: the release started again from scratch at 3ca3bd74a, which wrapped the comment, split the function and unpacked ACPICA through a Makefile rule. The last fix was not tried the way the release runs the checks. At 01:13 the Mac failed again on the same headers: the release's preparation runs the style cases without that rule.
* 01:13 to 02:12: nobody saw the Mac's failure for an hour. release.sh reads the Mac run's result only in its last wait, after the testing host's run and Windows', so its output kept showing the GitHub run in progress. The manager's checks at 01:42 and 02:12 read that output; the second also read the Mac run's console, found it unchanged since 01:13, and so found the failure.
* 02:21: the release started again from scratch at 26b625f0c, whose style cases unpack ACPICA themselves. The testing host's run, 68 minutes in and untouched by the fault, was cancelled. This fix was not tried the way the release runs the checks either: on the Mac, the clang case runs in the tooling machine, an Ubuntu guest, on a copy of the tree that cannot unpack ACPICA.
* 02:32: the release started again from scratch at df536e17d, which sends ACPICA's headers into the tooling machine. The testing host's run was cancelled 11 minutes in. This fix was reproduced in the tooling machine before it was made, and the Mac's preparation, style checks included, passed by 02:42 and was whole at about 02:59.
* Cost: 2 h 32 min from the first start to the start that holds, and the testing host's run thrown away twice. The milestone skill already said to fix a release's failures and run it again with `release.sh -r`, which keeps every machine's passes and runs again only what failed; `-r` takes a new commit too. The chat relayed each restart after checking only that it ran, not whether a restart was needed.

## The release's parent

* 02:43 to 02:53: the owner asked why Windows waits for the WSL run on the same host. The chat measured the host nearly idle and ordered both at once. The manager named two clashes: Windows' guests may take 120 GiB beside WSL's 128 GiB on a 191 GiB host, and both sides take port blocks from the lowest free one, while WSL's localhost forwarding holds its ports on Windows. The chat then kept the order serial, then ordered it parallel again at the owner's protest, then kept it serial. The serial order, with the whole host for each run, is right while the port clash is near certain.
* While preparing the parallel run, the manager stopped the release's parent. The Mac run, started by the parent, had inherited the descriptor of the release lock, so no new parent could start until the Mac run ended, near 05:25. The manager set a start that waited for the lock, which would have left Windows idle for up to an hour and a half after the WSL run. The first copy of release.sh, made for the parallel run, had stopped at 02:50 on the same lock without starting anything. At 02:53 a copy of release.sh without the lock check took over, resumed from the record, and found the GitHub run and the Mac run as they were. No run was lost.

## Idle machines

* At 02:58 the testing host's run was 23 minutes in and still building, one compiler at a time, at a load of about 2 on 32 threads, with no guest running. The Mac was about 80% idle on its 10 cores, with one etude process on one core and no guest running, after its suites were reported started.
* 03:05: the testing host's run, 33 minutes in, still had no guest running. Its make had nothing left to do; the one busy process, `etude run` at 20% of one core, was reading `build/x86_64/images/cases/rehearse/save.img`, 2.2 GiB and mostly zeros, in 1 MiB reads for its preparation record, at 0.9 GiB of it. The rehearse-lenovo image, 3.4 GiB, is hashed the same way. The preparation hashes its inputs one file at a time on one core while 31 threads wait.
* The Mac's run prepares the in-house tree from clean, over 20 minutes each time, and repeated it at every restart; the vendor and GNU builds come from its kept copy.

## A test that timed the clock

* 03:12: the Mac's run failed one check, `etude_remote_hosttest`'s deadline row. The row gave its program 300 ms before stopping it and required the program to have written one line by then. Under the release's load the Mac does not always run the program within 300 ms: the manager reproduced the failure in 4 of 10 and 1 of 25 runs of the release's own binary, with the run stopped by its deadline as intended and nothing written. The check judged the clock, which no suite may do.
* 03:21: 36b8c67f2 requires the row's run to end stopped and to have run at most once; 30 of 30 runs passed at a load of 10. The Mac's run goes on; when the release reads its result, `release.sh -r` at 36b8c67f2 reruns only what failed.

## The testing host's run

* 04:18: the testing host's run failed five suites: doom and uefi on both architectures, and procfiles on x86-64.
* doom and uefi: the GitHub runners' copies of the tree had no Doom WAD, which is not in Git and which nothing in the release puts there, so the images were built without /demo/doom1.wad. At 04:43 the manager copied the shareware WAD, checked by its SHA-256, from the testing account's clone into both runner copies, whose checkout keeps untracked files. The Windows and Mac copies have it.
* procfiles: 896c46f86, in this milestone, made os-real 22 MiB on x86-64, and the procfiles probe was still told 20480 KiB. 85f7f596e gives it the size of its architecture's os-real. Its first run is the release's rerun.
* 05:11: the Mac's run ended with the two failures already fixed, procfiles on x86-64 and the deadline row of etude_remote_hosttest.
* 05:15: Windows' run, 56 minutes in, had run no case. Its preparation was rebuilding the GNU layers from source in its tooling machine: fstools, sanitizers, chromium, gdb, js, native, ports and host-products, each "built here", for i686 with x86-64 still to come. Its copy holds no store entry at this milestone's keys, and the run at 496d0efe0 had rebuilt the GNU toolchain the same way before it stopped on style. Windows' D: drive fell to 302 GiB free, 19%.
* The rerun with `release.sh -r` at 85f7f596e follows once Windows and the Mac have their results; it runs only the failed suites.

## Windows' run

* 07:39: Windows' run ended after 1057 cases with four suites failed: procfiles on x86-64 and the deadline row in host on i686, both fixed already, and two of its own.
* acpi on both architectures: QEMU refused `-acpitable file=D:/rach2/...` with "Could not open 'D'". QEMU splits that value at each colon, so a Windows path breaks at its drive letter. etude now gives the path without its drive, which is the drive of QEMU's working directory, and refuses a path with any other colon.
* host on i686: `etude_qemu_hosttest`'s real-poweroff row lost the end of its console, the `poweroff` lines among it, though the guest shut down. On Windows the launcher asked QEMU to quit as soon as QMP's SHUTDOWN arrived, while the console's last bytes were still on their way on the other connection, and a Windows process that exits resets its connections. The launcher now reads the console for the grace period after the event and then asks QEMU to quit. The x86-64 run of the same test passed.

## The rerun and the Lenovo

* 07:58: `release.sh -r` at 094b9c395 ran again only the failures, but it dropped Windows' four suites: the record's Windows failure line ended in two carriage returns from Windows' ssh output, so its last word failed the registry check, and with no line for Windows the release would have run every suite there again. At 08:10 the manager appended the right `again windows` line to the record before Windows' step read it.
* 08:19 to 08:45: the testing host passed its five suites, Windows its four (rebuilding only the GNU layers host-products and host-checks, whose inputs the fixes touched) and the Mac its two. The Lenovo's install and trial boot passed at 09:01.
* 09:01 to 09:19: the Lenovo's native run, a step release.sh gained on 29 October and that no release had reached, failed to start three times, each refused before any case ran. release.sh fetches `prepared.tar.gz` and unpacks it, but `native-deploy` needs what `make native-assets` publishes: the archive's digest file beside it, the `archive_sha256` that the preparation adds to `prepared.json` after making the archive, and the archive staged on the machine by `native-deploy stage`, which release.sh never runs. The manager wrote the digest file and the field from the verified digest, staged the archive, removed the three failed starts from the record and resumed the release at 09:18; the native cases then ran.
* 09:34: the native run ended with 248 of 250 cases passed. gnu/coreutils found no clock reading in the kernel's log: the ring of 16 KiB held only the last 213 records, `vfs: unmounted` lines of the native cases' private roots, and the boot records were gone. Those lines are a contract that the mount scenario reads, so 25f347548 changes the row instead: on a native machine whose log no longer begins at boot, it skips the comparison with the boot clock. The fixed row passed on the Lenovo. gnu/chromium complained 4 times of D-Bus because the test account's home, /var/native/test, did not exist. The manager had staged the payload at 09:18 with the main checkout's native-deploy, built on 25 October, which predates the code of 28 October that makes a missing home; the release builds its own in its worktree, but its run does not stage. With the home made, chromium ran on the Lenovo without complaints.
* 09:50: `release.sh -r` at 25f347548 keeps every pass of the other machines and runs again only the two cases, after the build, install and trial boot that a new commit needs. Its run at 10:02 was refused like those at 09:01, and the manager staged by hand again, with the same old binary, so the home was again missing; it was made by hand at 10:04:44, two seconds after chromium started, and coreutils passed. The release was resumed for chromium.
* 10:05: both native cases passed; 10:06: the acceptance passed on the trial boot, the trial was committed and copied to the fallback slot, master moved to 25f347548 and the milestone closed.

## Disk

* The Mac's project build holds 178 GiB: 123 GiB in the release copy, 26 GiB in a second copy of the x86-64 vendor builds in the main checkout, and 11 GiB of scratch from 23 to 25 October in build/tests/portable-batch. The status check calls the Mac's disk low below 20% free and critical below 40 GiB; the owner rules that the Mac works down to 5 GiB.

## Fixes

Made during the release:

* The manager reruns a failed release with `-r` and cancels no run on a machine the fault does not touch. Before a release it runs each machine's preparation and style suite the way the release runs them, and it reproduces a failure before it fixes it.
* The chat checks that each step it relays is the quickest sound one, and stops it when it is not.
* At each check during a release, the manager reads every machine's run, not only release.sh's output, which shows the Mac's result only at the end.
* No action and no warning about the Mac's disk above 5 GiB.

After the release, in one process fix to master:

* The style suite runs once per release, in the full run; the Mac and Windows select every other suite, or the failures of the last record but style.
* The Mac run closes the release lock's descriptor, so a stopped parent can start again.
* The Mac's disk is critical only below 5 GiB in tools/flow/status.sh and AGENTS.md, and never low.
* The 26 GiB second copy and the 11 GiB of old scratch are removed.
* While the full run goes on, release.sh says at once that the Mac's run failed, before the wait that records it. The full run and the Mac's wait poll every minute instead of every five.
* release.sh strips the carriage returns of Windows' output before it reads Windows' status and failures.
* The build fetches the native payload's digest file and published `prepared.json`, and the Lenovo's step stages the payload, with the native-deploy it builds, when the machine does not have it.
* The install's wait no longer counts the installer's absent status file as a machine that did not answer.
* The full run takes 20 workers and 64 GiB, and copies the shareware WAD from WSL's `/etc/rach2/doom1.wad` into a runner's copy that has none.

Left for the next milestone's plan, since each is a change to etude: the preparation's hashing on one core and its byte copies of the disk inputs, the serial host checks, the GNU build guest's threads, Windows rebuilding what WSL built, Windows preparing before it takes the whole host, quitting QEMU once the console is quiet, one clean build per release, and the end run's repeat of what the release runs.
