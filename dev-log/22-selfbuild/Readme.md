# Milestone 22: building Rach2 on Rach2

Milestone 20 added the compiler, and milestone 21 added a clock and make. This milestone makes Rach2 self-hosting. It builds the kernel, 24 utilities, and the shell from its staged sources. The test extracts the resulting kernel bytes, checks them, and boots them through the complete probe suite. `make selfbuild-test` takes about seven minutes.

## Building in the guest

```
# cd /src/rach2
# make -f guest.mk        # 71 objects, then the link through @kernel.objs
```

The same command works in `/src/utils` and `/src/ish`. Root can build each tree in place. An unprivileged user must first copy a tree home with `cp -r /src/rach2 ~ && cd ~/rach2` because root owns `/src`. Earlier milestones staged TinyCC as `/bin/cc` and pdpmake as `/bin/make`. Each source tree includes a POSIX `guest.mk` for pdpmake because the project's host makefiles require GNU make. The stager assigns every node the same nonzero time, one second past the epoch. A constant time makes repeated image builds identical, while a nonzero time prevents pdpmake from treating a source as absent.

## Four build blockers

Planning used two independent investigations, one in the main session and one by Codex. Comparing both results with the vendored TinyCC source exposed four blockers where milestone 20 had identified two. The fixes keep one source tree valid for both toolchains without compiler conditionals.

The linker script was the first blocker. The TinyCC script parser supports only the libc-stub subset and cannot read `rach2/link.ld`. The guest build therefore uses TinyCC's native ELF layout. Both linkers use `_start` as the entry symbol. The boot allocator reads `_end`, which TinyCC defines and `link.ld` assigns for GNU ld. The guest link is `cc -static -nostdlib -o kernel @kernel.objs -Wl,-Ttext=0x100074 /lib/tcc/libtcc1.a`. Its response file is necessary because 71 objects exceed the 63-argument exec limit. The address places TinyCC's first load segment at file offset 0 and address 0x100000. Every host kernel link runs `tools/check_kernel.py` to check the multiboot header, both load segments, the entry, and the image end against the loader contracts. After extraction, the host checks the guest kernel with the `--tcc` profile because Python is unavailable in the guest. Both link commands write a temporary file and rename it only after a successful check. A failure therefore cannot leave a newer partial kernel that make considers current.

The assembler dialect was the second blocker. The TinyCC assembler supports every directive and privileged instruction used by the five `.s` files except `.macro`. The source therefore writes all 49 interrupt stubs explicitly and replaces `pushal` and `popal` with the `pusha` and `popa` spellings accepted by both assemblers. The phase compares the linked kernel's `.text`, `.rodata`, and `.data` sections before and after the change and fails on any byte difference.

The third blocker was three missing instruction names. The TinyCC opcode table omits `clflush`, `rdrand`, and `rdseed`. Their four call sites now emit fixed-register `.byte` sequences for both compilers, so the host and guest kernels execute identical bytes. A `#ifdef __TINYC__` alternative was rejected because it would make kernel behavior depend on the compiler.

The compiler-runtime ABI was the fourth blocker. The TinyCC compiler calls `__divdi3`-family helpers for arithmetic that GCC inlines, so the guest link includes `libtcc1.a`. It also calls `memmove` and `memset` for structure assignment and initializer gaps. `kstring.c` now defines the three standard memory functions but keeps their bodies in the `k_` forms. The host test runs that file under AddressSanitizer, which replaces calls to the standard names with its own implementations. Two unsupported constructs were also rewritten in the common dialect: `__builtin_unreachable()` became a `noreturn` declaration, and a statement-position fallthrough attribute became the `/* fall through */` comment.

## Limits measured in the guest

The guest builds reached three limits. Measurements determined each new value, and each increase was reviewed separately.

The first limit was the page cache. The initial kernel build took more than a minute for each compilation. Every compiler invocation faulted the 269 KiB compiler, sources, headers, and objects through a 1 MiB cache backed by polled disk reads, so the working set could not remain resident. The cache is now 4 MiB (`PC_SLOTS` in `rach2/sys/mm/pagecache.h`). On a small-memory machine it shrinks to one quarter of the free frames, with the old 1 MiB value as its minimum. This change reduced the projected four-and-a-half-hour two-build run to about one minute. A 12 MiB boot checked the adaptive size. `blockprobe` also increased to 24 files of 256 KiB so its working set still exceeds the cache and tests eviction.

The second limit was the per-process VMA table. Linking `login` combines two sources, and its SHA-512 crypt code pulls more `libc.a` members than any one-source link. It exhausted the 64-entry table and made TinyCC report that memory was full, matching the earlier failure at the 16-entry limit in milestone 20. The limit is now 128. `vmamusl` requires more mappings than a 64-entry table can provide, so the suite detects a regression.

The third limit was the kernel's lack of SIMD state. `busybox sha256sum` selected its accelerated path from CPUID and then faulted on the first vector instruction. CPUID reports CPU features, but this kernel leaves `CR4.OSFXSR` clear and does not save XMM registers during a context switch. The two `HWACCEL` options are disabled in `rach2.config`, so BusyBox uses the portable C paths. The initial diagnosis blamed the emulator. Review instead identified the kernel limitation recorded in the deferral section.

## What shipped

Fourteen implementation commits precede this record. The dialect work comprises assembly changes (`c7f3d24`), C changes (`5375718`), and instruction encodings (`8f3e329`). The remaining commits added the linker contract and checker (`cc3f2e3`), guest makefiles and their host guard (`266a187`), staged sources pinned by the checks (`8ffb033`), the bounds-checking extractor (`c409ce6`), the page-cache increase (`374e508`), the BusyBox configuration (`db6e234`), the kernel harness (`c1c23f9`), the boot proof (`d76f24d`), two userland stages (`1128283` and `f58157f`), and the final review (`ba9bbfe`).

`make selfbuild-test` performs the complete proof. It boots a copy of os-real, logs in as the unprivileged `test` account, copies `/src/rach2` home, and builds the kernel in two clean workspaces. Their hashes must match. It then builds the 24 utilities and ish. Runnable programs execute with real inputs. The four credential programs must return their exact documented refusal statuses. The four programs that cannot run harmlessly, `halt`, both init variants, and the crash fixture, must be valid executables. After a sync, the host extracts the exact kernel with `readfile.krffs` and compares its hash with the guest's value. It runs `check_kernel.py --tcc` and boots the extracted bytes through all 61 probes. If Docker is available, the same bytes are placed on a GRUB disk and must reach a login prompt; otherwise the harness prints the reason for the skip. A second short boot logs in as root and rebuilds the staged `/src/ish` without copying or touching it. `tools/check_repro.py` checks the fixed time on every staged node, and this boot proves that the fixed time permits an in-place build.

## What the reviews caught

Every phase went through an adversarial review, and the largest catches changed real contracts:

* The extractor checked bounds with addition, allowing a crafted 64-bit node size to wrap past the limit. It now uses subtraction, and the host test includes UINT64_MAX cases. The same review found that the output path could identify the input image. Opening it with `fopen("wb")` would truncate the image before reading its first byte. The extractor now rejects equal input and output device and inode pairs.
* Codex constructed three malformed kernels that the checker accepted: one with a zero physical address, one with a load beyond the file, and one with an entry inside a load's zero-filled region. The checker also scanned four bytes beyond QEMU's multiboot search limit. Each rule now has its own self-test fixture and expected diagnostic, so a mutation cannot pass by failing a different check.
* A failed kernel link could leave an artifact that make considered current. The link now uses a unique temporary file for each invocation and publishes it by rename only after the check passes.
* `kernel.objs` had no prerequisites, so changing the object list could leave a stale response file. It now depends on `guest.mk` and regenerates through a temporary file.
* The exec-budget check measured makefile text before macro expansion. It now expands macros as pdpmake does and includes an environment allowance before comparing the result with the kernel's 3072-byte limit.
* The reproducibility check did not test directory traversal order because a host might already return sorted names. It now checks the exact sorted traversal at two directory levels with scrambled fixtures. Removing the stager's sort makes the mutation fail.
* A later manifest line could reassign the staged `/src` trees to another owner. The manifest checks now allow only the intended `/src` entries. Their inventory fixes the path, mode, owner, and declared host source for each guest makefile and one representative source. The image check verifies each node but not its bytes; the builds consume and thereby test those bytes.
* The userland smoke checks accepted any nonzero refusal status, including a crash or failed exec. Each credential program must now return its exact documented refusal status.
* The Docker operations in the GRUB test had no time limit. A watchdog now bounds them.
* After the cache increase, the `blockprobe` working set fit in the cache and `test-disk` no longer tested eviction. Its working set is now 6 MiB and exceeds the cache again.

## Corrected predictions

The plan expected removing loader.o to report a missing `_start`, but TinyCC stops earlier at the loader's undefined symbols. The plan now records that diagnostic. It also expected a vector-8 stub mutation to fail the probe suite. The mutated kernel passes all 61 probes because no probe raises a double fault. The effective negative instead changes vector 14 and breaks the first boot step; the plan records vector-8 coverage as a suite limitation. Two proposed guest preflight checks were impossible. The kernel lacks `statfs`, so the host checks disk space through the free-node chain. `/proc` does not list processes, so the harness enforces the process budget by allowing only one session. Review replaced the initial emulator-based SHA diagnosis with the kernel's missing SIMD-state contract. TinyCC also emits no symbol table. Symbol assertions therefore apply only to the GNU-linked kernel, while the TinyCC checker profile fixes the required layout.

## Deferred work

The kernel does not enable SIMD state, although CPUID advertises the CPU's vector features. A program that selects a vector path from CPUID will therefore fault, as accelerated BusyBox did. A complete fix must enable `CR4.OSFXSR` and preserve XMM state per process, or hide features that the boot cannot support. That work is outside this milestone.

The GRUB boot test is implemented and bounded. It was skipped on the development machine because the Docker daemon was not running. QEMU completed every other boot proof, and the harness reports which of the two Docker prerequisites caused a skip. The GRUB test runs on hosts where `make os-real-grub-disk` already works.

No probe raises a double fault, so the suite cannot execute the vector 8 stub. Its evidence is the phase-1 byte comparison with the macro-expanded original.

Neither musl nor Dropbear is compiled in the guest, for the reasons recorded in milestone 20. The pdpmake program parses `-j` but does not implement parallel jobs. The TinyCC compiler's `-Wall` checks less than the host's `-Werror -Wextra` policy, so the host compilation remains the strict correctness check. The guest build is the final integration test over the same sources.

## Verification

The complete verification set passed in one run after `make distclean` emptied the build tree. musl, BusyBox, Dropbear, TinyCC, and pdpmake were rebuilt from their tarballs. The run used `make full`, `verify-style` with its 12 numbered checks and tool preflight, `verify-host` with 20 host tests, `test` with 61 of 61 probes, `tcc-test`, `make-test`, `selfbuild-test`, `ssh-test`, `account-test`, `shutdown-test`, `login-test`, `ping-test`, `crash-test`, `test-disk`, `test-disk-ahci`, `persist-test`, `persist-test-ahci`, `cred-persist-test`, and `test-noentropy`.
