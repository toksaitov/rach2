# PLAN.md — Port musl libc to Rach2, and build the OS subsystems it requires

**Status: candidate final draft (polished).** Principle throughout: **build it the
way real operating systems do, because the fidelity is the educational value.** The
plan ports **musl**, then **busybox** and **dropbear**, so Rach2 becomes a minimal
but real Unix you can SSH into — and builds the kernel subsystems that requires
(process ABI, TLS, virtual memory, a hierarchical filesystem, signals, terminals,
ptys), each in its simplest *real* form. Settled in this revision: KRFFS gains
**simple real directories** (Phase 6) rather than flat-namespace workarounds; and
the three biggest blockers (threads, dynamic linking, a memory-architecture
rewrite) are confirmed **off the path**. Read it, push back.

## What this is, and why

Today every Rach2 user program is *freestanding*: it hand-writes its own `int
0x80` stubs, its own `strlen`, its own typedefs, and links `-nostdlib`. That
bootstrapped the kernel, but it has hit its ceiling — no `printf`, no `malloc`, no
`FILE`, no `errno`, and every utility re-implements the same scaffolding. The next
step is a **real, standard C library**: we port **musl** and statically link the
utilities against it.

But "add musl" understates the work and undersells the value. **musl is a forcing
function.** A real libc cannot run until the kernel provides the foundational
services *every* modern OS has, and this plan builds each one **in the simplest
form that is still genuinely how real kernels do it**, as a teaching artifact:

- a **process/binary ABI** — argc/argv/env and an auxiliary vector on an aligned
  stack (the real SysV/i386 contract);
- **thread-local storage** — a `%gs`-based thread pointer via a GDT TLS
  descriptor, exactly as Linux manages i386 TLS;
- a **virtual-memory subsystem** — per-process **virtual memory areas (VMAs)**
  with **demand paging**, the data structure and mechanism real kernels are built
  on (stacks grow on fault, `mmap`/`brk` register regions, faults are dispatched
  by region, fork shares copy-on-write);
- the **`-errno` syscall contract** — negative errno from the kernel, exactly the
  Linux i386 ABI a libc expects;
- **signals** — disposition, masking, delivery on return-to-user, and a
  `sigreturn` trampoline, the real model.

The goal is **those subsystems, real and readable**; musl is the end-to-end test
that proves them. Rach2 teaches core OS concepts at university — a student should
open our VMA/demand-paging, TLS, or signal code and see *how a real kernel
supports a real libc*. "musl runs" is the test; "the subsystems are real, clear,
and correct" is the point.

### Why musl

Small, clean, static-link-friendly, famously readable — one concept per short
file. Its requirements map almost one-to-one onto teachable, real kernel
subsystems with none of glibc's sprawl. It speaks the Linux i386 ABI we already
use (`int 0x80`; number in EAX; args EBX/ECX/EDX/ESI/EDI; result in EAX), so the
*entry mechanism* is already compatible — the gap is which *services* exist.

### Scope — the end goal in view

The endpoint is a **minimal but real Unix**: an interactive **busybox** shell you
reach over the network by **SSH (dropbear)**. The work is three arcs:

- **Part I — musl + core kernel subsystems** (Phases 0-9, detailed below): static
  musl runs the utilities, atop the foundations done the real way — a real frame
  allocator over actual RAM, a VMA + demand-paging VM subsystem (incl. copy-on-
  write fork), Linux-style `%gs` TLS, the `-errno` contract, a **simple
  hierarchical filesystem** (KRFFS gains directories), a crypto-ready entropy
  source, and a real signal subsystem.
- **Part II-A — a real interactive shell (busybox)** (roadmapped below):
  poll/select, pipes, `termios` + a line discipline, and `/dev` device files.
- **Part II-B — remote login (dropbear)** (roadmapped below): pseudo-terminals,
  crypto-grade `/dev/urandom`, and the socket fill-out — then you `ssh` into Rach2.

The busybox+dropbear target is deliberately Rach2-shaped: both are **static,
fork-based, and low-memory** (the Alpine Linux combination), so — as the resolved
blockers at the end show — the end goal needs **no threads, no dynamic linker, and
no memory-architecture rewrite**. Part I is detailed; Part II is detailed when
reached.

This is **not** "minimal musl." The "simplest expedient" choices (eager mapping,
big static stack, partial errno) are rejected precisely because they are *not* how
real systems work and therefore teach the wrong thing.

---

## Ground rules (read every run)

1. **No git, ever.** No commits, merges, rebases, branches, or codemods. The user
   commits. Verify by building and running, never by VCS.
2. **The educational quality bar is the whole point.** Every new file is written
   to be *read by a student*: our terse ASCII `/* */` comments, `_t` typedefs,
   `_NAME_H_` guards, module-prefixed snake_case, declarations at top of block,
   4-space indent, Allman braces, C89 under `-ansi -Wall -Wextra` with zero
   warnings — for kernel code and every new musl-port subsystem (the pre-existing
   `utils/`/`ish/` were never built `-ansi`; they are relinked against musl in
   Phase 9, not retrofitted to C89). Prefer the **simplest version of the real design**,
   not the simplest design; comment the *why*; cite the real-OS analogue in the
   Readme so students can connect it to Linux/BSD. Each subsystem ships a short
   `Readme.md` in our voice explaining the concept and how to reason about it, and
   points at its probe test.
3. **Consistency with the existing tree.** Match the kernel style, the Readme
   style, the `build_*.sh`/Makefile style, and the `tests/`+`utils/` layout. Do
   not invent conventions; extend the ones that exist.
4. **Run continuously; stop only on a true blocker — and call Codex before you
   stop.** No pauses between phases. If you get genuinely stuck — a verification
   that will not pass, a subtle bug, a design you are unsure of — **hand it to
   Codex**: invoke the `codex:codex-rescue` subagent (an independent GPT‑5.5
   diagnosis / implementation pass via the installed Codex plugin) and work with
   its result. Halt and report to the user only if *that* also fails after genuine,
   repeated effort, or a decision arises this plan does not cover. Do not invent
   scope.
5. **Artifacts and the harness are ephemeral** — keep them in
   `/tmp/rach2_verify/` (the DNS-era `mk_image.sh` + `drive.py` harness lives
   there and is reused). The only repo files we add are real kernel sources, real
   libc glue, real programs, the Makefile/`build_*.sh`/Readme edits, and this plan.
6. **A phase is done only when its verification passes non-interactively *and* a
   harsh Codex review of its code has been run and its findings addressed.** Tick
   its checkbox and continue. Checkboxes are the only progress state, so the run
   resumes from the first unticked phase.

---

## The verification philosophy (how we steer)

The central lesson from the DNS port: **verify each subsystem hermetically,
in-guest, with a hand-written freestanding "probe" program, BEFORE musl depends on
it.** This de-risks the hard parts while we control every byte, gives a
deterministic offline checkpoint at every step, and the probes are themselves
teaching artifacts (a 40-line program that grows its own stack on a fault *is* the
demand-paging lesson). Each subsystem phase has:

- **Probe (hermetic):** a freestanding `tests/*probe.c` exercising the new facility
  via raw `int 0x80`, asserting, printing `All ... checks passed!`, exit 0. No
  musl, no network, deterministic.
- **Integration (milestone):** once the probes are green, a real **musl** binary
  that depends on the facility runs in the guest.
- **Codex review (per phase):** after the probe and integration pass, run a
  **harsh, independent Codex review** of the phase's code — the `codex:codex-rescue`
  subagent over the phase's diff, asked for an *adversarial* pass that challenges
  the design and hunts bugs, not a rubber stamp. Read every finding and **fix or
  adjust before ticking the phase**. A second GPT‑5.5 set of eyes on each subsystem
  catches what our own probes assume away — and "your code gets independently
  reviewed before it lands" is itself a good engineering habit to model.

Harness: `mk_image.sh <bins...>` builds a KRFFS image; `drive.py` boots
`qemu-system-i386 -kernel ... -serial stdio -display none`, waits for the
shell-ready marker, drives `exec <prog>` (re-sending on the known debug-shell
drop), captures serial; the caller greps for pass markers + `Process exited with
status 0`. The debug shell is flaky after ~4 sequential `exec`s per boot — **run
each probe as a first/near-first exec per boot**.

---

## Static analysis, formatting, and harness hardening (kept lightweight)

A small quality gate — **compiler warnings as the hard gate, formatting only on new
work, a static analyzer only where it is signal-heavy** — plus cheap harness
upgrades. Set up once in Phase 0 as `tools/check.sh` (a `make verify-style` target);
run it in each phase's **Verify** and the Phase 9 sweep, beside the probes and the
Codex review. Apply only to **in-house code** — exclude `ports/doom`, any `vendor/`,
and the imported `musl`/`busybox`/`dropbear` trees. Because we never touch git,
"the files a phase touched" is a manifest the run maintains at
`/tmp/rach2_verify/phase-files.txt`, and the per-phase Codex review diffs against a
**phase-start tree snapshot** (`diff -ruN`), not `git diff`.

**The gate:**
- **`-Werror` on the kernel build** — make the existing zero-warning
  `-ansi -Wall -Wextra` policy a hard *compiler* gate, not just a grep.
- **`clang-format` as a *check only* on the files a phase touched** (`--dry-run
  -Werror`; never reformat the tree). A `.clang-format` encodes the house style
  (LLVM base, 4-space, **Allman on functions / K&R on control**, no include sorting,
  pointers right, column 100).
- **`cppcheck --std=c89 --platform=unix32 --enable=warning,style`** scoped to the
  **kernel and the new `*probe` sources** — *not* the legacy `utils/`/`ish/`, which
  predate the C89 bar and aren't built `-ansi`, so a strict `--std=c89` pass
  false-positives there — with a `tools/cppcheck.suppress`. This is
  a deliberate, repo-specific **substitution for the conversation's `clang-tidy`**,
  not the same choice: on C89 freestanding i686, `clang-tidy`'s 64-bit-target
  analysis false-positives on exactly the scary kernel code, while scoped `cppcheck`
  is low-noise and needs no compilation database. We accept the trade — we give up
  `clang-tidy`'s cognitive-complexity / function-size gate and do **not** add
  `lizard` to win it back; the per-phase Codex review is what flags a monster
  function.
- **`shellcheck -e SC2086`** on the in-house shell scripts (the QEMU
  option-splitting is intentional).
- **Keep the convention grep** (ASCII-only, no `//`, `_NAME_H_` guards) — it catches
  what the tools cannot.
- **A `readelf` ABI check** on produced musl/probe binaries — ELF32 i386, `EXEC`,
  static (no `PT_INTERP`), entry in the expected range — cheap insurance the build
  stays on-ABI.

**Harness upgrades (cheap, high-value):**
- **Real pass/fail, not just serial greps.** Add QEMU's `isa-debug-exit` device,
  written by the **kernel's exit path in a harness/debug build** (a ring-3 probe
  cannot `out` to it) — `sys_exit(status)` encodes the status to the port so the
  harness reads a real exit code under a hard wall-clock timeout; a hang or
  triple-fault is then an unambiguous failure, not a missing marker.
- **The dual-target seam (host-side unit tests).** Pure, syscall-free logic (the DNS
  parser, VMA insert/split, path resolution, dirent parsing, the CSPRNG core) is
  written to compile **both freestanding (into the kernel) and hosted (into a native
  test binary)** — one `.c` file, two targets. The host build runs fast and
  deterministic beside the in-guest probe, **always under
  `-fsanitize=address,undefined`** — a near-free catch for the off-by-one / overflow
  / use-after-free bugs that are murder to see in-guest. (We already did this for the
  DNS parser.) The seam is itself a teaching point: students see why the logic is
  shaped to be testable off the metal.
- **A diagnostic mode for the hard bring-up (not a gate).** When a Phase 2 (TLS),
  Phase 3 (usercopy fault), or Phase 4 (paging/VMA) probe triple-faults or wedges,
  re-run QEMU with `-d int,cpu_reset -D qemu-trace.log -no-reboot -no-shutdown` to
  freeze the fault and read the interrupt/reset trace, and attach gdb via `-s -S`
  (the existing `debug.sh` already wires the gdbstub). Pure failure-analysis, run by
  hand when a probe is red — never part of the pass/fail sweep.
- **Don't let the loop cheat.** Under autonomous `/goal`, a probe/test is never
  weakened to make a phase pass; the no-git rule and the per-phase Codex review
  guard this — a red probe means fix the code, not the test.

**Deliberately *not* adopted (overkill for a teaching hobby OS now):**
**kernel-wide** sanitizers (KASAN, blanket kernel UBSan handlers — the host-test
ASan/UBSan above is the cheap 90%), a CI service, `libFuzzer`/fuzzing, formal
methods, CodeChecker/Coverity/Infer, `lizard`/`jscpd` metrics, a doc-site
generator, pre-commit frameworks, and any multi-agent "org chart" — the per-phase
adversarial review is already the process. (`clang-tidy` is addressed above;
`sparse` is shelved but with a live trigger that fires in Phase 3 — see there.)

---

## Target end-state architecture

```
toolchain (host):  the stock i686-elf-gcc builds musl itself (no cross-toolchain;
                   spiked and confirmed — see Phase 0).
libc/              pinned musl source (pinned tarball, no git submodule), build step, the
  Readme.md        link recipe, and how utils select musl vs. freestanding.
rach2/sys/
  mm/frame.{c,h}   NEW: real frame allocator sized from the multiboot memory map
  mm/vm.{c,h}      NEW: per-process VMAs + demand-paging fault dispatch + brk/mmap
  mm/paging.c      EDITED: user fault handler routes to vm.c (was a panic)
  proc/tls.{c,h}   NEW: GDT TLS descriptor + set_thread_area (Linux-style)
  proc/fork (cow)  EDITED: copy-on-write via frame refcounts + write-fault
  signal/signal.*  NEW: minimal real signal subsystem
  lib/krandom.*    NEW: a small real-shaped entropy/CSPRNG source
  errno.h          NEW: Linux i386 errno values for the -errno ABI
  fs/krffs.{c,h}   EDITED: simple hierarchical directories + path resolution
  proc/elf.c, syscall.c, isr_handler.s, switch.s, tables/gdt.* : EDITED
tests/*probe.c     NEW: one hermetic probe per subsystem
utils/             EDITED: link against musl, drop the freestanding stubs
  (Part II adds: fs/devfs, tty/termios, pty, and the poll/pipe syscalls.)
```

**Decisions (the real-OS lens):**

| Decision | Choice | Why (real + educational) |
|---|---|---|
| Linking | **static, non-PIE first** | a real, supported mode; dynamic linking is the documented next real step, loader kept dynamic-ready |
| Syscalls | **`int 0x80`** | the canonical, always-present i386 mechanism; vDSO is an optimization (deferred) |
| Error ABI | **`-errno` everywhere + real `errno.h`** | exactly the Linux kernel/libc contract; partial errno was rejected |
| Memory | **VMAs + demand paging** | the address-space model every real kernel uses; eager mapping was rejected |
| Frame pool | **real allocator from the multiboot map** | uses actual RAM (the kernel's own `ToDo`); 32 MB hardcode was a placeholder |
| TLS | **GDT TLS descriptor, rewrite-on-switch** | precisely how Linux manages i386 TLS |
| Stack | **grows on fault (a VMA)** | real demand-paged stack growth, the textbook example |
| fork | **copy-on-write** (frame refcounts) | the canonical fork optimization taught in every OS course |
| Entropy | **real-shaped CSPRNG/`getrandom`** | `AT_RANDOM` and `getrandom` from real entropy, not a fixed constant |
| Signals | **disposition + mask + delivery + sigreturn** | the real signal model (standard signals; RT-signals deferred) |
| Filesystem | **simple hierarchical KRFFS (real directories)** | the central FS concept (path resolution, the tree); honest limits kept at perms/size, not structure |

---

## Phases

> Each phase: **Goal · Concept taught · Design · Build · Verify (probe + musl) ·
> Codex review · Done when.** Dependency-ordered; the crux (TLS, Phase 2) is
> de-risked early and hermetically; the VM subsystem (Phase 4) is the centerpiece.
> After each phase, a harsh Codex review (the plugin) gates ticking its box.

**An adversarial Codex review of this plan (it read the real source) is folded into
the phases below.** Key corrections it surfaced: preserve a full trapframe (not just
`%ds`) and close the syscall `sti` windows (Phase 2); add
`copy_from_user`/`copy_to_user` + `-EFAULT` before demand paging (Phase 3/4); real
PTE permissions before COW (Phase 4/5); Linux wait-status encoding (Phase 5); a
KRFFS on-disk directory format updated in the **host tools** too, plus the
descriptor / open-file-description split (Phase 6); real scheduler **wait queues**
instead of `sti;hlt` (Phase 7); and **virtio-rng** entropy for dropbear (Part II-B).

### [x] Phase 0 — Research, decisions, toolchain, harness
**Goal:** kill every unknown that would stall a later phase; get a reproducible,
in-repo musl build.
**Concept taught:** what a libc is vs. what a kernel must provide; the SysV/i386
process ABI on paper.
**Do:**
- **Toolchain — RESOLVED (no cross-toolchain; spiked and confirmed).** Do **not**
  build a musl cross-toolchain. Build **musl itself** with the stock `i686-elf-gcc`
  students already install:
  `CROSS_COMPILE=i686-elf- ./configure --target=i686 --disable-shared && make`
  (~90 s) yields `libc.a` + `crt1.o/crti.o/crtn.o` and musl's headers. Confirmed:
  a `hello.c` using `malloc`/`snprintf` links to a **static i386 ELF, EXEC
  (non-PIE), entry in the 0x08048000 region, using `int 0x80`** — same shape as
  today's freestanding binaries, real libc behind it. Pin the musl version (spiked
  with 1.2.5); vendor it (pinned tarball, not a git submodule); add a `libc/` build step in
  `krffs/build_*.sh` style; encode the util link line (`... crt1.o crti.o prog.c
  libc.a $(i686-elf-gcc -print-libgcc-file-name) crtn.o -z noexecstack`). Document
  it in `libc/Readme.md`. Onramp stays "same compiler + one library build," and is
  *more* educational than a prebuilt toolchain.
- **musl startup spike (write it down).** The i386 static path
  (`crt1`→`__libc_start_main`→`__init_libc`→`__init_tls`(`set_thread_area`)→ctors
  →`main`→`exit`/`exit_group`) and the exact auxv it consults
  (`AT_PAGESZ/CLKTCK/PHDR/PHENT/PHNUM/ENTRY/RANDOM/SECURE/NULL`). Drives Phases 1-3.
- **Integration-layout + harness** decisions; confirm the tree builds clean and
  the existing test + net suites pass (baseline green).
- **Stand up the lightweight quality gate** (see the section above): `.clang-format`,
  `tools/cppcheck.suppress`, a `make verify-style` / `tools/check.sh` target, the
  `isa-debug-exit` harness hook, the host **dual-target seam** harness (native test
  builds under `-fsanitize=address,undefined`), and a `-Werror` kernel build —
  baseline-clean before Phase 1.
**Verify:** the toolchain produces a static i386 musl ELF (`file`/`readelf`) —
**already confirmed by the spike**; `build_all` clean; suites pass.
**Done when:** musl builds reproducibly in-repo, the startup/auxv contract is
written in `libc/Readme.md`, baseline green.

### [x] Phase 1 — Process image and the initial stack (argv, env, auxv, alignment)
**Goal:** hand a process a correct SysV/i386 stack: `argc`, `argv[]`, real
`envp[]`, a complete **auxv**, 16-byte aligned.
**Concept taught:** the process/binary ABI — where the kernel and the C runtime
meet on the stack; what the auxv is for.
**Design:**
- **Unify** the two divergent stack builders (`sys_execve`, `exec_program`) into
  one documented helper (they disagree today on alignment and string placement).
- Build, low→high: `argc`, `argv` ptrs, `NULL`, real `envp` ptrs, `NULL`, **auxv
  pairs** terminated by `AT_NULL`, then the string/aux-data area; 16-byte align the
  entry `esp`.
- Populate auxv from kernel facts: `AT_PAGESZ`=4096, `AT_CLKTCK`=100,
  `AT_PHDR/PHENT/PHNUM/ENTRY` (record in `elf_load`), `AT_SECURE`=0, and `AT_RANDOM`
  (16 bytes from `lib/krandom`). **Entropy tiering (important, resolves a
  cross-phase tension):** Part I `krandom`/`AT_RANDOM`/`getrandom` is **best-effort,
  non-security** randomness (`RDTSC`/timer/RTC mixing) — fine for a stack canary,
  *not* for keys; the **crypto-grade, fail-closed `/dev/urandom` + trusted
  `getrandom`** (seeded by virtio-rng) arrives only in **Part II-B**, before dropbear.
- Add **real `envp`** support to `execve` (today ignored); the boot-shell `exec`
  passes a small default environment.
- Give the process a **modest initial stack** (a few pages, eagerly mapped) and
  **record its bounds** (with the ELF segment bounds) in the PCB; Phase 4 turns that
  metadata into the stack VMA it grows on fault. *(VMA structures don't exist until
  Phase 4 — Phase 1 only records bounds, it does not build a VMA.)* Not a giant eager
  block.
- **Replace the whole-file ELF loader.** Both exec paths slurp the entire ELF into a
  512 KB static buffer and reject anything larger (`syscall.c`, `kernel.c`) — static
  busybox/dropbear exceed that. Move to a **segment loader**: read the ELF header +
  program headers, then load each `PT_LOAD` range from KRFFS into its eagerly-mapped
  pages (this also produces the `AT_PHDR`/`AT_PHNUM`/`AT_ENTRY` metadata for the
  auxv). *Nice-to-have:* make exec failure atomic — build the new address space and
  commit only on success, rather than freeing the old image first.
**Build:** the unified stack builder; the segment loader; `lib/krandom.{c,h}`;
`elf_load` records program headers; `auxv.h`.
**Verify (probe `tests/auxvprobe.c`, hermetic):** walk the stack from `_start`,
print `argc`, each `argv`/`envp`, and every auxv `{type,value}`; assert the
expected entries/values, env round-trips, vector is `AT_NULL`-terminated,
`AT_RANDOM` is non-constant across two runs.
**Done when:** the probe shows a correct, terminated auxv with real env and
non-constant `AT_RANDOM`, the stack is aligned, one builder serves both paths.

### [x] Phase 2 — Thread-local storage (the crux: Linux-style GDT TLS + `%gs` everywhere)
**Goal:** a `%gs`-based per-process thread pointer that **survives context
switches, IRQs, and syscalls** — musl puts `errno`, the stack canary, and (later)
`pthread_self` behind `%gs`.
**Concept taught:** segmentation as it survives in modern x86 — TLS via a GDT
descriptor, **mirroring Linux's i386 `set_thread_area` GDT-entry mechanism**, and
why entry/exit paths must preserve segment state.
**Design (from the investigation):**
- **GDT.** Enlarge `GDT_ENTRY_COUNT` (today exactly full at 6/6); add a TLS data
  descriptor slot + a runtime `gdt_set_tls(base, limit)` (the current setter is
  file-static). This is the real Linux layout (a dedicated TLS GDT entry).
- **`set_thread_area`(243).** Read the user `struct user_desc`, program the TLS
  descriptor (base=TCB, 4 GB limit, DPL 3, data R/W), return the entry number,
  write it back — the same syscall and `user_desc` ABI musl calls from
  `__init_tls`.
- **PCB + switch.** Store the per-process TLS base in `struct process`; in
  `switch.s` (which today touches *no* segment register) rewrite the shared TLS
  GDT descriptor from the incoming process and reload `%gs` — exactly how Linux
  reloads TLS entries on `__switch_to`. Do the descriptor rewrite + `%gs` reload
  with **interrupts disabled** (the switch can be entered from syscall context where
  IF=1), so a nested IRQ can't observe a half-updated TLS descriptor.
- **The critical fix — interrupt/syscall entry.** `isr_params_t` saves only `%ds`
  and the exit path smears it across `%es/%fs/%gs`; the first timer tick after musl
  sets its thread pointer would destroy it. Extend the saved frame to include
  `%gs` (and `%fs/%es`) and save/restore each individually on both stubs,
  **including the nested-IRQ window** (the syscall stub runs with interrupts
  enabled). Must be exactly right. Two more pieces the review flagged: **(a) close
  the `sti` windows** — the syscall stub does `sti` *before* `in_syscall` is set and
  again just before `iret`; set `in_syscall` before enabling interrupts and let
  `iret` restore IF (drop the pre-`iret` `sti`), so a timer IRQ can't preempt a
  half-built or half-restored frame; **(b) update every consumer of the frame
  layout** — the new `gs/fs/es` fields shift offsets, so `sys_fork`'s hand-spliced
  child frame and `isr_return` must match. And since `switch.s` hard-codes
  `struct process` offsets, **append** new PCB fields (TLS base here, VMAs in
  Phase 4, signal state in Phase 8) and re-check the asm offsets in the same patch —
  never insert mid-struct.
**Build:** `proc/tls.{c,h}`; PCB field; `switch.s`/`isr_handler.s` edits; a
`proc/tls` Readme citing the Linux analogue.
**Verify (probe `tests/tlsprobe.c`, hermetic — de-risks the crux):** call
`set_thread_area`, point `%gs` at a known TCB, write sentinels at `%gs:0`/offsets,
then **spin thousands of `int 0x80` + busy-wait through many timer IRQs**, re-read
each time, assert intact; a concurrent second process with a *different* TCB
proves per-process isolation across switches. Prints `All TLS checks passed!`.
**Done when:** the probe proves `%gs` TLS survives IRQs/syscalls/switches with
per-process isolation — without musl.

### [x] Phase 3 — The syscall contract (`-errno`) + MVP calls + first musl binary
**Goal:** make the kernel speak the real `-errno` ABI, add the calls musl needs to
reach `main`, and **run a static musl `hello` in the guest.**
**Concept taught:** the real kernel/libc error contract — negative errno from the
kernel, `errno` set by the libc.
**Design:**
- **`-errno` everywhere.** Add `errno.h` with the **Linux i386 errno values**.
  Dispatcher returns `-ENOSYS` for an empty/out-of-range slot (not `-1`). Sweep
  **all** existing handlers to return `-errno` on failure (a bare `-1` becomes
  `EPERM` and defeats musl's `ENOSYS` fallbacks). This is comprehensive, not
  "most-used" — the contract must be uniform to be real. **Update the existing
  freestanding utils/tests/stubs that compare exactly to `-1`** (e.g. `cat`/`ls`
  treat any non-`-1` fd as valid) to test `< 0` first, in the same change — else a
  `-ENOENT` reads back as a valid fd and they regress.
- **A user-copy / fault contract (prerequisite for Phase 4 demand paging).** Today
  syscalls dereference user pointers directly (`sys_write` reads `buf[i]`,
  `sys_execve` walks user strings) behind only an `access_ok` range check. Once user
  memory is demand-paged, a kernel touch of a not-yet-present user page faults *in
  kernel mode* — which must mean "fault it in, or return `-EFAULT`," never panic.
  Add `copy_from_user`/`copy_to_user`/`strncpy_from_user`/iovec helpers and route
  every pointer syscall through them. Then **annotate user pointers `__user`** (a
  no-op macro in normal builds) **and run `sparse` once over the syscall/usercopy
  sources** — a raw deref of a `__user` pointer becomes a sparse error, turning "we
  think every access is routed through the helpers" into a checkable fact. This is
  the one place the conversation's `sparse`/`__user` trigger genuinely fires, against
  a real Rach2 bug class (`sys_write`/`sys_execve` deref user memory directly today).
  A one-shot Phase-3 pass, not a standing gate.
- **MVP syscalls:** `writev`(146) (stdio/`printf`), `exit_group`(252) (musl's
  `exit` uses it), `set_tid_address`(258) (record+return tid),
  `ioctl(TCGETS/TIOCGWINSZ)`→`-ENOTTY` (so `isatty` picks buffering), `getpid`(20),
  `uname`(122).
- **musl link integration:** wire `libc/` so a program links `crt1/crti` + static
  `libc.a` + `crtn` via the cross compiler.
**Verify:**
- **Probe `tests/errnoprobe.c`:** an unimplemented number returns `-ENOSYS`; a
  deliberately failing call returns the right `-errno`.
- **Integration milestone:** a static **musl** `hello.c` runs in the guest, prints
  `hello`, kernel reports `Process exited with status 0`. *The* milestone — musl
  reaches `main`, does buffered stdio, exits cleanly.
**Done when:** the errno probe passes and musl `hello` runs in Rach2.

### [x] Phase 4 — Virtual memory: the address-space model (VMAs + demand paging)
**Goal:** the real VM subsystem — a per-process map of memory regions, frames
allocated lazily on fault, stacks that grow, `brk`/`mmap` as region operations,
and a fault that kills a *process* (not the machine). **The centerpiece.**
**Concept taught:** the core of virtual memory — the **VMA/address-space model**
and **demand paging**, the data structure and mechanism real kernels are built on.
**Design:**
- **Real frame allocator first (`mm/frame.{c,h}`).** Size the frame pool from the
  **multiboot memory map** (`mem_upper`/the mmap), replacing the
  `HARD_MEMORY_LIMIT` placeholder (the kernel's own `ToDo`). Document the honest
  remaining cap: the kernel can only directly touch identity-mapped low RAM, so
  total *resident* memory is bounded until a high-memory kernel split lands
  (a documented future effort — see *Resolved leftover blockers*). Within that, a
  real bitmap/stack allocator over actual RAM. **Critically, only hand out frames
  the kernel can actually touch:** the ELF loader and fork write user frames through
  their identity (physical) address, so **cap the allocatable pool to the
  direct-mapped window** — do not allocate high multiboot RAM until a `kmap` /
  high-memory split exists.
- **VMAs (`mm/vm.{c,h}`).** Per-process **small fixed array** of
  `{start, end, prot, type(code/data/heap/stack/anon)}` (simpler to teach than a
  list, and `MAX_PROCS` is 16) — *the* structure Linux/BSD use. `exec` sets up
  code/data/heap/stack VMAs; `mmap`/`munmap` insert/split; `brk` extends the heap
  VMA. Map pages with **real PTE permissions from the VMA `prot` / ELF `p_flags`**
  (text read+execute, data read+write) — `alloc_user_page` maps everything writable
  today and `elf_load` ignores `p_flags`, and COW (Phase 5) needs that fixed to tell
  a write-fault from a protection fault.
- **Demand-paging fault dispatcher.** Replace the panic-on-fault with: read CR2 +
  the error code; find the VMA containing CR2; **not-present in a valid VMA →
  allocate a frame and map it (demand paging)**; **fault just below the stack VMA,
  within the growth limit → extend the VMA and map (stack growth)**; **no VMA, or a
  protection violation → deliver a segfault (kill the process; wired to `SIGSEGV`
  in Phase 8)**; a genuine **kernel-mode** fault still panics. Handle the subtle
  case of a kernel-mode fault on a *user* address (the kernel touching the user
  stack on the process's behalf) by checking CR2 against the user range, not CS.
- **`brk`(45), `mmap2`(192), `munmap`(91)** implemented as VMA operations over the
  demand-paged backing. Anonymous `MAP_PRIVATE|MAP_ANONYMOUS` first; file-backed
  mmap is a documented follow-on.
**Build:** `mm/frame.{c,h}`, `mm/vm.{c,h}`, the fault dispatcher in `paging.c`,
PCB VMA-list head, the syscalls; an `mm/vm` Readme teaching VMAs + demand paging
with the Linux analogue.
**Verify (probe `tests/vmprobe.c`, hermetic):** `brk`-grow and r/w the heap;
`mmap` anon, write a pattern, read back, `munmap`; recurse to force **stack
growth** and confirm it succeeds; (separate boot) deref a wild pointer and confirm
**the process dies but the kernel survives** and the shell returns; confirm a
lazily-faulted page reads back zero. **Integration:** a malloc/`qsort`-heavy musl
program runs and exits 0.
**Done when:** VMAs + demand paging + stack growth work under the probe, faults are
per-process, and a malloc-using musl program runs.

### [x] Phase 5 — Copy-on-write fork
**Goal:** make `fork` copy-on-write instead of deep-copying, and add `wait4` to
complete the process model. COW is the canonical fork optimization *and* genuinely
pays off for the end goal — the shell and sshd `fork`-then-`exec` constantly, and
deep-copying pages that `exec` immediately discards is pure waste.
**Concept taught:** copy-on-write, frame reference counting, and the write-fault.
**Design:** upgrade the frame allocator from a used-*bit* to a **refcount** (per
the investigation, `_frames[]` is a bitmap today). `fork` copies the VMA list and
marks both parent's and child's pages **read-only**, bumping each frame's
refcount; a **write fault** on a shared page copies it (if refcount > 1),
remaps it writable, and drops the refcount — otherwise just re-grants write. Frees
decrement refcounts. Reserve a **software PTE bit for COW**, rely on the Phase-4
write-protect (so a write-fault is distinguishable from a protection fault), and
**flush the parent's TLB** after write-protecting its pages. Wire into the Phase 4
fault dispatcher. Add `wait4`(114) (musl's `wait`/`waitpid` issue it; the shell/sshd
reaper) beside `waitpid`(7), and fix the **wait-status encoding**: `waitpid` writes
the raw child exit code today, but musl/busybox expect the Linux encoding
(`WIFEXITED`/`WEXITSTATUS`, signal deaths) — encode it in both calls (zeroed
`rusage` if requested) and update the raw probes to match.
**Verify (probe `tests/cowprobe.c`, hermetic):** `fork`; both sides read the same
data (shared); each writes its own pages and confirms isolation; verify (via a
debug count or `/proc`-style hook) that frames were shared then split only on
write; many forks do not exhaust frames. **Integration:** a musl program that
`fork`s, both sides `printf` distinct state, parent reaps via the new `wait4` — at
minimum the fork+COW path is exercised.
**Done when:** the COW probe shows share-then-split with no leak, and fork works
under musl.

### [ ] Phase 6 — Files and directories (a hierarchical KRFFS + the `*at` API)
**Goal:** give KRFFS **simple real directories** and add the file-API syscalls
musl's `fopen`/`stat`/`opendir`/`mkdir` need — so file utilities and a navigable
tree work, and the existing `mkdir`/`rm -r` utils finally do something real.
**Concept taught:** the filesystem tree — directory entries and **path
resolution** — and the modern `*at` file API a libc builds `FILE*`/`DIR*` on.
**Design:**
- **Simple hierarchical KRFFS (extend it *in place*, not a new FS).** A directory
  node type whose entries map `name → node`; **path resolution** that splits a path
  on `/` and walks components from the root; `mkdir`(39)/`rmdir`(40). Route
  `open`/`stat`/`unlink`/`rename`/`chdir`/`getdents64` through the walk (today they
  flat-match a single name). Now `/etc`, `/tmp`, `/home`, `/bin` are real
  directories. This is a **KRFFS v2 on-disk format** (node headers gain a
  `parent_id`, or a directory node carries dirents) — and the **same format must be
  taught to the host tools** (`mkfs.krffs`, the FUSE mount that today *rejects*
  directories, `fsck`/`defrag`, and the `build_*.sh` stagers) so images can actually
  contain `/etc`, `/bin`, `/dev`. Kernel-side path-splitting alone cannot stage a
  tree.
- **Split the fd layer (needed for `O_CLOEXEC`/`fcntl`, and by pipes/ptys later).**
  Today `fd_entry_t` *is* the open object; `dup2` copies it by value and `fork`
  copies the whole table, so they don't share file offsets the Unix way and there is
  nowhere to hang per-descriptor `FD_CLOEXEC`. Introduce the real two-level model — a
  **descriptor table** (per-descriptor flags incl. close-on-exec) pointing at
  **refcounted open-file descriptions** (shared offset + status flags); `dup`/`fork`
  share the description, `execve` closes `FD_CLOEXEC` descriptors. This is the
  foundation pipes, ptys, and sockets build on in Part II.
- **Deliberate, documented simplifications** — the honest hobby-OS line drawn at
  the *right* place (not at structure): no users/permissions/ownership (`chmod`/
  `chown` are no-ops), small fixed limits, linear directory scans (no hashing /
  B-trees), no hard links or symlink chains, no journaling. These say "educational"
  without amputating the core *tree* concept.
- **The `*at` file API:** `openat`(295) (resolve relative to `AT_FDCWD`/a dirfd),
  `fstat64`(197) + `fstatat64`(300), `fcntl64`(221)
  (`F_GETFL/SETFL/SETFD/DUPFD(_CLOEXEC)`), `_llseek`(140) (64-bit); honest
  `O_CLOEXEC`/`O_NONBLOCK`.
**Build:** `fs/krffs.{c,h}` directory support + path resolution; the new syscalls;
an updated `fs` Readme on the tree, path resolution, and the simplifications.
**Verify (probe `tests/fileprobe.c`, hermetic):** `mkdir` nested directories,
`openat`/`fstat`/`read` a file inside one, `chdir` into a subdir + `getcwd`,
`getdents` lists a subdir, `fcntl` round-trips, `_llseek` seeks, `rmdir` removes an
empty dir. **Integration:** musl `cat <dir>/<file>`, `ls <dir>`, `mkdir`/`stat`,
and the existing `mkdir`/`rm -r` utils work on real subdirs.
**Done when:** the probe passes, real directories work, and musl file utils
navigate the tree in-guest.

### [ ] Phase 7 — Time, identity, and entropy
**Goal:** monotonic/real time, real sleep, identity, and `getrandom`.
**Concept taught:** time from a tick source; blocking sleep via the scheduler; a
kernel entropy source.
**Design:** introduce **real scheduler wait queues** first — today a blocking call
parks inside the syscall with `sti; hlt` and the timer skips preemption while
`in_syscall` is set, so a sleeping/blocked task wrongly stalls the whole system. Add
a wait-queue + timer-sleeper mechanism where a blocked task yields and other
processes keep running; this is the substrate `nanosleep` — and later `poll`, pipes,
ttys, and sockets — all need. Then: `clock_gettime`(265) for
`CLOCK_MONOTONIC`/`REALTIME` (document the 10 ms tick resolution); `nanosleep`(162)
as a real **block-and-schedule** sleep on that substrate; `getrandom`(355) backed by
`lib/krandom` — a real CSPRNG (ChaCha-style) seeded from **`RDRAND`/`RDSEED` behind a
CPUID check**, with the honest caveat that timer jitter alone is not defensible
entropy and the real source (**virtio-rng**) lands in Part II-B;
`getuid/geteuid/getgid/getegid`→0. (`getpid` already landed in Phase 3.)
**Verify (probe `tests/timeprobe.c`, hermetic):** `clock_gettime` monotonic;
`nanosleep(N)` waits ~N **and yields** (a second process makes progress meanwhile);
`getrandom` returns varying bytes. **Integration:** a musl program that timestamps,
sleeps, and seeds `arc4random` runs.
**Done when:** the time/entropy probe passes and a musl time/sleep program works.

### [ ] Phase 8 — Signals (minimal, but the real model)
**Goal:** a small, correct signal subsystem covering what `abort()`, faults, and a
shell + sshd need — synchronous `SIGSEGV`/`SIGFPE`, `SIGINT` from Ctrl-C, and the
process-control signals `SIGCHLD` (reap), `SIGPIPE` (broken pipe), and
`SIGTERM`/`SIGHUP` (teardown). **Job control** (`SIGTSTP`/`SIGCONT`, foreground
process groups, the controlling tty) is deferred to **Part II-A**, where sessions
and termios exist to make it meaningful.
**Concept taught:** signal disposition, masking, **delivery on return-to-user**,
the signal frame, and the `sigreturn` trampoline — the real mechanism.
**Design:** per-process pending/blocked masks + disposition table;
`rt_sigaction`(174), `rt_sigprocmask`(175), `rt_sigreturn`(173),
`kill`(37)/`tgkill`(270); **default actions** (term/core/ignore/stop); **delivery**
when returning to user mode (push a signal frame on the user stack, enter the
handler, `sigreturn` restores). Wire the Phase 4 fault path to raise `SIGSEGV`;
generate `SIGCHLD` on child exit (the shell's reaper); deliver `SIGINT` from Ctrl-C
to the **current foreground process** (no process groups yet). Phase 8 builds only
the signal *mechanics* — disposition, masks, delivery, `sigreturn`, defaults; the
pieces that need II-A primitives — `SIGPIPE` (needs pipes), and process-group /
`SIGTSTP`/`SIGCONT` / controlling-tty routing — land in **Part II-A** with pipes and
termios. Standard signals only (no RT queueing — documented).
**Verify (probe `tests/sigprobe.c`, hermetic):** install a `SIGSEGV`/`SIGUSR1`
handler, raise it, confirm it runs and execution resumes; `sigprocmask`
blocks/unblocks a pending signal in order; default-terminate works.
**Integration:** musl `abort()` terminates correctly; a handler via musl
`sigaction()` fires; Ctrl-C interrupts a running musl program.
**Done when:** the signal probe passes and musl `abort`/handlers/Ctrl-C work.

### [ ] Phase 9 — Adopt musl for the utilities, regress, document
**Goal:** convert the real programs to musl, prove nothing regressed, write the
docs that make all of this teachable.
**Do:**
- Convert `utils/` (`true`/`false`/`echo`/`cat`/`ls`/… + the net utils
  `ping`/`nc`/`httpd` + `resolv.c`) to build/link against musl, dropping the
  freestanding `crt0`/stubs. Incremental; keep the freestanding build until each
  program is proven on musl. (`resolv.c` can later become a thin `getaddrinfo`
  shim — a documented follow-on from the DNS seam.)
- Update `utils/Makefile` (+ `.linux`), the `krffs/build_*.sh` staging, the
  per-area Readmes; add `libc/Readme.md` and the subsystem Readmes
  (`mm/frame`, `mm/vm`, `proc/tls`, `fs/krffs`, `signal`, `lib/krandom`) in our voice, each
  teaching the concept, citing the real-OS analogue, and pointing at its probe.
  Register the new components in the root `Readme.md` table.
**Verify:** `make clean && make` (kernel + libc + utils + tests) zero warnings;
one combined boot runs musl-linked utilities and **every subsystem probe** still
passes and the **existing** syscall + network suites pass unchanged; the full
quality gate (`make verify-style`: `-Werror` build, clang-format check, scoped
cppcheck, shellcheck, host seam tests under ASan/UBSan) plus the convention grep
(ASCII, no `//`, guards).
**Done when:** utilities run on musl, the whole suite is green from clean, every
new subsystem has a Readme + passing probe.

---

## Part II — busybox and dropbear: a real Unix you can log into

Part I (musl) is the foundation. The end goal is a **minimal but real Unix**: an
interactive **busybox** shell, reachable by **SSH (dropbear)** — the Alpine
combination. Re-evaluating the deferred list against *these two programs* promotes
several "someday" items to **required**, and confirms the scary ones stay off the
path. Detailed when reached; here is the roadmap and the *why*.

### Part II-A — a real interactive shell (busybox)

busybox's `ash` + applets need, beyond Part I:

- **[ ] I/O multiplexing — `poll`/`pselect6`/`ppoll`.** Required by most applets and
  anything juggling fds. *(The call musl's own resolver wanted, deferred then;
  busybox forces it.)* Over the fd layer + scheduler block/wake. Probe: poll two
  pipes with a timeout.
- **[ ] Pipes — `pipe`/`pipe2`.** Shell pipelines `a | b`: a kernel pipe object
  (ring buffer, two fds, blocking, `SIGPIPE`). Probe: fork, write/read across it.
- **[ ] Reaping — `wait4`(114)** (with the Phase 5 process model). The shell reaps
  children and reads exit status; pairs with `SIGCHLD`.
- **[ ] Terminal layer — `termios` + line discipline.** Raw vs. cooked, echo, line
  editing, `tcgetattr/tcsetattr` (ioctl `TCGETS/TCSETS`), `TIOCGWINSZ`. The console
  becomes a real tty — the heart of an interactive shell. Probe: toggle raw/cooked,
  check echo/erase.
- **[ ] Device files + `/dev` (a devfs).** The hierarchical FS (Phase 6) already
  hosts `/etc`, `/tmp`, `/home`; here a small **devfs** mounted at `/dev` adds the
  char devices a shell expects — `/dev/null`, `/dev/zero`, `/dev/full`, `/dev/tty`,
  `/dev/console` — routed to in-kernel device handlers. (The ptys in II-B extend
  this namespace with `/dev/pts`.)
- **[ ] Sessions + job control — `setsid`/`setpgid`/`getpgrp`, a controlling tty,
  and the `SIGTSTP`/`SIGCONT` handlers deferred from Phase 8.** Foreground/
  background jobs; Ctrl-C/Ctrl-Z to the right process group.
- **Milestone:** a static musl **busybox** runs as the shell — `ls`, `cat`,
  `grep`, pipelines, and job control work interactively.

### Part II-B — remote login (dropbear SSH)

dropbear (server + client, static, fork-per-connection — **no threads**) needs,
beyond II-A:

- **[ ] Pseudo-terminals — `/dev/ptmx` + `/dev/pts/N`.** The master/slave pair: the
  sshd child runs the shell on the slave (II-A termios), the server shuttles bytes
  between the socket and the master (`TIOCGPTN`/`TIOCSPTLCK`/`openpty`). The biggest
  II-B subsystem; builds on the II-A terminal layer. Probe: open ptmx, fork a child
  on the slave, round-trip bytes through the master.
- **[ ] Crypto-grade entropy — `/dev/urandom` + the Phase 1/7 CSPRNG, promoted.**
  dropbear generates host and session keys — **security-load-bearing**, so the
  CSPRNG must be seeded from a **QEMU `virtio-rng` device** (the real entropy source;
  `RDRAND` only behind a CPUID check, *never* timer jitter alone) and **fail closed**
  if no real source is present. Exposed as `/dev/urandom` + `getrandom`. Probe:
  statistical sanity + reseed.
- **[ ] Key + auth staging.** Stage `/etc/dropbear` host keys (host-generated or
  first-boot) and pick the auth policy — `/etc/passwd` + a fixed password, or
  `authorized_keys` — now that the Phase-6 directory tree can hold them.
- **[ ] Socket fill-out — `getsockname`/`getpeername`/`setsockopt`/`getsockopt`/
  `shutdown`.** dropbear sets `SO_REUSEADDR`/`TCP_NODELAY`, queries peers,
  half-closes. Small additions to the existing socketcall.
- **[ ] A few more syscalls** both touch: `dup`/`dup3`, `umask`,
  `access`/`faccessat`, `getrlimit`/`setrlimit`, `sysinfo`, `gethostname`/
  `sethostname`, `clock_nanosleep` — each tiny, with the `-errno` discipline.
- **Milestone (the capstone):** from the host, `ssh -p <hostfwd> root@localhost`
  into the guest; dropbear authenticates, allocates a pty, execs busybox `ash` — an
  **interactive shell over SSH**. A real, if tiny, Unix.

## Resolved leftover blockers — confirmed NOT needed even for busybox + dropbear

Evaluating against the end goal *resolves* the scary blockers by showing they are
not on the path:

- **Threads / `clone` / futex / `pthread` — not needed.** busybox and dropbear are
  **fork-based and static**; musl `fork` uses `SYS_fork`(2), which exists. The whole
  end goal needs no threading. (Threads stay a clean future lesson; Phases 2/4/5 lay
  the foundation.)
- **Dynamic linking — not needed.** Both build fully **static** (the Alpine
  pattern). The loader stays dynamic-ready for a future lesson.
- **High-memory kernel split / >32 MB resident — not forced.** dropbear and busybox
  are built for low-memory embedded systems (a dropbear session runs in a few MB);
  the 32 MB resident pool comfortably holds one SSH session + shell. *Constraint:*
  it caps **concurrent** sessions, not a single login. **So the end goal does not
  require touching the memory architecture** — the one genuinely large blocker stays
  a documented future effort.
- **vDSO/`sysenter`, real-time signals, file-backed executable paging** — extras,
  not on the path.

**Net:** the road to "SSH into a busybox shell" is long but every remaining step is
a **bounded, teachable subsystem** (poll, pipe, termios, pty, devfs, a CSPRNG,
socket fill-out — with the hierarchical filesystem already done in Part I). None of
the three big architectural blockers is required — a clean, low-risk roadmap.

## Part I execution risk (still live)

- **`%gs` preservation under the syscall `sti` window (Phase 2)** — not a "will it
  work" risk (the recipe is clear) but an "execute exactly" one; the TLS probe is
  built to hammer nested IRQs during syscalls. **Toolchain — RESOLVED** (build musl
  with the stock `i686-elf-gcc`; spike confirmed).

## Open research questions / spikes (resolve in the owning phase)

- Exact musl auxv dependency for a static, no-TLS hello (Phase 0/1).
- KRFFS v2 on-disk directory format (`parent_id` vs. a dirent node) — and threading
  the same change through the host tools (mkfs / FUSE / fsck / build scripts)
  (Phase 6).
- Fixed-limit budget for one SSH session: confirm `MAX_PROCS`(16), `MAX_FDS`(16),
  and the socket pool(16) suffice via a budget probe; raise them if not (before
  Part II).
- `/dev` devfs: how device nodes sit in the Phase-6 directory tree and route to
  in-kernel handlers (II-A).
- pty/termios: how much real line discipline to implement now vs. defer (II-A/B).

*(Settled by the Codex review: directories extend KRFFS in place; VMA = small fixed
array; entropy = virtio-rng, fail-closed; job control moves to Part II-A; `getpid`
lives only in Phase 3.)*

## Definition of done (the full arc)

- **Part I:** a pinned static musl runs Rach2's utilities (`printf`/`malloc`/
  `FILE*`/`errno`/`abort`) atop real, **probe-verified** kernel subsystems — SysV
  stack + auxv (real env, real-entropy `AT_RANDOM`), Linux-style `%gs` TLS, a real
  frame allocator + **VMAs + demand paging**, **copy-on-write fork**, a **simple
  hierarchical filesystem** (real directories), the `-errno` contract, and a real
  signal subsystem.
- **Part II-A:** a static musl **busybox** runs as an interactive shell — pipes,
  job control, termios, and `/dev`, navigating the Part-I directory tree.
- **Part II-B:** **dropbear** accepts an SSH connection and runs that shell over a
  pty with crypto-grade entropy — **you can `ssh` into Rach2.**
- Every subsystem ships a `Readme.md` (with its real-OS analogue) and a passing
  `tests/*probe`; the existing syscall + network suites never regress; the whole
  project builds clean under our conventions. All commits are the user's; this plan
  and the harness never touched git.
