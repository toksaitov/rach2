# Milestone 54: GNU libc and the GNU system

Milestone 54 is accepted on 2026-09-30 at the commit that adds this record, from source `18189958`. Rach2 runs GNU libc 2.44 and a GNU userland beside its musl system on its ext2 root: bash, coreutils and the GNU tools configure scripts expect, GNU make, Vim, Perl, Git, curl, OpenSSH in Dropbear's place, GCC 16.2.0 with binutils 2.47, and GDB 18.1. The SpiderMonkey shell runs a script in its JIT, and Debian's i386 `chromium-headless-shell` loads a page. The plan is [`plan.md`](plan.md), the record of the work is [`notes.md`](notes.md), and the faults the work found are entries 227 to 278 of the [shortcomings list](../50-shortcomings/plan.md).

## What was built

The kernel gained what glibc and its programs ask of Linux: `mprotect`, `MAP_FIXED` and the reuse of freed space; `clone` as glibc's fork and threads use it, `futex`, robust lists and `set_tid_address`; signals in Linux's i386 frames with `siginfo`, `SA_RESTART` and processor faults; position-independent images with the interpreter they name; the `*at` calls and paths of 4096 bytes; `eventfd`, `epoll`, `timerfd`, `memfd_create` with seals, a tmpfs for `/dev/shm`, shared file mappings, `madvise` and `mremap`; `ptrace`, core files, capabilities and the `/proc` files a browser reads. The GNU layer under `vendor/gnu/` builds an i686-linux-gnu cross toolchain from pinned tarballs, the ports with it, the native compiler, GDB, the SpiderMonkey shell and Debian's browser packages, and `make os-real-gnu-ext2` builds the GNU system's root.

## What proves it

* The gnu suite, `tests/run gnu`, boots the GNU root and drives it over SSH with nobody at the console: logins by key and by password, scp and sftp, and 27 rows, each a port doing its main job.
* The last `tests/run all`, on source `18189958` at the testing host, ran 91 suites and passed all 91, and `tests/system/rehearse_group_mutate.sh` passed after it. The set before it, on `0d38791e`, had failed only `style`, on one comment line of 97 columns, which `fde76235` wraps.
* The proofs of the leftovers: 319 guest faults, each failing its row; 58 host and 29 system mutation scripts; the raw and mount suites' faults.
* The reviews: Codex over the whole batch and over every round of fixes until one found nothing; `notes.md` lists what each found.
* The machine: on 2026-09-30 the GNU system was installed on the Lenovo and its 27 rows passed there.

## The record

* Source: `18189958`. Build options: the kernel with `i686-elf-gcc -m32 -ansi -Werror`, freestanding and without libgcc; the GNU layer with the cross toolchain of `make gnu-toolchain`.
* The machine's kernel: `build/rach2/kernel` of the Mac's build, 1238584 bytes, sha256 `7cd77e4b73306c251516855576e710bf940d23a87c98cd6a4f9888aac20f45da`, in the candidate slot and committed.
* The machine's root: `os-real-gnu-2g.ext2`, the GNU root in 2048 MiB, sha256 `f78fa2d273cb65841974c8ae8232d5909e03b51ba5d5dbce7e654c12f5b3fc45`, written by `rach2-install --update` and grown to 232.8 GiB. The suite's root, `os-real-gnu.ext2` of 4096 MiB, is sha256 `aab5eeed923ed7b3d621c04216c614a28f2d8b9c6e9a01f43690016f9529cd59`.
* The machine's recovery partition: sha256 `aed1b4b2b2f52811bddcf61bbd350d4848b3f69247558a8104b697ec212da3a3`, the sealed disk's. The stick: `rach2-install-stick-gnu-with-key.img`, 3633315840 bytes, sha256 `e065261781b23cdcd73f52ad843c0ff48e753e87e35b039c578d0b8df9b72b8c`.
* Hardware: the Lenovo G50-70 at 192.168.0.106, its Samsung SSD 870 EVO of 488397168 sectors, serial `S6P2NX0T381638Z`, and the SanDisk 3.2Gen1 stick, serial `00014101091624031318`, plugged into it.
* The testing host's judges: i686 GCC 15.2.0, QEMU 10.2.1 run with TCG, Cppcheck 2.19.0 and clang-format 21.1.8, on 32 processors. The Mac's cross compiler, which built the machine's kernel, is i686-elf-gcc 16.2.0.

## What is still open

Entry 261 keeps three features no suite's row needs, the write-back of shared writable file mappings, the inotify calls and a netlink route socket, for the owner to schedule; Chromium runs without them. Entry 262 keeps the cause of a stress-ahci failure, likely entry 277's, which the logs a failure now keeps will confirm.
