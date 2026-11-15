# The second discovery run: rules for every step's agent

The owner approved the run's plan, `build/flow/coordinator/graphics-research/run2-plan.md`, as a whole on 2026-11-06, and ordered it started on 2026-11-08 at 20:05 UTC+6 ("go"). Read first, in this order: `run2-machinery.md` (how the run is set up and what each machine has now), `run2-plan.md`, the graphics plan `plan.md` (draft 5; its milestone numbers 73 to 80 are old, so name tasks as the plan does and do not renumber them), and the first run's report `tonight/report.md` with the file of your machine (`tonight/qemu-wsl.md` for the testing PC, `tonight/lenovo.md` for the Lenovo). All are under `build/flow/coordinator/graphics-research/`. `AGENTS.md` holds the project's rules; its Resources, Hardware and Writing sections bind you.

## What to find

Each step measures what its row of the plan's table names, against Linux in the same environment: a Debian 13 reference runs first, and its result is kept as what Rach2 must later match. Say for each result which plan decision, task or risk it changes, and how. A path that fails is a result: record the exact error, the version, and whether the cause is ours, the host's or the program's, traced to the source where it can be.

## How to work

* Write your report as you go to `build/flow/coordinator/graphics-research/run2/stepN.md` on the Mac (N your step, or `lenovo-setup` for the Lenovo's setup), with raw outputs copied into `run2/stepN/`: each check, the exact command, the result, what it means. Plain ASCII, `*` bullets, no em-dashes, short sentences. A later reader must be able to repeat each run from it.
* Read numbers from the run's own output, never from memory. Mark any figure you did not read yourself.
* Wait for a long command in the background with the tool's own notice, never by polling a process table or sleeping. Read a long run's sign of progress on a short clock and stop it when that sign stops.
* Leave no process of yours running at the end, and remove your scratch; keep images, built programs and downloads under `build/cache/run2/` on the machine that made them, and give their paths and sizes in the report.
* Use no subagents.
* When done, reply with the report's path and the five findings that matter most, each with its number and what it changes.

## Machines

* The testing PC: `ssh testing` lands in cmd.exe. Pipe scripts: `ssh testing 'bash -s' < script` for Windows (MSYS bash), and `ssh -o BatchMode=yes testing "MSYS_NO_PATHCONV=1 wsl.exe -u testing -- bash -s" < script` for WSL as `testing` (`-u root` for root), with the output through `tr -d '\000\r'`. An inline command with nested quotes or `$?` fails in cmd.exe. Work in your own directory: `/home/testing/rach2/build/run2/stepN/` in WSL, `D:/rach2/build/run2/stepN/` on Windows. Do not edit or sync the tree of `/home/testing/rach2`, and do not use `tools/dev/testing.sh sync`; bring sources by `git bundle` or tar into your own directory. The Debian 13 image of the first run is `/home/testing/rach2/build/cache/gfxref-20261104/img/debian-13-generic-amd64.qcow2`; make your own overlay on it, and leave the first run's `overlay.qcow2` as it is.
* The Lenovo: Rach2 at 192.168.0.106, `ssh -i krffs/rach2_demo_ed25519 -o UserKnownHostsFile=build/flow/coordinator/graphics-research/run2/lenovo-known -o StrictHostKeyChecking=accept-new root@192.168.0.106`. Reach it only with Apple's `ssh`, `scp` and `nc`: a program built or installed on the Mac and started from this session gets EHOSTUNREACH to the LAN. Under Linux it may answer only on `fe80::xxxx:xxff:fexx:xxxx%en1`.
* The Mac runs no graphics: it has only TCG.

## Limits

* Downloads only from official sources: deb.debian.org, security.debian.org, cloud.debian.org, dl.google.com, download.mozilla.org, the projects' own release pages (GE-Proton's GitHub releases, Mesa's and freedesktop's GitLab, Khronos's GitHub for VK-GL-CTS). Check each against the checksum its source publishes. GitHub sparingly: release files and source tarballs only, no `gh`, no API polling, no pushes.
* On Windows change nothing: no reboot, no feature, driver, Hyper-V, network, Tailscale or `.wslconfig` change. In WSL change nothing system-wide except what your step's brief allows; no `apt install` on the WSL host. Do not touch `D:/WSL`, `D:/hyper-v`, `D:/diadochi`, `C:/temp`, `D:/WSLbackup-20261022head.bin`, other accounts' homes, `/home/testing/bin`, `/home/ghrunner`, the test runner's site, ledger and locks, or the release copy beyond reading its images.
* Never print or copy a credential, key or passphrase. Linux on the Lenovo uses its wired port with DHCP and keeps `ath9k` from loading, as the first run did, so the Wi-Fi passphrase is not needed and stays on Rach2's root.
* The Lenovo: silent, its sound driver kept from loading and nothing played aloud; its screen dimmed unless your brief says a step takes it. Writes to any of its disks are authorized; the Mac's internal SSD is never written.
* No suites, tests or releases of Rach2 besides what your step names. No commit, no push, no board or GitHub change.
