# Milestone 20: a compiler on the machine

Before this milestone, every Rach2 program was built on another machine. A student edited a program on a Mac, rebuilt an image, and booted it. The milestone adds native compilation: write C at the Rach2 prompt, compile and run it there, then compile one of the project's tools from source carried in the image.

The compiler is tinycc 0.9.27, vendored and cross-built like musl, busybox, and dropbear. Producing an executable requires a compiler, assembler, and linker. GCC and Clang provide only the compiler and pass their output to GNU `as` and `ld`; either port would also require binutils and then `make`, which the image lacks. The small teaching compilers have the same dependency. Tcc provides all three stages in one binary.

The scope is static output only, with no `-run`. The largest native build is one of the system's utilities. The guest does not compile the kernel, C library, or SSH server; the plan records the reason for each limit beside its phase.

## What a student does with it

```
$ ssh -i krffs/rach2_demo_ed25519 -p 2222 root@localhost
# printf '#include <stdio.h>\nint main(void){puts("hello");return 0;}\n' > /tmp/m.c
# tcc -static -o /tmp/m /tmp/m.c
# /tmp/m
hello
# tcc -static -o /tmp/cat /src/utils/cat.c
```

An ordinary user can read the sysroot and use the compiler. `make tcc-test` runs the complete workflow above as the unprivileged `test` account.

## Where the plan was wrong

The implementation disproved four claims in the plan, which has been corrected in place.

The draft removed `--config-musl` because its name suggested that it only set a dynamic-loader path, which this static system does not need. The build showed another effect: the flag also removes `bcheck.o` from the runtime (`lib/Makefile:31-33`). The `bcheck.c` implementation uses glibc malloc hooks absent from musl, so `libtcc1.a` does not build without the flag. As a result, `tcc -b` bounds checking is unavailable.

The first ELF checker used the wrong address window. `mm/usercopy.h` permits user addresses through `0xC0000000`, while `elf_load_image` stops at `0xBFFFF000`, one page earlier. Using the wider limit allowed images that `exec` rejects. The original boundary cases derived their addresses from the constant under test, so the same mistake moved the cases and kept them passing. They now use literal addresses.

The proposed syscall check subtracted the calls of a known working program from those of the compiler run. A trace that prints each syscall number once per boot cannot support that comparison: a number reported by an earlier process is suppressed for the compiler, producing an empty difference. The trace now reports each number once per process, and the check selects records by process name.

Phase 6 originally used `/bin/cat` as its reference. Os-real has no such file: busybox dispatches `cat` as an internal applet, while the in-house `cat` belongs to the other image. The test compares against `busybox cat`, the implementation that os-real uses.

## The kernel VMA table change

While reading musl's 1.9 MB `libc.a`, tcc reported `memory full (malloc)`. Physical memory usage was low, and almost all of the address space remained free.

Each process stores its address-space regions in a fixed array bounded by `VMA_MAX` in `rach2/sys/mm/vm.h`. The limit was 16. Code, data, heap, and stack consume four entries before the program begins, leaving room for twelve large allocations. Musl uses `mmap` for sufficiently large allocations, and each mapping consumes another entry. The malloc error did not identify the exhausted table.

The limit is now 64. The 55th probe, `tests/musl/vmamusl.c`, creates 24 mappings, writes a distinct byte to the first and last page of each, and verifies every value. It detects overlapping table entries as well as premature exhaustion.

## What shipped

The following commits changed code, configuration, or a manifest. The list comes from `git show --stat` and excludes commits that only changed this record.

* `119b2c2` vendors tinycc, cross-builds it with the guest search paths, and adds `tools/check_elf.py` to check an ELF against each `elf_load_image` contract and the separate project policy.
* `8120992` applies a pinned build-time patch that makes the compiler reject `-run`, `-shared`, and dynamic executable links.
* `dc592ef` checks the address window accepted by `exec` rather than the wider usercopy window, using literal boundary addresses.
* `ef9cc28` adds the `tree` manifest directive because a manifest cannot list 218 headers individually.
* `5619d26` also rejects raw images because output format and output type are separate fields.
* `e5bd651` preserves an explicitly declared directory, rejects a symlinked tree root, and checks the image rather than the manifest for `/sbin/init`.
* `db7b7a1` stages the compiler and sysroot in os-real.
* `cc27e9f` raises `VMA_MAX` to 64 and adds the probe described above.
* `d25a97d` adds `make tcc-test`, the ENOSYS trace controlled by `rach2.trace=enosys`, and `tools/check_link.py`.
* `48b7a28` compiles the project's `cat` in the guest and compares it with busybox's implementation.
* `17a454b` reports missing syscalls by compiler process name instead of subtracting syscall numbers from separate boots.
* `2893d4e` adds the `make ssh-test` target so the verification run can execute the SSH capstone unattended.
* `26b6ba2` updates prose affected by the compiler.
* `9caec47` compares staged headers with their source tree and requires them to be readable by an ordinary account.
* `c465f67` strengthens both new tests and ensures that they stop their machines on every exit path.
* `b18b59b` applies the same cleanup to the four older boot harnesses.
* `55b496e` makes the link check count only exported definitions because local definitions cannot resolve external references.
* `e9d1499` makes the capstone request a terminal and prove that the session received one.
* `fe1d4be` reports missing or non-directory sysroot ancestors and requires each node name to contain one path component.

## What the reviews caught

Each phase received an independent adversarial review, and every finding was checked against the code.

The compiler layer had three confirmed defects. The output policy rejected dynamic executables and shared libraries but allowed `-Wl,-oformat=binary`, a separate field in the same structure that produces a raw image unsupported by the loader. The ELF checker accepted images that `exec` rejects, as described above. Finally, the guest could not link: gcc-built musl refers to `__udivmoddi4`, `__divmoddi4`, and `__mulxc3`, but the guest has no libgcc to provide them. Three helper objects are now built into `libtcc1.a`. On the host, `tools/check_link.py` examines the exact staged files and requires their undefined symbols, minus their combined exported definitions, to be empty. It names all three exclusions explicitly.

The stager had three confirmed defects. A `tree` entry reapplied the mode to an already declared directory, so `dir 0755 /include` followed by a `tree` below it could silently change ownership. The stager followed a symlinked tree root instead of rejecting it. The guard also asked the manifest whether `/sbin/init` existed instead of inspecting the image.

The tests had four confirmed defects. A host-side end-anchored pattern checked the residue from a rejected compile, but console lines end with a carriage return; the pattern matched nothing and accepted any output. The check now runs in the guest. The ENOSYS verdict ran before the largest compile, so syscalls from that compile could appear after the test's only log read. It now runs last. The `cat` comparison used a 29-byte file, requiring only one read and one write; a `cat` implementation that stopped after its first read or ignored a second argument could pass. It now compares a 21 KB binary file and then two files, with every status chained. The SSH test established two sessions without proving their authentication methods. The `-i` option only adds an identity, so an agent key could authenticate root, and the password case never proved that a password was requested. Each login is now restricted to its claimed method. The test also rejects three cases: no credentials, the test account with a one-character password error, and root by password, which `-g` forbids.

One confirmed defect was outside the milestone's subject. Every boot harness in `tools/` stopped its machine only on its explicit success and failure paths. Cancellation after boot but before the verdict left QEMU running with the disk copy and forwarded port whose names the trap had removed. All six harnesses now perform machine cleanup in the trap.

The header checker also compared source and staged trees only by file count. Removing a header and adding any other file preserved the count. It now compares every path and byte.

The next review found three more defects; the first invalidated an earlier argument in this record. A fourth review of the closing commits found another three, described below. Placing SSH refusals after two successful logins proves only that the server answered recently, not that it answered each rejected attempt. A connection reset, a newly dead server, or an expect parse failure caused by `[` or `$` in a generated password also produces no output. Each refusal must now include `Permission denied`, proving that ssh received a server response. Variable data reaches expect through the environment instead of being inserted into its source. The header comparison also failed to prove that the compiler could open the staged files: `tree 0600 /include` preserves every byte while limiting the sysroot to root. The guard now requires world-read permission on compiler inputs, world-execute permission on `/bin/tcc`, and world-execute permission on every parent directory. Finally, cleanup cleared each pid after killing and reaping it. A trap in that interval could signal a new process that had inherited the pid. Cleanup now takes and clears the pid before using it.

The final review found three defects in the closing commits, including an inaccurate sentence in this record. The SSH test claimed that its sessions used a dropbear pty, but ssh does not request a terminal when given a command. Both logins therefore used pipes and never exercised the pty path. They now request a terminal with `-tt` and run `test -t 0 && test -t 1` before printing the success token, so a missing terminal fails the test. The link check counted local definitions. `nm` marks a local symbol with a lowercase letter, and the archive contains a local `__udivmoddi4` beside the injected global definition. A linker cannot use the local symbol, so removing the injected object left the check passing while student links failed. The checker now counts only exported definitions and fails when `nm` fails instead of treating empty output as success. The sysroot reachability check also skipped missing and non-directory ancestors, reported by the resolver as `ENOENT` and `ENOTDIR`. It now reports both cases and requires each node name to be one path component; a name such as `a/b` would otherwise describe an inventory path that the kernel cannot resolve.

Two proposed mutations did not make their tests fail. Making `cat` write half a buffer still writes all data because the loop repeats; the effective mutation starts each write one byte into the buffer. Removing a header from the host tree changed the reference count along with the staged count. The effective mutation stages only a subset while leaving the source tree intact.

## What stays deferred, and why

The compiler rejects `tcc -run` and every unsupported output: `-shared`, an executable link without `-static`, and a raw image. The `-run` mode compiles into memory and jumps, which requires the unavailable `mprotect` syscall. A dynamic executable would load and jump without relocation because the loader maps `PT_LOAD` segments but ignores `PT_INTERP` and `PT_DYNAMIC`. Each rejection explains the limitation rather than accepting a command that cannot run correctly.

The `tcc -b` bounds-checking mode remains unavailable because the build requires `--config-musl`, as explained above.

This milestone does not compile the kernel in the guest. Tcc implements only a subset of the linker-script language used by `rach2/link.ld`; its assembler lacks the GAS `.macro` used by `rach2/sys/isr_handler.s`; and the guest then had no `make` to drive the build. Milestone 21 added make, and milestone 22 removed the other two obstacles: the kernel now uses tcc's native linker behind a structural checker, the stubs contain no macros, and `make selfbuild-test` builds and boots the kernel in the guest. Those later changes show that the limitations were solvable but outside this milestone. Bellard's tcc build of a Linux 2.4 kernel also established the general direction.

The guest does not compile musl because its build requires GNU make, `ar`, `ranlib`, and a shell toolchain. Dropbear is ordinary C that tcc could probably compile, but it requires `make` and more memory than this exercise warrants. The image instead carries the host-built `libc.a` and headers.

The machine has no `make`, so multi-step builds require explicit command lines. A single command can compile multiple translation units, as proved with `ping.c` and `resolv.c`, matching the host build of that program; a project build remains unavailable. Milestone 21 later closed this deferral by adding pdpmake. The port depended on the clock and timestamp support beneath it, so about nine tenths of that milestone was kernel work.

`MAX_FDS` remains 16. Each nested `#include` holds a descriptor until that include returns. The tested programs nest only a few includes rather than sixteen, but the descriptor limit remains small.

## Verification

The final verification ran once after `make distclean` emptied the tree, rebuilding musl, busybox, dropbear, and tcc from their tarballs: `make full`, `make verify-style`, `make verify-host` with 18 host boundary tests, `make test` with 55 of 55 probes, `make tcc-test`, `make ssh-test`, `make account-test` for both images, `make shutdown-test`, `make login-test`, `make ping-test`, `make crash-test`, `make test-disk`, `make test-disk-ahci`, `make persist-test`, `make persist-test-ahci`, `make cred-persist-test`, and `make test-noentropy`. Every target passed.

The capstone now runs as a target in that sequence. `make ssh-test` logged root in by key and the test account by password over a dropbear pty, then rejected three attempts. As the unprivileged `test` account, `make tcc-test` compiled and ran a program, compiled the project's `cat` and `ping` from source in the image, and verified rejection of the four unsupported output forms.
