# Milestone 20: a compiler on the machine

Rach2 has always been a machine you build somewhere else. A student edits a program on a Mac, rebuilds an image, and boots it. This milestone makes the machine able to build a program on itself: write C at its own prompt, compile it there, run the result, and compile one of this project's own tools from the source the image carries.

The compiler is tinycc 0.9.27, vendored and cross-built the way musl, busybox and dropbear already are. It is the only choice the constraint leaves. Turning C into a running program takes a compiler, an assembler and a linker; gcc and clang are only the first of the three and hand their output to GNU `as` and `ld`, so porting either means porting binutils, and then a `make`, which this image does not have. The small teaching compilers lose the same way. tcc is all three in one binary.

The ceiling is stated as plainly as the goal: static output only, no `-run`, and the largest thing the machine compiles is one of its own utilities. The kernel, the C library and the SSH server are not compiled in the guest, and the reasons are in the plan beside the phases rather than left to be rediscovered.

## What a student does with it

```
$ ssh -i krffs/rach2_demo_ed25519 -p 2222 root@localhost
# printf '#include <stdio.h>\nint main(void){puts("hello");return 0;}\n' > /tmp/m.c
# tcc -static -o /tmp/m /tmp/m.c
# /tmp/m
hello
# tcc -static -o /tmp/cat /src/utils/cat.c
```

The sysroot is readable by an ordinary user, which is the difference between a compiler on the machine and a compiler for the administrator: `make tcc-test` does all of the above as the unprivileged `test` account.

## Where the plan was wrong

The plan is corrected in place; this is the summary of what the work disproved.

`--config-musl` was dropped from the configure line on the reasoning that its name only concerns a dynamic loader path, which is nothing to a system with no dynamic linker. The build disproved it in one step: the flag also drops `bcheck.o` from the runtime (`lib/Makefile:31-33`), and `bcheck.c` is written against glibc's malloc hooks that musl does not have, so without the flag `libtcc1.a` does not build at all. The cost is that `tcc -b`, bounds checking, is not available on this machine.

The ELF checker was written against the wrong window. `mm/usercopy.h` allows user addresses up to `0xC0000000` and `elf_load_image` loads only up to `0xBFFFF000`, one page less, so a checker using the wider bound approves an image `exec` refuses. Worse, the boundary cases were first written in terms of the constant they were testing, so they moved with the mistake and passed. They are literal addresses now.

The syscall verdict was to be a subtraction: record what a program known to work asks for, then require the compiler's run to add nothing. A trace that prints each number once per boot cannot answer that, because a number some earlier process already reported is suppressed for the compiler and the subtraction comes back empty. The trace names each number once per PROCESS and the question is asked by process name.

Phase 6's reference was to be `/bin/cat`. There is no such file in os-real: busybox dispatches `cat` as an applet inside itself, and the in-house `cat` lives in the other image. The comparison is against `busybox cat`, which is the program that image actually uses.

## The one kernel change, and why it was not memory

Reading musl's 1.9 MB `libc.a`, tcc reported `memory full (malloc)` on a machine whose physical memory was barely touched and whose address space was almost entirely free.

A process's address space here is a fixed array of regions, `VMA_MAX` in `rach2/sys/mm/vm.h`. That number was 16, and four are spoken for before a program runs a line: its code, its data, the heap and the stack. musl's allocator reaches for `mmap` once an allocation is large enough, and every one of those takes a region, so the ceiling was twelve large allocations held at once. malloc's failure message says nothing about tables, which is why this looked like a memory problem for longer than it should have.

The number is 64 now, and `tests/musl/vmamusl.c` is the 55th probe: it takes 24 mappings one at a time, writes a distinct byte into the first and last page of each, and requires them all to still hold it, so a table that overlapped its entries would fail as loudly as one that ran out.

## What shipped

The commits that changed code, configuration or a manifest, read from `git show --stat` rather than from the plan. The ones that only wrote this record are not listed.

* `119b2c2` vendors tinycc, cross-built for this machine with the guest's own search paths baked in, and adds `tools/check_elf.py`, which judges an ELF against `elf_load_image`'s contract rule for rule and against this project's policy separately.
* `8120992` refuses `-run`, `-shared` and a dynamic executable link inside the compiler itself, by a pinned patch applied at build time.
* `dc592ef` judges an ELF by the window `exec` loads into, not the wider one usercopy allows, with the boundary cases written as literal addresses.
* `ef9cc28` adds the `tree` manifest directive, since a manifest cannot name 218 headers.
* `5619d26` refuses a raw image too, the output FORMAT being a different field from the output type.
* `e5bd651` leaves a declared directory alone, refuses a linked tree root, and asks the IMAGE about `/sbin/init` rather than the manifest.
* `db7b7a1` stages the compiler and its sysroot into os-real.
* `cc27e9f` raises `VMA_MAX` to 64, with the probe above.
* `d25a97d` adds `make tcc-test`, the ENOSYS trace behind `rach2.trace=enosys`, and `tools/check_link.py`.
* `48b7a28` compiles this project's own `cat` on the machine and checks it against busybox's.
* `17a454b` asks the compiler by name what it could not find, rather than subtracting one boot's numbers from another's.
* `2893d4e` makes the SSH capstone a target, `make ssh-test`, so the gate can run it without a person at the keyboard.
* `26b6ba2` sweeps the prose the compiler changed.
* `9caec47` compares the staged headers with the tree they came from, and requires an ordinary account to be able to read them.
* `c465f67` makes both new tests prove what they claim, and ends their machines on every way out.
* `b18b59b` gives the four older boot harnesses the same way out.

## What the reviews caught

Every phase was reviewed by an independent adversarial reader and every finding was adjudicated against the code.

Confirmed and fixed, in the compiler's own layer: the output policy refused a dynamic executable and a shared library but not `-Wl,-oformat=binary`, which is a different field of the same structure and produces a raw image this loader cannot touch; the ELF checker approved images `exec` refuses, as above; and the guest could not link at all, because musl built by gcc references `__udivmoddi4`, `__divmoddi4` and `__mulxc3`, the guest has no libgcc to append, and a student's first link would have failed on an undefined symbol. Three helper objects are compiled into `libtcc1.a` now, and `tools/check_link.py` asks the question on the host, of the very files that will be staged: every undefined symbol in the staged libraries, minus everything they define between them, must be empty, with three exclusions each stated rather than quietly dropped.

Confirmed and fixed, in the stager: `tree` re-applied the mode to a directory another line had already declared, so a `dir 0755 /include` followed by a `tree` under it could be silently re-owned; a tree root that was a symlink was followed instead of refused; and the guard asked the MANIFEST whether an image carries `/sbin/init` when it is the image that answers.

Confirmed and fixed, in the tests themselves. The leftover check for a refused compile was written as a host-side pattern anchored at the end of a line, and this console ends its lines with a carriage return, so it matched nothing and passed whatever was there; it is asked in the guest now. The ENOSYS verdict was evaluated before the largest compile in the run had happened, so a syscall that compile went on to ask for would have been logged after the only look at the log; it is the last thing the test does now. The `cat` comparison ran over a 29-byte file, which is one read and one write, so a `cat` that returned after its first read or ignored its second argument would have passed; it runs over 21 KB of a binary file and again over two files, chained so every status is required. And the SSH test proved that two sessions happened without proving how either was let in: `-i` only ADDS an identity, so an agent key could have carried the root login, and the password half never recorded that a password was asked for. Each login is pinned to the method it claims now, and three attempts must be refused: no credentials at all, the test account one character off its password, and root by password, which `-g` bars.

Confirmed and fixed, outside the milestone's subject: every boot harness in `tools/` killed its machine only on the two paths it wrote for itself, success and its own failure. A run cancelled between the boot and the verdict left QEMU alive, holding the disk copy and the forwarded port whose names the trap had just removed. The cleanup is the trap now in all six.

And one about a checker rather than a system: the staged header tree was compared with the tree it came from by COUNTING files. Drop a header, gain a file of any name, and the count is right again. It is compared path by path and byte by byte now.

The round after all of that was right about three more, and the first of them undid an argument this record had already made. Ordering the SSH refusals after the two logins that must work shows the server was answering a moment ago; it does not show it answered THIS attempt. A connection reset, a server that had just died, and an expect script that failed on a `[` or a `$` in a generated password all produce exactly what a refusal produces, which is nothing. Each refusal now has to carry `Permission denied`, which is ssh reporting an answer it received, and everything variable reaches expect through the environment rather than being pasted into its source. The second: the header comparison said nothing about whether the compiler could OPEN any of it, and `tree 0600 /include` stages every byte correctly into a sysroot only root can use; the guard now requires the world's read bit on what the compiler reads, its execute bit on `/bin/tcc`, and its execute bit on every directory above any of them. The third: the new cleanup cleared each pid after killing and reaping it, so a trap arriving in that window could signal a pid the host had since given to somebody else. The pid is taken and forgotten first now.

Two mutations meant to prove a test could fail did not, and both were instructive. Writing half a buffer in `cat` still writes everything, because the loop goes round again; the mutation that means something starts the write one byte in. And removing a header from the host tree moved the count the check compared against, so the check stayed happy; the mutation that means something stages a subset of the tree while the tree stays whole.

## What stays deferred, and why

`tcc -run` is refused, and so is every dynamic output: `-shared`, an executable link without `-static`, and a raw image. `-run` compiles into memory and jumps, which needs `mprotect`, and this kernel has none. A dynamically linked program would load and jump with nothing relocated, because the loader maps `PT_LOAD` segments and never looks at `PT_INTERP` or `PT_DYNAMIC`. Each refusal names its reason, because a compiler that accepts the command and dies strangely has taught the student a lie about the machine.

`tcc -b`, bounds checking, is not available, for the `--config-musl` reason above.

The kernel is not compiled in the guest. `rach2/link.ld` is a real linker script and tcc implements a subset of that language; `rach2/sys/isr_handler.s` uses a GAS `.macro`, which tcc's assembler does not have; and there is no `make` in the guest to drive any of it. Each is solvable and none is this milestone. Bellard compiled a Linux 2.4 kernel with tcc, so the direction is not fanciful.

musl is not compiled in the guest: its own build wants GNU make, `ar`, `ranlib` and a shell toolchain. dropbear is ordinary C that tcc could probably take, but it wants `make` and more memory than the exercise is worth. The image ships the `libc.a` and headers the host build already produces.

There is no `make` on the machine, so a build of more than one command is a command line. Two translation units in one command work and are proved (`ping.c` with `resolv.c`, which is how the host builds that same program); a project is not.

`MAX_FDS` is 16 and a nested `#include` holds a descriptor open for as long as it is nested. It did not bite, because the includes these programs reach nest a few deep and not sixteen, but the ceiling is real and the number is small.

## The gate

Run in one pass from a tree `make distclean` had emptied, so musl, busybox,
dropbear and tcc were all built from their tarballs again: `make full`, then
`make verify-style`, `make verify-host` (18 host seam tests), `make test` (55 of
55 probes), `make tcc-test`, `make ssh-test`, `make account-test` (both images),
`make shutdown-test`, `make login-test`, `make ping-test`, `make crash-test`,
`make test-disk`, `make test-disk-ahci`, `make persist-test`,
`make persist-test-ahci`, `make cred-persist-test` and `make test-noentropy`.
Every one passed.

The capstone is a target now rather than a procedure, so it ran inside that
pass: `make ssh-test` logged root in by key and the test account by password,
over a dropbear pty, and had three attempts refused. `make tcc-test` compiled a
program, ran it, compiled this project's `cat` and `ping` from the source the
image carries, and was refused the four kinds of output this machine cannot
run, all as the unprivileged `test` account.
