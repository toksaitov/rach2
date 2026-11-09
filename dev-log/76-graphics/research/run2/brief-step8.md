# Step 8: today's Rach2 on the GNU root, with Debian's graphics userland

Row 8 of `run2-plan.md`: Rach2 in QEMU on the testing PC, booted on the GNU root, running Debian's lavapipe, Weston headless and Wine, with `rach2.trace=enosys` on its command line. It replaces the plan's line estimates of milestones 73, 76 and 77 with what today's kernel actually lacks.

* The images: the release copy's x86_64 build, `/home/testing/rach2/build/worktrees/release/build/x86_64/` (the kernel and `images/os-real-gnu.ext2`), read only: copy what you boot into your own directory with its SHA-256. Read `vendor/gnu/Readme.md` and the GNU root's test cases under `tests/cases/` for how the root boots and how files reach it. Boot under KVM in WSL, or TCG if KVM breaks it, and say which.
* Bring Debian 13's amd64 packages for Mesa 25.0.7 (llvmpipe, lavapipe and their loaders), Weston 14 headless, Wine, and their dependencies onto a copy of the root, from deb.debian.org with the repository's checksums, unpacked with `dpkg-deb -x` on the host, not installed by Rach2.
* Run, under the trace: `eglinfo`, `vulkaninfo`, a GL and a Vulkan program offscreen, Weston headless with a client, `wine --version`, then a WineD3D program. For each: what ran, the first failure and the system calls, ioctls, files under `/dev`, `/proc` and `/sys`, and flags the trace names as missing, counted. Group them by kernel subsystem.
* Close with the list of missing kernel pieces each program needs, by plan task, and how that changes the size of milestones 73, 76 and 77.
* Step 2 runs at the same time in the same PC's WSL; your work is functional, not timed, so share the PC and keep to 8 threads.
* Step 1 (`run2/step1.md`) found the release copy's `os-simple.krffs` swept and rebuilt it from the same commit in its own directory; check the files you need exist before you start, and rebuild what is missing the same way, in your own directory. Its Rach2 boot method under KVM in WSL is in that report.
