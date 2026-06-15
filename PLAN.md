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

- **Part I — musl + core kernel subsystems** (Phases 0-11, **done**): static
  musl runs the utilities, atop the foundations done the real way — a real frame
  allocator over actual RAM, a VMA + demand-paging VM subsystem (incl. copy-on-
  write fork), Linux-style `%gs` TLS, the `-errno` contract, a **simple
  hierarchical filesystem** (KRFFS gains directories), a crypto-ready entropy
  source, a real signal subsystem, the TCP `send`/`recv` path, and refcounted
  open-file descriptions.
- **Part II-A — a real interactive shell (busybox)** (Phases 12-18, detailed below):
  the blocking/dispatch substrate, pipes, poll/select, `/dev` + a minimal `/proc`,
  `termios` + a line discipline, sessions + job control, then the busybox port.
- **Part II-B — remote login (dropbear)** (Phases 19-23, detailed below):
  pseudo-terminals, crypto-grade `/dev/urandom` (RDSEED/RDRAND fail-closed; virtio-rng
  optional), the socket fill-out, auth/key staging, then the dropbear port — and you
  `ssh` into Rach2.

The busybox+dropbear target is deliberately Rach2-shaped: both are **static,
fork-based, and low-memory** (the Alpine Linux combination), so — as the resolved
blockers at the end show — the end goal needs **no threads, no dynamic linker, and
no memory-architecture rewrite**. Part I and Part II are both detailed below to the
same depth.

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

### [x] Phase 6 — Files and directories (a hierarchical KRFFS + the `*at` API)
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

### [x] Phase 7 — Time, identity, and entropy
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

### [x] Phase 8 — Signals (minimal, but the real model)
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

### [x] Phase 9 — Adopt musl for the utilities, regress, document
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

### [x] Phase 10 — Finish the socket path: `send`/`recv` on connected TCP
**Goal:** make musl's `send()`/`recv()` work on a connected TCP socket so the musl
`httpd` actually serves requests — the regression the Phase 9 port introduced and
the head-to-head check missed.
**Concept taught:** the Linux **socketcall multiplexer**, and the ABI subtlety
that `send`/`recv` are *not* their own syscalls — musl compiles them to `sendto`/
`recvfrom` with a NULL address. A from-scratch stack that routes by *sub-call
number* instead of *socket type* silently pushes stream traffic down the datagram
path. "Adopt musl" is the forcing function that exposes it; the freestanding
`httpd` dodged it by calling `SC_SEND`/`SC_RECV` directly, and `nc` by using
`read`/`write`, which is why only the ported `httpd` broke.
**Design:** in `rach2/sys/syscall.c`, `do_send`(SC_SEND=9)/`do_recv`(SC_RECV=10)
route to `tcp_send`/`socket_tcp_recv`, but `do_sendto`(11)/`do_recvfrom`(12) route
to the UDP path (`udp_tx`/`socket_recv`). When the fd is a connected `SOCK_STREAM`
socket, route `sendto`/`recvfrom` to the TCP path exactly like `send`/`recv`
(POSIX: a null destination on a connected socket means `send`/`recv`). Leave the
`SOCK_DGRAM` path untouched; for stream `recvfrom` accept a NULL `sockaddr` (no
`-EFAULT`) and skip the source-address writeback.
**Verify (probe `tests/musl/tcpsendrecv.c` + live `httpd`):** a new musl program
connects out through the SLIRP gateway `10.0.2.2` to a host echo server and
round-trips a line using `send()`/`recv()` (**not** `read`/`write`), printing a
marker; and the **musl** `httpd`, run under QEMU `hostfwd`, serves a real `curl` —
HTTP 200, non-empty body, a `"GET / HTTP/1.1" 200` access-log line (the
freestanding `httpd` already proves the kernel TCP works, so this isolates the
send/recv routing). `ping`/`nc`/`dnstest` unchanged; `make verify-style`/
`verify-host`. **Then a harsh, independent Codex review** of exactly the
`do_sendto`/`do_recvfrom` change — it touches the untrusted socket path, so
scrutinize the NULL-`sockaddr` handling, the buffer/length bounds, and that the
`SOCK_DGRAM` path is byte-for-byte unchanged — and fix **every** finding.
**Done when:** musl `send()`/`recv()` round-trip over TCP, the musl `httpd` serves
`curl`, the rest of the suite is unchanged, and Codex's findings are addressed.

### [x] Phase 11 — Refcounted open-file descriptions (`dup`/`fork` share the offset)
**Goal:** give file descriptors real POSIX semantics — a `dup`'d or `fork`'d fd
shares one **open-file description** (and its seek offset) — closing the Phase 6
deferral.
**Concept taught:** the **three-layer fd model** every Unix has: the per-process
*fd table* → a shared, refcounted *open-file description* (offset + status flags) →
the underlying *file/inode/socket*. Today Rach2 collapses the first two, so
`dup2(fd,1)` then `printf` and the original `fd` disagree about the offset, and
`lseek` on one does not move the other. This is *the* structure that makes shell
redirection, pipes, and `fork`+`exec` behave; Linux calls it `struct file`.
**Design:** add a refcounted `struct open_file` (the OFD) from a fixed static pool:
`{ refcount; offset; status_flags (O_APPEND / access mode); backing (KRFFS node id
/ socket* / device kind) }`. The fd-table entry becomes `{ fd_flags (FD_CLOEXEC —
**per-fd**, not per-OFD); struct open_file *ofd }`. `open`/`creat`/`openat`
allocate an OFD (refcount 1); `dup`/`dup2` copy the pointer + `ref++` and clear the
new fd's `FD_CLOEXEC`; `fork` copies each entry's pointer + `ref++` (parent and
child share the offset); `close` does `ref--` and frees at zero (releasing the
backing socket via its own unref); `read`/`write`/`lseek`/`getdents` use the OFD's
shared offset and honor `O_APPEND`; `execve` closes the `FD_CLOEXEC` fds;
`fd_init_stdio` builds the stdin/stdout/stderr OFDs. Refactor `fs/fd.{c,h}`
(`fd_get`/`fd_alloc`/`fd_table_init`); `alloc_process` `k_memset`s the fd table so
OFD pointers start NULL.
**Verify (probe `tests/ofdprobe.c`, hermetic + a musl check):** prove (a) `dup`/
`dup2` **share** the offset — read N bytes via one fd, then read via the dup and
confirm it continues from N (not 0); (b) `fork` **shares** the offset — the child
advances it and exits, the parent `waitpid`s and its next read sees the advance;
(c) closing one fd leaves the dup working, and many open/close cycles leak no OFDs;
(d) `execve` still closes `O_CLOEXEC` fds. Plus a musl integration check: `dup2` a
file over `stdout`, `printf`, confirm the shared offset advanced. The **whole**
suite + utilities stay green from a clean `make clean && make` (zero warnings, one
combined boot). Update `fs/Readme.md` to document the OFD layer and drop the
Part-II deferral note. `make verify-style`/`verify-host`. **Then a harsh,
independent Codex review** of the fd-layer refactor — scrutinize the refcount
lifecycle (no leak, no double-free, the socket-unref-on-last-close interplay), the
`fork`/`execve`/`dup2` paths, and the PCB layout — and fix **every** finding.
**Done when:** `dup`/`dup2`/`fork` share the seek offset, the OFD pool is
leak-free, the whole suite is green from clean, the OFD layer is documented, and
Codex's findings are addressed.

---

## Part II — busybox and dropbear: a real Unix you can log into

Part I (musl) is the foundation. The end goal is a **minimal but real Unix**: an
interactive **busybox** shell, reachable by **SSH (dropbear)** — the Alpine
combination. Part II is now **detailed to the same depth as Part I**: each item below
is a full phase (Goal · Concept · Design · Build · Verify (probe + integration) ·
Codex review · Done when), dependency-ordered, numbered **12-23** continuing Part I,
and it inherits every Ground rule, the verification philosophy, and the quality gate
above unchanged. The arc is two halves: **II-A (12-18)** turns the console into a real
interactive shell; **II-B (19-23)** lets you SSH in. wait4(114) and the refcounted
open-file descriptions already landed in Part I (Phases 5/11), so they are
prerequisites met, not phases here.

**This Part II was itself hardened by an adversarial Codex pass over the real source
and the vendored musl headers** (it found six BLOCKERs in the first draft). The
corrections are folded in below; the ones worth stating up front, as Part I lessons
sharpened:

- **ABI numbers/structs are pinned against the *vendored* `libc/musl-1.2.5`, not from
  memory.** Part I's `httpd` regression (musl compiling `send`/`recv` into
  `sendto`/`recvfrom`, us routing by sub-call number) is the cautionary tale; the
  first draft of *this* plan repeated it three times. Confirmed from the headers:
  `getpgid` is **132** (not 155); musl i386 defines the whole **select family**
  `select(82)`/`_newselect(142)`/`pselect6(308)` plus `poll(168)`/`ppoll(309)` — so we
  implement the family routed to one core and let the real busybox binary pick, never
  a hand-guessed single number; socketcall sub-call **15 is already our private
  `SC_PING`** but Linux 15 is `getsockopt`, so the private ping must be renumbered
  before the socket fill-out; `struct termios` in musl userspace is **NCCS=32 + speeds**
  (the kernel `TCGETS` 19-`c_cc` struct is a layout-compatible *prefix*), and pty
  ioctls are exact bit patterns (`TIOCGPTN=0x80045430`, `TIOCSPTLCK=0x40045431`).
  **The musl-1.2 64-bit-`time_t` transition matters on i386:** musl issues the
  `*_time64` variant *first* — `clock_gettime64`(403), `clock_nanosleep_time64`(407),
  `pselect6_time64`(413), `ppoll_time64`(414) — and falls back to the time32 number
  (265/267/308/309) on `-ENOSYS`. Part I already leans on that fallback (it implements
  `clock_gettime`(265)/`nanosleep`(162) and lets musl down-convert), so Part II's
  poll/select and `clock_nanosleep` either implement the time32 number and rely on the
  same fallback or implement `*_time64` directly — but must **pick deliberately and
  prove it against the real binary**, never assume the bare number. Every
  ABI-crossing phase adds a **by-number/by-offset ABI probe** and proves it against the
  real binary.
- **A generic backing-dispatch + a real multi-waiter wait queue do not exist yet — so
  they are Phase 12, built *first*.** Part I's `struct open_file` carries only
  `{refcount, offset}`, fd dispatch is still an `enum` switch, and a blocking socket
  parks a **single** `waiting_proc` with `sti; hlt`. Pipes, poll, ttys, ptys, and
  signal-interruptible sockets all need a real `wait_queue` (many waiters, lost-wakeup-
  safe) and a `file_ops`-style backing table before they can be built.
- **The OFD layer is the per-object substrate.** Each new object (pipe, tty, pty,
  accepted socket) is a backing kind hung off the refcounted `struct open_file`;
  `fork`/`dup`/`close` refcounting already works, so each only implements its
  read/write/poll/close ops — *and* `O_NONBLOCK` must become a real OFD status flag
  that is **enforced** (today `F_SETFL` only honors `O_APPEND`), and **`close(0/1/2)`
  must be made legal** (a daemon/shell closes and reopens stdio; the kernel bans
  `fd<3` today).
- **New PCB fields are append-only** (`switch.s` hard-codes offsets — the Phase-2
  lesson): pgid/sid/ctty and the new `PROC_STOPPED` state (Phase 17) **append** and
  re-check the asm offsets in the same patch.
- **Entropy: the required path is RDSEED/RDRAND, fail-closed — virtio-rng is the
  optional paravirt upgrade, off the critical path.** The kernel already CPUID-gates
  RDSEED/RDRAND (Phase 7); a from-scratch virtio-pci driver is educationally nice but
  unrelated to busybox/dropbear and could sink the run, so it is split out as an
  explicit stretch (Phase 20-opt). New PCI devices share IRQs — now safe via this
  session's chained-IRQ dispatcher — and every new IRQ handler still checks its own
  device first.
- **Pure logic uses the dual-target host seam** (pipe ring buffer, N_TTY cook/erase/echo
  state machine, `fd_set`↔`pollfd` translation, virtqueue index math) — compiled both
  into the kernel and into a native ASan/UBSan test, the cheap catch for the off-by-ones
  that are murder to see in-guest.
- **The interactive + SSH harness drivers live *in-tree* (`tools/`), not in `/tmp`,**
  and are **raw-byte with hard failures** — the tracked `tools/harness.py` force-appends
  a newline and *ignores* a failed `expect=`, which cannot validate Ctrl-C/Ctrl-Z, raw
  mode, backspace echo, an `ssh` password prompt, or a PTY session. Part II adds a
  raw-byte expect driver (control chars, no forced newline, assert-on-miss) and a host
  `ssh`-through-`hostfwd` driver, both committed.
- **Re-`make clean` after any `.h` edit** (no header-dependency tracking — a Part-I
  footgun), and **respect the fixed limits** (`MAX_PROCS`/`MAX_FDS`/socket pool = 16):
  static accounting up front, then a **live** budget probe once ptys exist (after Phase
  19), raising the limits (append-only PCB, re-check asm offsets) before dropbear if one
  session does not fit.

### Part II-A — a real interactive shell (busybox)

### [x] Phase 12 — Blocking & dispatch substrate (wait queues + the file-backing layer)
**Goal:** build the two primitives every later II-A/II-B object needs and that Part I
does **not** have yet — a real multi-waiter **wait queue** and a **file-backing dispatch
table** — and fix the two fd-layer gaps (`O_NONBLOCK`, `close(0/1/2)`) that block the
shell. No new userspace feature; the foundation pipes/poll/tty/pty/sockets all stand on.
**Concept taught:** how a kernel sleeps and wakes the *right* set of waiters without
lost wakeups, and how one `read`/`write`/`poll`/`close` entry point dispatches to many
object types (the `file_operations` indirection real kernels are built on).
**Design (grounded in the source):** today `struct open_file` carries only
`{refcount, offset}` ([fd.h](rach2/sys/fs/fd.h)), fd handling is an `enum`-switch, and a
blocking socket parks a **single** `waiting_proc` with `sti; hlt`
([socket.c](rach2/sys/network/socket/socket.c)) — none of that scales to pipes/poll.
Build (a) a `wait_queue` (a small fixed waiter list + `wq_wait`/`wq_wake_all`) on the
Phase-7 sleeper substrate, lost-wakeup-safe (check-condition-under-no-preempt, then
sleep), **signal-interruptible** (`-EINTR`); (b) a `struct file_ops`
(`read`/`write`/`poll`/`ioctl`/`close`) and a backing-kind tag on the OFD, replacing the
`enum`-switch with a dispatch the new objects register into; (c) add **`O_NONBLOCK`** as
a real OFD status flag honored by `F_SETFL`/`F_GETFL` (today only `O_APPEND` is —
[syscall.c](rach2/sys/syscall.c)); (d) **make `close(0/1/2)` legal** (drop the `fd<3`
ban — a shell/daemon closes and reopens stdio); (e) migrate the socket single-waiter
park onto the wait queue so socket blocking becomes `EINTR`/`EAGAIN`-correct. The wait
queue + dispatch are written on the **host seam**.
**Build:** `sys/proc/wait_queue.{c,h}`, the `file_ops`/backing-tag in `fs/fd.{c,h}`, the
`F_SETFL`/`O_NONBLOCK` + `close` fixes, the socket-wait migration; a `proc/wait_queue`
Readme citing the Linux analogue.
**Verify (probe `tests/waitprobe.c`, hermetic):** two processes block on one wait queue,
a third wakes **both** (no lost wakeup, both run); a blocked wait returns `-EINTR` when a
signal arrives; `O_NONBLOCK` flips via `F_SETFL` and is read back by `F_GETFL`;
`close(1)` then re-`open` onto fd 1 succeeds (stdio is closable). **Host seam:** the
wait-queue + dispatch under ASan/UBSan. **Integration:** an existing musl net test still
blocks/wakes correctly through the migrated socket path (no regression). **Then a harsh,
independent Codex review** of the lost-wakeup race, the `EINTR` path, and the dispatch
refactor — fix every finding.
**Done when:** multi-waiter wake + `EINTR` + enforced `O_NONBLOCK` + closable stdio all
pass, sockets ride the new wait queue with no net regression, and Codex is addressed.

### [x] Phase 13 — Pipes (`pipe`/`pipe2`)
**Goal:** a real kernel pipe — a bounded FIFO that blocks, signals EOF on writer-close,
and raises `SIGPIPE`/`EPIPE` on reader-close. Shell pipelines `a | b` stand on it.
**Concept taught:** the pipe as the original Unix IPC primitive — a ring buffer with two
refcounted descriptors, reader/writer **wait queues** (Phase 12), and the EOF/`SIGPIPE`
end-of-stream contract.
**Design:** a `struct pipe` from a fixed pool — a one-page ring buffer, read/write end
counts, reader/writer `wait_queue`s (12), registered through the Phase-12 `file_ops` as
a pipe backing on the OFD layer (`fork`/`dup` already refcount). `pipe`(42)/`pipe2`(331)
honoring `O_CLOEXEC`/`O_NONBLOCK` (enforced per 12). `read` blocks while empty unless all
write ends are closed (→ `0`, EOF); `write` blocks while full; a write with no readers
raises `SIGPIPE` + `-EPIPE`; `O_NONBLOCK` → `-EAGAIN`. Implement the pipe's `poll` op
(consumed by 14) and `FIONREAD`. The ring-buffer logic is on the **host seam**.
**Build:** `sys/ipc/pipe.{c,h}` + the `file_ops` registration, the syscalls; an
`ipc/pipe` Readme.
**Verify (probe `tests/pipeprobe.c`, hermetic):** `fork`; parent writes, child reads,
bytes + order match; fill the pipe so the writer **blocks** until the reader drains;
close the read end → write gets `SIGPIPE`/`-EPIPE`; close the write end → read gets `0`;
`O_NONBLOCK` write on a full pipe → `-EAGAIN`. **Host seam:** the ring buffer under
ASan. **Integration:** a musl `pipe`+`fork` round-trip. **Then a harsh, independent
Codex review** of the lifecycle (no pool leak, last-close ordering, the SIGPIPE/EOF
edges).
**Done when:** the probe shows blocking + EOF + `SIGPIPE` + non-blocking, a musl
fork+pipe round-trips, the pool is leak-free, and Codex is addressed.

### [x] Phase 14 — I/O multiplexing (the `select`/`poll` family)
**Goal:** wait on many descriptors with a timeout — the call busybox's `ash` loop and
most applets depend on.
**Concept taught:** readiness-based multiplexing — each pollable object answers "which of
`{readable, writable, error/hup}` now" and offers a wait queue (12); the syscall scans,
and if nothing is ready, blocks on all of them until a wakeup or timeout.
**ABI (Part-I lesson applied — confirmed from the vendored musl headers):** i386 musl
defines the **whole family** — `select(82)`, `_newselect(142)`, `poll(168)`,
`pselect6(308)`, `ppoll(309)`, plus the `time64` variants `pselect6_time64(413)`/
`ppoll_time64(414)` musl tries first — so we **implement them all routed to one core**
(the `time64` ones down-converting the `timespec`, or relying on musl's `-ENOSYS`
fallback to the time32 number) and let the real busybox binary pick, rather than betting
on one (the first draft wrongly assumed `pselect6`). `fd_set` is musl's `FD_SETSIZE=1024`
shape while `MAX_FDS=16`, so the translation scans only the first `MAX_FDS` bits: a large
`nfds` with **no** bit set past `MAX_FDS` is fine (we simply have no descriptors up
there), but a bit set for an fd ≥ `MAX_FDS` is an invalid descriptor → `-EBADF` (the real
select/poll contract) — never a silent truncation.
**Design:** a per-backing `poll(events) -> revents` + register-on-wait-queue op for pipes
(13), tty (16), sockets (now on the wait queue, 12), regular files (always ready), devfs
char devices (15). The poll core scans (fast path), else sleeps on every referenced wait
queue until a wakeup or timeout, then re-scans (lost-wakeup-safe per 12). `_newselect`/
`select`/`pselect6` translate `fd_set`↔pollfd (the `p*` variants do the atomic sigmask
swap honestly); document the 10 ms tick resolution. The `fd_set`↔pollfd translation is on
the **host seam**.
**Build:** `sys/io/poll.{c,h}`, the per-backing poll ops + a number-by-number ABI probe;
a Readme on readiness + wait queues.
**Verify (probe `tests/pollprobe.c`, hermetic):** two pipes (13); poll with nothing
ready → `0` on timeout (~the requested time elapsed); write one → `POLLIN`; close a write
end → `POLLHUP`; the same via `select`/`_newselect` `fd_set`; a large `nfds` with only
in-range bits works, while a bit set for an fd ≥ `MAX_FDS` returns `-EBADF` (no silent
truncation). **Host seam:** the set↔pollfd translation under ASan.
**Integration:** a musl program polling `stdin` + a pipe; an **ABI-by-number probe**
logs which select/poll number the real binary emits. **Then a harsh, independent Codex
review** of the wait/rescan race and the sigmask handling.
**Done when:** the probe passes (timeout, readable, `POLLHUP`, the select family), a musl
poll/select program works, the lost-wakeup race is closed, and Codex is addressed.

### [x] Phase 15 — Synthetic filesystems: a `/dev` devfs and a minimal `/proc`
**Goal:** real device files (`/dev`) and the slice of `/proc` that musl + busybox
actually need — so the tty, the shell, and (in II-B) `ttyname` work.
**Concept taught:** the synthetic filesystem — names backed by in-kernel drivers, not
storage: a char device dispatches `open` to a `struct device_ops`, and `/proc` exposes
kernel/process state as files. The seam between the namespace and driver code.
**Design:** a **devfs** integrated with the Phase-6 KRFFS path resolution (a `/dev` whose
children resolve to registered `device_ops`; pick + document a device-node kind carrying
`{major,minor}` vs. a mounted devfs). Core char devices: `/dev/null` (read→EOF,
write→sink), `/dev/zero` (zeros), `/dev/full` (write→`-ENOSPC`), `/dev/console` +
`/dev/tty` (the console, a full tty in 16), `/dev/random`+`/dev/urandom` (best-effort
now, crypto in Phase 20), routed through the Phase-12 `file_ops`. **A minimal `/proc`**
(Codex BLOCKER — busybox `ps` and musl `ttyname_r`/`realpath` read it): `/proc/self/fd/N`
(symlink-ish to the open file's name — what musl's `ttyname_r` uses), `/proc/PID/stat`
+ `/proc/PID/cmdline` (enough for `ps`), `/proc/self` → the caller's PID. Stage `/dev`
into the image (host tool + `build_*.sh`); `/proc` is purely synthetic.
**Build:** `sys/fs/devfs.{c,h}` + `device_ops`, `sys/fs/procfs.{c,h}` (the minimal set);
path-resolution/open wiring; image staging; a `fs/devfs` + `fs/procfs` Readme.
**Verify (probe `tests/devprobe.c`, hermetic):** `/dev/zero`→zeros, `/dev/null`→EOF/sink,
`/dev/full`→`-ENOSPC`, `/dev/urandom` varies; `/proc/self/fd/0` resolves to the console;
`/proc/<pid>/stat` reports the live process. **Integration (a self-contained musl
`devtest`, since pipes/busybox do not exist yet):** it reads N bytes of `/dev/zero` and
checks they are zero, reads `/dev/null` for EOF, writes `/dev/full` for `-ENOSPC`, and
calls `ttyname(0)` → a `/dev` name (exercising `/proc/self/fd`). **Then a harsh,
independent Codex review** of the device dispatch + the `/proc` synthesis.
**Done when:** the core char devices behave, the minimal `/proc` answers `ttyname_r`/`ps`,
and musl uses both; Codex is addressed.

### [x] Phase 16 — The TTY subsystem: `termios` + the N_TTY line discipline
**Goal:** turn the console into a **real terminal** — cooked vs. raw input, echo, line
editing, control chars — controlled via `termios`. The heart of an interactive shell,
and the layer ptys reuse.
**Concept taught:** the tty/line-discipline model — the canonical **N_TTY** discipline
(cooked line buffering with ERASE/KILL editing + ECHO; `ISIG` turning INTR/QUIT/SUSP into
signals; OPOST/ONLCR output), and `termios` as its control surface.
**ABI (pinned against vendored musl, Codex MAJOR):** musl's *userspace* `struct termios`
is **`NCCS=32` + `__c_ispeed/__c_ospeed`**; the kernel `TCGETS`(0x5401)/`TCSETS`(0x5402)
struct is the **19-`c_cc` prefix** (`c_iflag/oflag/cflag/lflag`, `c_line`, `c_cc[19]`),
which musl's larger struct is layout-compatible with (speed lives in `c_cflag`). Add a
**compile-time `offsetof`/`sizeof` ABI assert** generated from the vendored headers and
prove the real busybox `tcgetattr`/`tcsetattr` round-trips — do not hand-guess the
layout.
**Design:** a generic `struct tty` = input queue + line-discipline state (`termios`) +
output processing + reader `wait_queue` (12) + a foreground-pgrp hook (filled in 17). The
**N_TTY discipline** is pure logic on the **host seam**: `ICANON` line-assembly with
ERASE/WERASE/KILL + `ECHO`/`ECHOE`, `ICRNL`/`INLCR`/`IGNCR`, `OPOST`/`ONLCR`, and
`VMIN`/`VTIME` for raw mode; `ISIG` recognizes INTR/QUIT/SUSP (delivery to the fg group
lands in 17). Retrofit the **console** (PS/2 keyboard + serial in, VGA + serial out) onto
a `struct tty` registered as `/dev/console` + `/dev/tty` (15); `read(0)` flows through the
discipline; implement `poll`/`FIONREAD`. ioctls: `TCGETS`/`TCSETS`/`TCSETSW`/`TCSETSF`,
`TIOCGWINSZ`/`TIOCSWINSZ`, `TCFLSH` (pgrp/ctty ioctls in 17).
**Build:** `sys/tty/tty.{c,h}` + `sys/tty/n_tty.{c,h}` (dual-target), the console
retrofit, the ioctls + the termios ABI assert; a `tty` Readme.
**Verify (probe `tests/ttyprobe.c`, harness-driven over serial — raw-byte in-tree
driver):** cooked mode — the driver sends `a`,`b`,BS,BS,`c`,`d`,NL → `read` returns
`"cd\n"` with the right echo; raw mode (`VMIN=1`, no `ICANON`/`ECHO`) → bytes un-echoed,
one at a time; `TCGETS`/`TCSETS` round-trip a known `termios`; `TIOCGWINSZ` sane. **Host
seam:** the cook/erase/echo state machine under ASan. **Integration:** the classic musl
`cfmakeraw`+`tcsetattr`+per-byte-`read` "getch" loop works; cooked line edit echoes
correctly. **Then a harsh, independent Codex review** of the discipline edges
(buffer-full, erase at line start, EOF, CR-NL) and the console retrofit.
**Done when:** cooked + raw line discipline works on the console, `termios` round-trips
through real musl (ABI assert green), the discipline passes its host tests, and Codex is
addressed.

### [x] Phase 17 — Sessions, process groups, and job control (the highest-risk phase)
**Goal:** the session/process-group model and full job control — Ctrl-C/Ctrl-Z reach the
**right** group, jobs stop and resume, and the shell learns when a child stops.
**Concept taught:** the Unix session→process-group→process hierarchy, the controlling
terminal and its **foreground group**, foreground/background signal routing, and the
stop/continue mechanism (the classic APUE chapter).
**Codex flagged this as the single most likely phase to sink the run** (today the kernel
classifies stop signals but **ignores** them — [syscall.c](rach2/sys/syscall.c) — has no
`PROC_STOPPED` state, and `wait4` only reaps zombies), so it is built in five explicit
sub-steps, each probe-verified before the next: **(a)** `setsid`(66)/`setpgid`(57)/
**`getpgid`(132)** [not 155]/`getpgrp`(65)/`getsid`(147) + the append-only PCB `sid`/`pgid`
(re-check `switch.s` offsets); **(b)** the controlling-tty ioctls
`TIOCSCTTY`/`TIOCGPGRP`/`TIOCSPGRP`/`TIOCNOTTY` and `/dev/tty` → the caller's ctty;
**(c)** a real **`PROC_STOPPED`** state + `SIGSTOP`/`SIGTSTP` stop and `SIGCONT` resume,
with `SIGCHLD` to the parent on stop/continue; **(d)** `wait4` `WUNTRACED`/`WCONTINUED`
and `kill(-pgid, sig)` (negative-pid → whole group) + `rt_sigsuspend`(179)/`pause`(29)
for the shell's wait loop; **(e)** wire the line discipline (16) to deliver INTR→`SIGINT`/
QUIT→`SIGQUIT`/SUSP→`SIGTSTP` to the **foreground group**, background read→`SIGTTIN`,
`TOSTOP` write→`SIGTTOU`, and **`SIGPIPE`** (now that pipes exist — deferred from Phase 8).
**Build:** the PCB fields, the session/pgrp syscalls, the ctty ioctls, the `PROC_STOPPED`
state + `wait4` flags + `kill(-pgid)` + `rt_sigsuspend`/`pause`, the signal-routing edits;
a Readme on sessions + job control.
**Verify (probe `tests/jobprobe.c`, harness-driven raw-byte over serial):** `fork` a
child into its own pgrp as the tty's fg group; Ctrl-C → **only** the fg child gets
`SIGINT`; a background child reading the tty gets `SIGTTIN`; Ctrl-Z **stops** the fg
child and the parent's `wait4(WUNTRACED)` reports it; `SIGCONT` resumes (parent sees
`WCONTINUED`); `kill(-pgid, SIGTERM)` hits the whole group; `setsid`/`setpgid`/`getpgid`
round-trip; `pause` returns on a signal. **Integration:** a musl job-control sketch.
**Then a harsh, independent Codex review** of the signal-routing matrix, the stop/cont
state machine, and the `wait4` encoding — fix every finding.
**Done when:** all five sub-steps pass their probes (pgrp/session, ctty, stop/cont,
`wait4`/`kill(-pgid)`, fg-group routing), and Codex is addressed.

### [x] Phase 18 — BusyBox-required syscalls + the BusyBox port (the II-A capstone)
**Goal:** a static musl **busybox** runs as the interactive login shell on the console
tty.
**Concept taught:** that the preceding subsystems compose into a *real shell* — and the
discipline of porting a large real program (curate a config, surface and fill the gaps
honestly) rather than writing a toy.
**Design:** first land the **busybox-required misc syscalls** Codex flagged as scheduled
too late — `getppid`(64), `times`(43), `sysinfo`(116), `umask`(60), `access`(33)/
`faccessat`(307), `vfork`(190) (a conservative alias to `fork`, or force the config off
it), `FIONBIO`/`FIONREAD` for every backing, and a decision on `statfs64`(268)/`readlink`
(implement minimal, or disable `df`/`readlink`/`realpath` applets and document) — each
with the `-errno` discipline. Then pin + vendor busybox (tarball, no submodule); a
`busybox/` build step (static against musl, `i686-elf-gcc`); a curated `.config` (ash +
`ls`/`cat`/`echo`/`grep`/`sort`/`head`/`tail`/`wc`/`mkdir`/`rm`/`cp`/`mv`/`kill`/`sleep`/
`ps`/…) with applets needing unimplemented facilities **disabled and documented**, and a
**file-backed-`mmap` pre-flight**: confirm the static config triggers no `mmap` of a file
(Rach2 returns `-ENOSYS` for non-anon `mmap` — [syscall.c](rach2/sys/syscall.c)) and no
mapped zoneinfo (set `TZ`/UTC). Stage busybox + a minimal `/etc` into the **runtime**
image; the console launches busybox `ash` (`ish` stays a fallback/init).
**Build:** the misc syscalls, the `busybox/` build step + `.config` + staging; a
`busybox/Readme.md` on the config + disabled-applet rationale + the `getcwd` `CWD_MAX=64`
cap.
**Verify (interactive, the in-tree raw-byte driver over serial):** busybox `ash` prompts;
`ls /`, `cat /etc/...`, `echo hi`, a pipeline `ls / | grep etc`, redirection `echo hi >
/tmp/x; cat /tmp/x`, job control (`sleep 5 &`, `jobs`, `fg`, Ctrl-Z/Ctrl-C), `ps`, and
`busybox --list` all behave. The **entire** Part-I suite + every probe green from `make
clean && make`; `make verify-style`/`verify-host`. **Then a harsh, independent Codex
review** of the misc syscalls + the shell-launch glue.
**Done when:** busybox `ash` runs interactively with pipelines, redirection, job control,
and `ps` on the console; nothing regressed; Codex is addressed.

### Part II-B — remote login (dropbear SSH)

> **Budget — static first, live after ptys.** Up front, do *static accounting*: one SSH
> session ≈ dropbear (1 proc) + its forked session child (1) + the shell — and fds:
> listen socket + accepted socket + pty master + pty slave + stdio. Against
> `MAX_PROCS`/`MAX_FDS`/socket pool = 16 this should fit but is tight. The **live**
> budget probe (stand the whole stack up and count) needs a pty pair, so it runs **after
> Phase 19**, gating Phase 23; raise the fixed limits (append-only PCB, re-check asm
> offsets) then if it does not fit.

### [x] Phase 19 — Pseudo-terminals (`/dev/ptmx` + `/dev/pts/N`)
**Goal:** the pty master/slave pair — the mechanism a network login uses to give a
remote user a real terminal. The biggest II-B subsystem, built on the Phase-16 tty layer.
**Concept taught:** the pseudo-terminal — a bidirectional channel whose **slave** end is
a full tty (reusing the N_TTY discipline) and whose **master** end is what sshd drives;
the dynamic `/dev/pts` namespace.
**Design:** a `struct pty` pool, each = a master endpoint + a slave `struct tty` (16).
`open("/dev/ptmx")` allocates a pair and returns the master fd; the slave appears as
`/dev/pts/N` (devfs, 15). Master write → slave input (line discipline) → slave `read`;
slave write → output processing → master `read`; both ends **pollable** (14) and
**blocking** (12). ioctls — **exact bit patterns pinned from the vendored musl headers**
(Codex MAJOR): `TIOCGPTN`=`0x80045430` (slave index), `TIOCSPTLCK`=`0x40045431` (unlock),
`TIOCSWINSZ` propagation, plus the ctty/pgrp ioctls (17) on the slave. Supports musl
`openpty`/`grantpt`/`unlockpt`/`ptsname` (which reads `/proc/self/fd` from 15). `/dev/pts/N`
appears/disappears on open/close. **Run the live budget probe here**, now the full
listen+accept+pty+child+stdio stack exists.
**Build:** `sys/tty/pty.{c,h}` reusing `n_tty`, the `/dev/ptmx` + `/dev/pts` devfs nodes,
the ioctls; a `tty/pty` Readme.
**Verify (probe `tests/ptyprobe.c`, hermetic):** `open` ptmx, `TIOCGPTN`→N (exact value),
`open` `/dev/pts/N`, `fork` a child that `setsid`+`TIOCSCTTY`s the slave as ctty/stdio;
the parent writes a line to the master → the child reads the **cooked** line and the echo
returns on the master; the child writes → the parent reads; `poll` works on the master;
the live budget probe fits one session. **Integration:** a musl `openpty`+`fork` program
round-trips — the exact shape sshd uses. **Then a harsh, independent Codex review** of
the master/slave wakeup paths, the pts lifecycle, and the ctty interplay.
**Done when:** ptmx/pts pairs work with the pinned ioctls, the slave is a full tty, musl
`openpty` round-trips, the one-session budget fits, and Codex is addressed.

### [x] Phase 20 — Trusted entropy: RDSEED/RDRAND → `/dev/urandom`, fail-closed
**Goal:** an entropy pipeline good enough to generate SSH keys — seeded from a genuine
hardware source, exposed as `/dev/urandom` + trusted `getrandom`, **fail-closed** when no
real source exists. **(Required path; virtio-rng is the optional stretch, 20-opt.)**
**Concept taught:** the entropy chain a CSPRNG must sit on, and the security rule a key
generator embodies — **refuse to run on non-defensible entropy** (timer jitter is not a
seed).
**Design (Codex MAJOR — virtio is gold-plating on the critical path):** the kernel
already CPUID-gates **`RDSEED`/`RDRAND`** seeding ([krandom.c](rach2/sys/lib/krandom.c)).
Add a **trusted-entropy flag**: seed the Phase-7 ChaCha CSPRNG from `RDSEED`/`RDRAND`,
mark the source trusted; if neither instruction is present, **fail closed** — `getrandom`
blocks/`-EIO` and `/dev/urandom` refuses, with a clear log (the weak jitter fallback is
**never** used for keys). Expose `/dev/urandom` + `/dev/random` (devfs, 15) and promote
`getrandom`(355) to crypto-trusted; `AT_RANDOM`/the stack canary stay best-effort (Phase
1) — document the tiering.
**Verify (probe `tests/rngprobe.c`, hermetic):** `/dev/urandom` reads vary, no long zero
runs, pass a cheap monobit/byte-distribution sanity; reseed changes the stream;
**(separate boot, CPU with no `RDRAND`)** the fail-closed policy holds — no silent weak
output. **Host seam:** the ChaCha core (dual-target since Phase 7) + the monobit check.
**Integration:** a musl `getrandom` + `/dev/urandom` consumer and a tiny keygen shape.
**Then a harsh, independent Codex review** (security-load-bearing — adversarial on the
seeding and the fail-closed path).
**Done when:** `RDSEED`/`RDRAND` seeds a crypto CSPRNG, `/dev/urandom` is non-trivially
random, fail-closed holds with no real source, and Codex is addressed.

**[ ] Phase 20-opt (stretch, off the critical path) — virtio-rng as a paravirtual entropy
source.** Purely educational (the modern VM device model): a minimal **virtio-pci**
transport — PCI discovery (`1af4:1005`/`1044`), one **virtqueue** (descriptor/avail/used
rings in direct-mapped low RAM — respect the Part-I frame cap), feature negotiation, kick
+ the used-ring ISR (a possibly-shared IRQ — safe via the chained dispatcher) — and a
`virtio_rng` driver that becomes a *preferred* trusted seed when present, RDSEED/RDRAND
still the fallback. `sys/virtio/virtio_{pci,rng}.{c,h}`; the virtqueue index math on the
host seam; a `virtio` Readme. Probe: `/dev/urandom` is virtio-seeded when the device is
attached. **Do this only if Phase 20 landed clean and time allows — it must not gate
dropbear.**

### [x] Phase 21 — The socket fill-out for a real server
**Goal:** the rest of the BSD socket surface a fork-per-connection sshd needs beyond Part
I's connect/send/recv — naming, options, half-close — and a hardened, interruptible
inbound path.
**Concept taught:** the server side of the socket API — `accept` minting a connected
socket, `getsockname`/`getpeername`, socket **options**, `shutdown` half-close — riding
the Phase-12 `file_ops`/wait-queue substrate over the Phase-11 refcounted OFDs, so a
forking server's fds block, poll, and refcount correctly.
**Design (Codex BLOCKER — a numbering collision):** Linux socketcall sub-call **15 is
`getsockopt`, but Rach2 already defines a private `SC_PING=15`**
([syscall.c](rach2/sys/syscall.c)) — **renumber the private ping out of the BSD range
first**, then add the real sub-calls **`getsockname`(6)/`getpeername`(7)/`shutdown`(13)/
`setsockopt`(14)/`getsockopt`(15)**. Honest `SO_REUSEADDR`/`TCP_NODELAY`/`SO_ERROR`/
`SO_KEEPALIVE` (real where cheap, documented no-op where not) + `FIONREAD`/`FIONBIO`. Make
`accept` return an **OFD-backed**, wait-queue-blocking (12), poll-able (14), `EINTR`-able
socket that inherits `O_NONBLOCK`, so `fork`-per-connection refcounts cleanly; confirm
`accept`/`poll` wake on an inbound SYN; `shutdown(SHUT_RDWR)`/`close` drive TCP teardown.
**Build:** the renumbered ping, the new sub-calls + a `get/setsockopt` table, the
accept/poll/EINTR hardening, `FIONREAD`/`FIONBIO`; a number-by-number socketcall ABI
probe; Readme additions.
**Verify (probe `tests/sockfilltest.c`, musl, via `hostfwd`):** a musl server
binds/listens, the host connects, `accept` returns; `getpeername`/`getsockname` correct;
`setsockopt`(`SO_REUSEADDR`/`TCP_NODELAY`)+`getsockopt` round-trip; `shutdown` half-closes
(peer sees EOF); a second `connect` after close works; a blocked `accept` returns `-EINTR`
on a signal. **Integration:** the Part-I musl `httpd` (unchanged) still serves `curl`
(no regression after the ping renumber); a fork-per-connection echo server handles two
host connections. **Then a harsh, independent Codex review** of the renumber, the option
table, and the accept/OFD/EINTR interplay.
**Done when:** the ping renumber is clean (`httpd` still serves), the server socket
surface is complete, options + `FIONREAD` round-trip, half-close + `EINTR` work,
`accept` is OFD-clean, and Codex is addressed.

### [x] Phase 22 — Auth, key staging, and the last small syscalls
**Goal:** stage the identity/credentials a login server needs, and clear the long tail of
tiny POSIX calls dropbear touches.
**Concept taught:** how a Unix authenticates a login (the `/etc/passwd` + keys staging)
and the honest line a credential-less hobby OS draws — plus the `-errno` discipline on
the last syscalls.
**Design:** stage **`/etc/passwd`** (root + a fixed password hash or a pubkey policy),
**`/etc/dropbear`** host keys (staged, or generated first-boot via Phase-20 entropy), and
**`authorized_keys`** in the Phase-6 tree; pick + document the auth policy (fixed password
vs. pubkey). No real users → `setuid`/`setgid`/`setgroups` accept 0 / are documented
no-ops; `getpwnam`/`getpwuid` work via `/etc/passwd` through musl. The tail syscalls, each
`-errno`: **`dup3`(330), `getrlimit`(76)/`ugetrlimit`(191)/`prlimit64`(340),
`gethostname`/`sethostname`(74), `clock_nanosleep`(267 — plus the `_time64`(407)
fallback per the ABI note)** (the busybox-shared ones —
`umask`/`access`/`sysinfo` — landed in Phase 18), with honest values (`RLIMIT_NOFILE`=
`MAX_FDS`, `RLIMIT_STACK`=the stack-VMA cap). **Advisory locks (Codex):** dropbear/musl
may issue `fcntl` `F_SETLK`/`F_GETLK` — implement **documented single-user no-conflict
semantics** (always grant) or disable the callers; do not silently `-ENOSYS` a path
dropbear needs.
**Build:** the staging (host tool + `build_*.sh`), the syscalls, the `F_SETLK` decision;
a Readme on the auth policy + the no-real-uid + no-conflict-locks simplifications.
**Verify (probe `tests/miscprobe.c`, hermetic):** each new syscall round-trips
(`getrlimit` honest caps; `dup3` dups + sets `CLOEXEC`; `gethostname` round-trips;
`clock_nanosleep` waits + yields; `F_SETLK` grants); musl `getpwnam("root")` parses
`/etc/passwd`. **Integration:** a musl program that `getpwnam`s root and reads its
rlimits. **Then a harsh, independent Codex review** of the staging + the tail syscalls.
**Done when:** identity/keys are staged, the auth policy is set, every tail syscall
round-trips under `-errno`, advisory locks have honest semantics, and Codex is addressed.

### [x] Phase 23 — Port dropbear; SSH into Rach2 (the capstone); regress
**Goal:** from the host, `ssh -p 2222 root@localhost` into the guest — dropbear
authenticates, allocates a pty (19), and execs busybox `ash` (18) over it with
crypto-grade entropy (20): **an interactive shell over SSH.** The whole arc's definition
of done.
**Concept taught:** that the subsystems compose into a *real, networked* Unix — and the
capstone discipline of porting a security daemon honestly.
**Design:** pin + vendor dropbear; build **`dropbearmulti`** static against musl (no
PAM/zlib; a KEX/cipher set that fits Rach2's crypto + Phase-20 entropy; curate
`options.h`); a `dropbear/` build step. Generate/stage host keys (22); run `dropbear -F
-E` (foreground, log to stderr) on port 22 with `hostfwd tcp:127.0.0.1:2222-:22`. Running
it surfaces the last gaps (a socket option, a pty ioctl, an rlimit) — fix with the
established discipline. The session: sshd `accept`s (21) → `fork`s → the child `setsid`s +
opens a pty slave as ctty (17/19) → execs busybox `ash` (18); the server shuttles
socket↔master.
**Build:** the `dropbear/` build step + `options.h` + key staging + the run wiring; a
`dropbear/Readme.md` on the config + the SSH-session anatomy.
**Verify (the capstone, the in-tree host `ssh` driver):** the driver runs a **real host
`ssh` client** through the `hostfwd` with the fixed credential, executes `uname -a; id;
ls /; echo SSH_OK`, and **hard-asserts** the output returns over the SSH channel; a second
run requests an **interactive PTY**, runs busybox `ash`, types a pipeline + Ctrl-C, and
asserts the right behavior. Host + session keys use Phase-20 entropy. The **entire** Part
I + Part II suite + every probe green from `make clean && make`; `make verify-style`/
`verify-host`. **Then a harsh, independent Codex review** of the integration glue, any new
syscalls, and the crypto/entropy wiring (adversarial — the security surface).
**Done when:** `ssh root@localhost -p 2222` yields an interactive busybox shell over a
dropbear-allocated pty with crypto-grade entropy, the full suite is green, and Codex is
addressed — **you can SSH into Rach2.**
**DONE:** real OpenSSH client → pubkey auth → busybox `ash` over a dropbear pty;
`pwd`/`whoami`/`ls`/`cat` work (rc=0). Run `exec dropbearmulti dropbear -F -R -p 22`
(NO `-E` — invalid without syslog; `-R` auto-generates the host key into `/etc/dropbear`
from Phase-20 entropy, so `-cpu max` is required). Gaps the real binary surfaced and
closed: `sin_family` byte order; `fsync`/`fdatasync` (RAM no-op); `link`→`-EPERM` (no
hard links → dropbear's direct-write fallback); `seteuid`/`setegid`/`setres*`/`setre*`
(root-only privilege drop+restore); `chmod`/`chown` family (the session pty). Staging
adds `/etc/shells` + `/etc/dropbear` + `/root/.ssh/authorized_keys` (demo key
auto-generated by `build_test_ramdisk.sh`). `permmusl` probe + `dropbear/Readme.md`.
verify-style/verify-host green; Codex SHIP (3 MEDIUM fixed). Follow-up: a plain
interactive `ssh root@localhost` (no command) needs `/dev/tty` to be the session's
own terminal (busybox ash's job-control init checks `tcgetpgrp`); `/dev/tty` now
resolves to the caller's controlling terminal (console or pty slave, else -ENXIO)
instead of always the console — this also removes the "Failed to disconnect from
controlling tty" warning.

## Part III — Polish the build: streamline, unify, modernize (the toolchain a student meets first)

Parts I and II built a real Unix you can `ssh` into. Part III makes the thing a
student **builds and edits** every day fast, obvious, and friendly. It is a
**polishing stage**, not new OS surface: the kernel and its binaries do not change
(objects stay byte-identical wherever a command line is intentionally unchanged);
only the *build system* does. The recurring Part-I/II footgun — **no
header-dependency tracking, so any `.h` edit needs `make clean && make`** — is the
headline; the rest removes the cruft a teaching repo accumulates: three different
dependency styles, cross-compiler detection copy-pasted in four Makefiles, a top
build that builds some components twice, three vendored third-party builds a student
must run *by hand* before `make` works, and no IDE language support for kernel hacking.

Part III inherits Part I/II's discipline unchanged. Each phase is **Goal · Concept ·
Design · Build · Verify · Codex review · Done when**, dependency-ordered, numbered
**24-30** continuing Part II. Per phase: build it; **prove it** — the build itself
plus a targeted demo, **and the invariant that guards every phase: `make
verify-style` and `make verify-host` stay green, a real in-guest boot still passes
(`tools/harness.py`, not just the static gates), and the dropbear SSH capstone still
reproduces end-to-end**; then a **harsh independent Codex review of exactly that
phase's build files with every finding fixed**; then a **one-line house-style commit,
no attribution**. Touch the relevant `Readme.md` in the same phase so docs never
contradict the new `make` flow.

**This Part III was jointly designed AND adversarially reviewed by Claude + Codex**
(the same pass Part II got; Codex's first read found ten issues — Phase 26 was too
broad, the third-party stamps under-specified, and the verification not mechanical
enough to protect the "still builds + boots + SSH reproduces" invariant — all folded
in below). The guiding value the two of us locked: for a *teaching* OS the **build is
curriculum** — a student must read and modify it with confidence, so **transparency
beats cleverness**. Decisions settled up front (do not relitigate during the run):

- **Stay on GNU Make.** No CMake/Meson/Bazel: a generator hides the freestanding
  compile + custom-`ld` link steps we *want* visible, and Make is the least-surprising
  common language for kernel + asm + FUSE host tools + QEMU scripts + vendored
  autotools/kbuild. Add **modern Make hygiene, not a framework** — and **keep commands
  visible** (no quiet/color macros that hide `i686-elf-gcc … -T link.ld`; a `V=`
  switch is a possible later nicety, not a headline).
- **Explicit kernel source list stays** — `find sys -name '*.c'` would silently link
  stray scratch files and make builds order-dependent. A `make check-sources` detector
  (Phase 29) catches a new `.c` not added to the **kernel** list and hard-fails the
  gate for it; it is advisory (not fatal) for Doom/vendored trees, where imported or
  unused `.c` files are intentional.
- **`compile_commands.json` is generated from Make's own source lists**, NOT `gcc -MJ`
  — verified: `i686-elf-gcc` (GCC 16.1) rejects `-MJ` (a clang-only flag).
- **Tiered builds:** plain `make` is the *base teaching OS* and stays fast; the slow
  capstones (busybox, dropbear, Doom) are opt-in (`make full`). `libc` is a real
  default prerequisite (utils hard-fail without it).
- **The SSH capstone rides the *test* ramdisk** (`krffs/build_test_ramdisk.sh`, which
  stages `/busybox`, `/sh`, `/dropbearmulti`, `/etc`, the demo key) — NOT the base
  `build_ramdisk.sh`. `make full` must produce *that* image, so the capstone is
  verified against what `make` actually emits.
- **Hybrid build dir:** final artifacts stay where the run scripts expect
  (`rach2/kernel`, `krffs/file_system.krffs`); only intermediates move out of tree
  (Phase 30, last and deferrable).
- **No `make run`.** The build system builds and supports build tasks; *running* the
  kernel has a real option matrix (disk/iso, audio device, `NET`/`NET_HOSTFWD`,
  `-cpu`, a future SSH boot) the `run*.sh` scripts already model — Make would only
  become a worse second copy. `make help` *points to* the run scripts.

### [x] Phase 24 — Shared cross-toolchain + automatic header-dependency tracking
**Goal:** end the `make clean && make` ritual — editing any kernel `.h` rebuilds
exactly the objects that include it, nothing more, nothing less; `make -j` is correct;
cross-compiler detection lives in one place, not copy-pasted.
**Concept taught:** automatic prerequisite generation (`gcc -MMD -MP` writes a `.d` of
each object's real header set, `-include`'d back) and DRY build config via a shared
`mk/` include — the modern-Make idiom every later phase reuses.
**Design:** `mk/toolchain.mk` detects the freestanding cross GCC (`i686-elf-gcc`, else
`x86_64-elf-gcc`) once and derives `CROSS_LD/AR/STRIP/OBJCOPY`; `mk/deps.mk` defines
`DEPFLAGS = -MMD -MP`. `rach2/Makefile` includes both, adds `$(DEPFLAGS)` to the C
compile, `-include $(C_OBJECTS:.o=.d)`, cleans the `.d`; `*.d` git-ignored. Asm (`.s`,
not preprocessed) needs none. The `.o` stay byte-identical (`-MMD` only adds sidecars).
*(Groundwork already in the working tree, uncommitted: Codex-review and commit it.)*
**Build:** `mk/toolchain.mk`, `mk/deps.mk`, the `rach2/Makefile` wiring, the gitignore.
**Verify:** clean build; **the demo — `touch sys/syscall.h`, `make` rebuilds only its
includers and relinks, leaving the other ~60 objects alone** (before, that edit rebuilt
nothing — the bug); `make verify-style`/`verify-host` green; **kernel binary unchanged
vs `HEAD`** (`cmp` the `.o`/kernel); a real boot still passes.
**Done when:** a kernel header edit triggers a correct partial rebuild with no
`make clean`; verify gates + boot green; Codex review of the Make logic addressed.

### [x] Phase 25 — One build idiom: fold every component Makefile onto `mk/`
**Goal:** kernel, tests, ish, utils, the Doom port, and the krffs host tools share
**one** transparent build idiom — no Makefile re-implements toolchain detection (was
4×) or header tracking (was three styles: krffs's verbose `-MMD`, ish's hand-listed
`$(HEADERS)`, none in tests/utils).
**Concept taught:** a small reusable rules layer that removes boilerplate without
hiding the command.
**Design:** `mk/rules.mk` provides the compile/link pattern rules + `-include
$(OBJ:.o=.d)`; each component Makefile shrinks to *flags + source list + `include
../mk/…`*. tests/ish/utils/doom gain header deps; the krffs host-tool Makefile (host
compiler, so `deps.mk` only) collapses its ~100 lines of per-target repetition.
Commands stay visible. **Byte-identity caveat:** turning a single-source `%.c ->
binary` rule into `%.o -> binary` *can* change final bytes, so either preserve the
exact compile/link shape for those, or scope the "unchanged" claim to artifacts whose
command line is intentionally identical — and check behavior, not just bytes, for the rest.
**Build:** `mk/rules.mk` + the five component-Makefile refactors + their Readmes.
**Verify:** every component builds; **touch a shared user-side header → only dependents
rebuild**; the musl utils build; **a real in-guest probe sweep + busybox boot via
`tools/harness.py`**; `make verify-style`/`verify-host` green; kernel `.o` byte-identical.
**Done when:** no component Makefile re-implements toolchain detection or header deps;
verify gates + boot green; Codex review addressed.

### [x] Phase 26 — `make doctor`: a friendly toolchain preflight (before any automation)
**Goal:** a student or a fresh box runs `make doctor` and gets a clear present/missing
report for every required tool with a per-tool install hint — *before* the new
third-party automation (Phase 27) can fail late on a missing `pkg-config`, FUSE,
`i686-elf-strip`, `ssh-keygen`, or `qemu-system-i386`.
**Concept taught:** environment preflight / actionable onboarding — the cross toolchain
is an OS course's single biggest first-day friction.
**Design:** `tools/doctor.sh` + a `make doctor` target check `i686-elf-gcc`/`-ld`/
`-strip`, `qemu-system-i386`, FUSE (fuse/fuse-t via `pkg-config`), `ssh-keygen`,
`cppcheck`, `shellcheck`, `clang-format`, host `cc`; print **ASCII** status
(`PASS`/`MISSING`/`OPTIONAL`, no Unicode — the convention gate is ASCII-only) with the
`brew`/`apt` line per miss; exit non-zero if a *required* tool is absent. The top
Makefile suggests `make doctor` when a build trips on a missing tool.
**Build:** `tools/doctor.sh`, the `doctor` target, the failure hint.
**Verify:** `make doctor` here reports all-present, exit 0; with a required tool hidden
from `PATH` it reports it MISSING + the hint and exits non-zero; `tools/doctor.sh` is
shellcheck-clean (rides `verify-style`).
**Done when:** `make doctor` accurately reports the toolchain with hints and the right
exit code; Codex review addressed.

### [x] Phase 27 — Top-level orchestration: tiered targets + stamp-wrapped third-party
**Goal:** a clean clone "just works" — `make` builds the base teaching OS **in order
with no manual steps** (no hand-running the three `build_*.sh`); `make full` adds the
slow capstones; `make help` lists everything and points at the `run*.sh` scripts.
**Concept taught:** **stamp files** for opaque/external builds (vendored
autotools/kbuild) and default-vs-extras build tiers.
**Design:** the top Makefile gains `deps` — `libc` built by default (utils need
`libc.a`), busybox + dropbear under `make full`/`DEPS=full` — each wrapped in a
**stamp file with its REAL inputs**: the `libc` stamp depends on the build script and
validates `install/lib/libc.a` + crt objects + headers before it is touched; the
`busybox` stamp on `busybox-*.tar.bz2` + `build_busybox.sh` + `rach2.config` +
`muslgcc` + the libc stamp; the `dropbear` stamp on its tarball + `build_dropbear.sh`
+ `localoptions.h` + `muslgcc` + the libc stamp. **`.DELETE_ON_ERROR`** so a failed
third-party build never leaves a valid-looking stamp. `make` = krffs, libc, kernel,
tests, ish, utils; `make full` = + busybox, dropbear, Doom. `make help` documents
targets; the per-component Readmes and the build-script headers stop telling students
to run `build_*.sh` by hand.
**Build:** the top Makefile, the three stamp wrappers, `make help`, the Readme/doc updates.
**Verify:** from a pristine state (stamps removed), `make` builds the base in order
with **no manual `build_*.sh`**; `make full` builds the capstones; a deliberately
corrupted third-party build leaves **no** stamp (re-runs next make); `make help` lists
targets; `make verify-style`/`verify-host` + boot green.
**Done when:** `make` from a clean tree produces the base OS with no manual steps,
tiers + stamps behave (incl. the `.DELETE_ON_ERROR` failure path); Codex review addressed.

### [x] Phase 28 — Ramdisk build/stage split + the capstone image + no double-build
**Goal:** Make **owns building**; the ramdisk scripts only **package**. `make ramdisk`
produces the base image and `make test-ramdisk` (a.k.a. the capstone image) the
busybox/dropbear/probe image — neither rebuilds components Make already built — and
`make full` ends at the exact image the SSH capstone logs into.
**Concept taught:** separation of *build* from *package*; making the capstone's real
artifact a first-class, reproducible target.
**Design:** `krffs/build_ramdisk.sh` and `krffs/build_test_ramdisk.sh` are split (or
`SKIP_BUILD=1`-guarded) so they stage prebuilt binaries instead of re-invoking
`make`/`ish`/`utils`/Doom internally (today's double-build at `build_ramdisk.sh`).
Top Makefile: `ramdisk` -> `file_system.krffs` (base); `test-ramdisk`/`capstone-ramdisk`
-> the staged image with `/busybox`, `/sh`, `/dropbearmulti`, `/etc`,
`/root/.ssh/authorized_keys` + the auto-generated demo key; `make full` depends on
`test-ramdisk`.
**Build:** the build/stage split of both ramdisk scripts + the new ramdisk targets +
the docs.
**Verify:** **no component is built twice** (grep the build log for a second
`ish`/`utils`/Doom compile); the base and capstone images build; **and the exact SSH
capstone, against the image `make full` produced:** boot `qemu-system-i386 -cpu max
-kernel rach2/kernel -initrd krffs/file_system.krffs` with `NET_HOSTFWD=hostfwd=
tcp::2222-:22`, guest `exec dropbearmulti dropbear -F -R -p 22`, host `ssh -i
krffs/rach2_demo_ed25519 -p 2222 -o StrictHostKeyChecking=no -o
UserKnownHostsFile=/dev/null root@127.0.0.1 'echo SSH_OK; whoami; uname -srm; ls /;
cat /etc/motd'` returns `SSH_OK`/`root` (rc 0); `make verify-style`/`verify-host` green.
**Done when:** the double-build is gone, the capstone image is a reproducible target,
and the real `ssh` login passes against `make full`'s output; Codex review addressed.

### [x] Phase 29 — IDE support: `compile_commands.json` + `.clangd`, and a source-list guard
**Goal:** clangd gives students real autocomplete, go-to-definition, and inline
diagnostics on the **freestanding** kernel; and a guard catches a new kernel `.c` not
added to `C_SRC`.
**Concept taught:** the LSP **compilation database** (generated from the build, since
GCC has no `-MJ`); explicit-source-list discipline backed by a detector.
**Design:** `tools/gen_compile_commands.sh` + `make compile-commands` read each
component's source list + flags (from Make) and write a repo-root
`compile_commands.json` (git-ignored, regenerable); a checked-in `.clangd` points
clangd at the cross GCC (`query-driver`/target) so it resolves `i686-elf-gcc`'s
freestanding headers, not the host's. `make check-sources` diffs the **kernel** `C_SRC`
against `find sys -name '*.c'` and **hard-fails** on an unlisted source (wired into the
gate); for Doom/vendored trees it is advisory only.
**Build:** the generator, `.clangd`, `make compile-commands`, `make check-sources`,
a short "IDE setup" Readme note.
**Verify:** `make compile-commands` writes a database whose kernel entries parse (a
`clang --syntax-only`/clangd check resolves a kernel TU's headers); `make check-sources`
flags a deliberately-unlisted scratch `.c`, then passes once removed; `make
verify-style`/`verify-host` green.
**Done when:** `compile_commands.json` regenerates via Make and clangd resolves kernel
symbols; `check-sources` catches an unlisted kernel source; Codex review addressed.

### [x] Phase 30 — Out-of-tree intermediates: a clean source tree (`.build/`)
**Goal:** the source tree stays clean — intermediates (`.o`, `.d`) live under `.build/`,
while final artifacts stay exactly where the run scripts expect (`rach2/kernel`,
`krffs/file_system.krffs`). `make clean` becomes `rm -rf .build` + the artifacts.
*(The one phase with real Make complexity and a moderate payoff; last and explicitly
deferrable — drop it if it costs more readability than the clean tree is worth.)*
**Concept taught:** out-of-source intermediates with in-place final artifacts (the
hybrid model), the path mechanics owned by `mk/rules.mk` so component Makefiles stay simple.
**Design:** `mk/rules.mk` gains a per-object `.build/<component>/…` output path and the
matching `-include`; final link/artifact targets unchanged so the run scripts, ISO, and
ramdisk steps are unaffected; `.build/` git-ignored. `compile_commands.json` paths
updated to match.
**Build:** the `.build/` relocation in `mk/rules.mk` + each component's object paths +
the compile-commands generator.
**Verify:** clean build; **no stray `.o`/`.d` anywhere in the source tree**; touch a
header → correct partial rebuild from `.build/`; `make clean` leaves a pristine tree;
the run scripts still find `rach2/kernel`; **a real boot + the SSH capstone reproduce**;
`make verify-style`/`verify-host` green.
**Done when:** intermediates are under `.build/`, the source tree is clean, final
artifacts + run scripts unaffected; verify gates + boot + SSH green; Codex review addressed.

**Part III done when:** a clean clone runs `make doctor` → `make full` and gets the
**entire OS including the dropbear SSH capstone** with no manual `build_*.sh` and no
`make clean` rituals; every component shares one transparent Make idiom; a header edit
rebuilds only its dependents; clangd works on the kernel; and `make verify-style`/
`verify-host`, a real boot, and the `ssh` login all still pass — **the build a student
meets first is now fast, obvious, and friendly.**

## Part II execution risk (live)

- **Job control (Phase 17) is the single highest-risk phase** (Codex's verdict, not
  dropbear): the kernel today *classifies* stop signals but ignores them, has no
  `PROC_STOPPED`, and `wait4` only reaps zombies — so it is split into five probe-gated
  sub-steps. "Exactly which process gets the signal" is the hard part; `jobprobe` drives
  Ctrl-C/Ctrl-Z over the raw-byte serial driver and asserts the recipient.
- **ABI fidelity is the recurring Part-I trap, now pinned from the vendored headers:**
  the select **family** (not `pselect6` alone), `getpgid`=132, the `SC_PING`=15
  collision, the `NCCS=32` termios prefix, the exact pty ioctl bit patterns — each gets a
  by-number/by-offset probe and is judged by the real busybox/dropbear binary.
- **The blocking/dispatch substrate (Phase 12) underpins everything** — if the wait
  queue has a lost-wakeup or the `file_ops` refactor regresses sockets, every later phase
  wobbles; its probe hammers multi-waiter wake + `EINTR` first.
- **Entropy is de-risked by making RDSEED/RDRAND the required path** (Phase 20) and
  **virtio-rng an explicit stretch** (20-opt) — the one genuinely new driver model can no
  longer sink the run. If a probe wedges anywhere, the Phase-0 diagnostic mode (`-d
  int,cpu_reset`, gdbstub) applies unchanged.
- **The one-session fixed-limit budget** is settled by a live probe after Phase 19 (ptys
  exist), raising `MAX_PROCS`/`MAX_FDS`/the socket pool (append-only PCB, re-check asm
  offsets) before dropbear if needed.

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

**Part I — all resolved** (the subsystems shipped): the musl auxv set (Phase 1), the
KRFFS v2 directory format threaded through the host tools (Phase 6), and the discovery
that surfaced *after* Part I — musl's `send`/`recv` → `sendto`/`recvfrom` routing
(Phase 10) and the IRQ-11 sharing between HDA and RTL8139 (the chained-IRQ dispatcher).

**Part II — open spikes, each owned by a phase:**
- Backing-dispatch + wait-queue shape: the `file_ops` table and the multi-waiter
  `wait_queue` that replace the fd `enum`-switch and the single-`waiting_proc` park
  (Phase 12) — the foundation everything else registers into.
- `/dev` devfs + `/proc` shape: a device-node kind in KRFFS carrying `{major,minor}` vs.
  a mounted devfs, and the minimal `/proc/self/fd` + `/proc/PID/stat` slice `ttyname_r`
  and `ps` need — pick the simplest real one (Phase 15).
- N_TTY scope: how much line discipline now (ERASE/KILL/ECHO/ISIG/OPOST + VMIN/VTIME)
  vs. deferred (IXON flow control, full `c_cc`) — settle in Phase 16 and reuse for ptys.
- ABI layouts pinned against the vendored `libc/musl-1.2.5`: the select **family**
  (`select`/`_newselect`/`poll`/`pselect6`/`ppoll`, implement all routed to one core —
  Phase 14), the `TCGETS` 19-`c_cc` termios prefix under musl's `NCCS=32` struct (Phase
  16), the exact pty ioctl bit patterns (Phase 19), and the `SC_PING`=15 renumber before
  the socketcall fill-out (Phase 21).
- Entropy: RDSEED/RDRAND fail-closed is the required source (Phase 20); virtio-pci +
  virtqueue is the optional paravirt upgrade, off the critical path (Phase 20-opt).
- Fixed-limit budget for one SSH session: static accounting up front, then a **live**
  probe after ptys exist (Phase 19) confirms `MAX_PROCS`/`MAX_FDS`/socket pool (16 each)
  hold listen + accept + pty + child + stdio; raise them (append-only PCB, re-check asm
  offsets) before dropbear if not.
- dropbear crypto/entropy fit: which KEX/cipher set Rach2's CSPRNG + entropy support, and
  the auth policy (fixed password vs. `authorized_keys`) (Phases 22/23).

*(Settled earlier by the Codex review: directories extend KRFFS in place; VMA = small
fixed array; entropy = virtio-rng, fail-closed; job control is Part II-A; `getpid`
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
- **Part III:** the build a student meets first is fast, obvious, and friendly — one
  transparent GNU-Make idiom across every component, automatic header dependencies
  (no `make clean` ritual), `make doctor`/`make help`, `make`/`make full` tiers that
  build the whole OS (incl. the SSH capstone) from a clean clone with no manual
  `build_*.sh`, and clangd support — with the kernel and all binaries unchanged.
- Every subsystem ships a `Readme.md` (with its real-OS analogue) and a passing
  `tests/*probe`; the existing syscall + network suites never regress; the whole
  project builds clean under our conventions. All commits are the user's; this plan
  and the harness never touched git.
