# Milestone 22: building Rach2 on Rach2

Milestone 20 put a compiler on the machine, milestone 21 gave it a clock and a make, and this milestone makes the machine self-hosting: it builds its own kernel, its 24 utilities and its shell from the source it carries, and the exact kernel bytes it built are extracted, checked and booted through the full probe suite. `make selfbuild-test` runs the whole of that in about seven minutes.

## What a student does with it

```
# cd /src/rach2
# make -f guest.mk        # 71 objects, then the link through @kernel.objs
```

The same works for `/src/utils` and `/src/ish`, and root builds them in place; an unprivileged account copies a tree home first (`cp -r /src/rach2 ~ && cd ~/rach2`), since `/src` is root's. The build tools are the ones earlier milestones staged: tcc as `/bin/cc`, pdpmake as `/bin/make`, and each source tree carries the POSIX `guest.mk` that pdpmake reads, since the repository's own makefiles are GNU makefiles it cannot. In-place builds work because the stager stamps every staged node with a fixed nonzero instant, one second past the epoch: constant, because two builds of one tree must produce one image; nonzero, because pdpmake reads a zero modification time as a file that does not exist.

## The four blockers, and what each turned out to need

The planning investigation (two independent passes, one here and one by Codex, argued to agreement against the vendored tcc source) found four blockers where milestone 20 had named two. All four were closed by making one source tree serve both toolchains, with no compiler conditionals anywhere.

The linker script. tcc's linker cannot read `rach2/link.ld` (its script parser handles the libc-stub subset only), so the kernel now links through tcc's native ELF layout: the entry symbol is `_start` (both linkers' convention), the boot allocator reads `_end` (which tcc defines itself and `link.ld` assigns for GNU ld), and the guest link is one command, `cc -static -nostdlib -o kernel @kernel.objs -Wl,-Ttext=0x100074 /lib/tcc/libtcc1.a`. The response file exists because 71 objects exceed the machine's own 63-argument exec limit; the address makes tcc's first load segment start at file offset 0 with address exactly 0x100000. `tools/check_kernel.py`, run on every host kernel link, checks the multiboot header, the two load segments, the entry and the image end against what the loaders require; the guest-built kernel is checked with its `--tcc` profile after extraction, on the host, where Python exists. Both links publish through a temporary file renamed on success, so a failed link or a failed check cannot leave a newer partial kernel that the next make treats as current.

The assembler dialect. tcc's assembler has every directive and privileged instruction the kernel's five `.s` files use except `.macro`, so the 49 interrupt stubs are written out in full, and `pushal`/`popal` are respelled `pusha`/`popa`, which both assemblers accept. The proof is byte identity: the `.text`, `.rodata` and `.data` sections of the linked kernel are compared before and after the change, section by section, and any difference fails the phase.

Three missing instruction names. `clflush`, `rdrand` and `rdseed` are absent from tcc's opcode table and are now emitted as `.byte` sequences with fixed registers at their four sites, unconditionally, so the guest-built kernel executes exactly the bytes the host-built one does. A `#ifdef __TINYC__` substitute was rejected for that reason: a self-build proof over a kernel that behaves differently per compiler proves less.

The compiler-runtime ABI. tcc calls `__divdi3`-family helpers where gcc inlines the arithmetic, so the guest link carries `libtcc1.a`; tcc also emits calls to `memmove` and `memset` for struct assignment and initializer holes, so `kstring.c` now defines the standard memory trio (the bodies stay in the `k_` forms, because the host seam test runs that file under AddressSanitizer, which rewrites calls to the standard names into its own copies). Two constructs tcc cannot take were replaced with forms both compilers accept: `__builtin_unreachable()` became a `noreturn` declaration, and the statement-position fallthrough attribute became the `/* fall through */` comment.

## What the in-guest builds measured

Three limits were reached during the in-guest builds, each measured rather than predicted, and each raised as its own reviewed change.

The page cache. The first in-guest kernel build ran at more than a minute per compile: every compile faulted the 269 KiB compiler, the sources, the headers and the objects through a 1 MiB cache window over polled disk reads, so the working set never stayed resident. The cache is 4 MiB now (`PC_SLOTS` in `rach2/sys/mm/pagecache.h`), adapted down to a quarter of the free frames on a small-memory machine with the old 1 MiB size as its floor, and the whole two-build run fell from a projected four and a half hours to about a minute. A 12 MiB boot was exercised to show the adaptation holds. `blockprobe` grew with it, to 24 files of 256 KiB, because an eviction proof whose working set fits the cache proves nothing about eviction.

The VMA table. Linking `login` (two sources whose SHA-512 crypt pulls more `libc.a` members than any one-source link) exhausted the 64-entry per-process mapping table, with tcc reporting memory full, the same failure shape that hit the 16-entry table in milestone 20. It is 128 now, and `vmamusl` requires more mappings than 64 could grant, so a regressed table fails the suite.

busybox's SHA acceleration. `busybox sha256sum` died with an invalid opcode in the guest: the accelerated path dispatches on CPUID, which reports what the CPU has, and this kernel never enables SSE state (`CR4.OSFXSR` stays clear, no context switch saves the XMM registers), so the accelerated routine's first vector instruction faults. The two `HWACCEL` options are off in `rach2.config` and the plain C paths run everywhere. The first written diagnosis blamed the emulator; the review traced it to the kernel's own SIMD gap, which is recorded as deferred below.

## What shipped

Fourteen working commits before this record. The dialect changes (`c7f3d24` assembly, `5375718` C, `8f3e329` encodings), the linker contract and its checker (`cc3f2e3`), the guest makefiles with their host-side guard (`266a187`), the staged sources pinned by the sweeps (`8ffb033`), the extractor with its bounds-validating path walk (`c409ce6`), the page cache (`374e508`), the busybox config (`db6e234`), the harness (`c1c23f9` the kernel, `d76f24d` the boot proof, `1128283` and `f58157f` the userland), and the sweep (`ba9bbfe`).

`make selfbuild-test` is the milestone as one target: boot os-real from a disk copy as the unprivileged `test` account, copy `/src/rach2` home, build the kernel twice from clean workspaces and require the two hashes equal, build the 24 utilities and ish (the runnable ones executed against real inputs, the four credential programs held to their exact documented refusal statuses, and the four that cannot be run harmlessly, `halt`, the two init variants and the crash fixture, checked as built executables), sync, then on the host extract the exact kernel bytes with `readfile.krffs`, require the host hash to equal the one the guest recorded, run `check_kernel.py --tcc`, and boot the bytes through all 61 probes. Where Docker is available the same bytes are built into a GRUB disk and must reach a login prompt; where it is not, the skip is printed with its reason. A second, short boot then logs in as root and rebuilds the staged `/src/ish` in place, no copy and no touch, which holds the fixed staged instant to its purpose: `tools/check_repro.py` asserts the value on every staged node, and this boot proves the flow it exists for.

## What the reviews caught

Every phase went through an adversarial review, and the largest catches changed real contracts:

* The extractor's bounds arithmetic used addition, which a crafted 64-bit node size wraps past; the checks subtract now, with UINT64_MAX cases in the seam test. The same review caught an output path that could name the image itself, which `fopen("wb")` would truncate before the first byte was read: the tool now refuses when the output's device and inode equal the input's.
* The kernel checker accepted three malformed kernels Codex crafted against it (a zero physical address, a load past the end of the file, an entry in a load's zero-fill) and scanned four bytes past QEMU's real multiboot search bound; all are rules with their own selftest fixtures now, each matched to its own diagnostic so a mutation cannot pass by tripping a different rule.
* The kernel link could leave a failed artifact where the next make would treat it as current; it publishes by rename after the check, under a per-invocation temporary name.
* `kernel.objs` had no prerequisites, so an edited object list would have linked through the stale response file; it depends on `guest.mk` and regenerates through a temporary file.
* The exec-budget check measured unexpanded makefile text; it expands macros the way pdpmake will and adds an environment allowance before comparing against the kernel's 3072-byte limit.
* The reproducibility check never proved the tree walk's order (a host that returns names sorted exercises nothing); it now asserts the exact sorted walk at two directory levels against scrambled fixtures, and the mutation that removes the stager's sort fails it.
* The `/src` staging could be repointed by a later manifest line handing the trees to another owner; the manifest's `/src` surface is allowlisted, and the guest makefiles plus one representative source are pinned by path, mode, owner and declared host source in the set-id sweep's inventory (the image half verifies the node, not its bytes; the bytes are what the builds themselves consume).
* The userland smoke checks accepted any nonzero status as a refusal, which a crash or an exec failure also produces; each credential program is now required to exit with its documented refusal status exactly.
* The GRUB half's Docker operations had no bound; they run under a watchdog now.
* The sweep round found `blockprobe`'s working set no longer exceeded the grown cache, so `test-disk` was no longer exercising eviction at all; the working set is 6 MiB now and exceeds it again.

## What was refuted or disproved

The plan predicted the loader.o-dropped negative would report a missing `_start`; tcc stops earlier, at the loader's undefined symbols, and the plan records the observed diagnostic. The vector-8 stub mutation was expected to fail the probe suite; the built kernel carrying it passes all 61 probes, because nothing raises a double fault, so the demonstrated negative uses vector 14, which breaks the boot's first step, and the vector-8 latency is stated in the plan as a limit of the suite's coverage. The planned in-guest preflights were impossible as written: the kernel has no `statfs` for a disk-space check (it runs host-side over the free-node chain) and `/proc` does not enumerate processes (the harness's own one-session structure is the process budget). The first SHA diagnosis, blaming the emulator's instruction support, was overturned by the review and replaced with the kernel's SIMD-state gap. And tcc turned out to emit no symbol table at all, so the checker's symbol assertions apply only to the GNU-linked kernel, and the tcc profile pins the layout instead.

## What stays deferred, and why

The kernel enables no SIMD state while CPUID advertises the features the CPU has, so any userland program that dispatches on CPUID and executes a vector instruction dies with an invalid opcode, as accelerated busybox did. The fix is a real contract (enable `CR4.OSFXSR`, save and restore XMM state per process, or mask the features a boot cannot honor) and none of it is this milestone.

The GRUB half of the boot proof is implemented and bounded, and it was skipped on the machine this milestone was built on, where the Docker daemon was not running; the QEMU half booted every proof, and the harness names which of the two Docker conditions caused a skip. The GRUB path runs where `make os-real-grub-disk` already could.

The double-fault stub's shape (vector 8) cannot be exercised by the probe suite, since no probe raises one; its correctness rests on the phase-1 byte comparison against the macro-expanded original.

musl and dropbear are still not compiled in the guest, for milestone 20's reasons. Parallel builds still do not happen (pdpmake's `-j` is parsed and unimplemented upstream). tcc's `-Wall` is weaker than the host's `-Werror -Wextra` policy, so the host gate remains the correctness gate; the guest build is a capstone beside it, from the same sources.

## The gate

The whole gate ran in one pass from a tree `make distclean` had emptied, so musl, busybox, dropbear, tcc and pdpmake were all rebuilt from their tarballs: `make full`, then `verify-style` (its 12 numbered checks plus the tool preflight), `verify-host` (20 host seam tests), `test` (61 of 61 probes), `tcc-test`, `make-test`, `selfbuild-test`, `ssh-test`, `account-test`, `shutdown-test`, `login-test`, `ping-test`, `crash-test`, `test-disk`, `test-disk-ahci`, `persist-test`, `persist-test-ahci`, `cred-persist-test` and `test-noentropy`. Every step passed.
