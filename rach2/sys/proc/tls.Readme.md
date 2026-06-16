# Thread-Local Storage (`%gs`) for Rach2

Per-process thread-local storage on i386, reached through the `%gs` segment.
This is what lets a real C library keep `errno`, the stack-protector canary, and
(later) `pthread_self()` at a fixed `%gs`-relative address.

Real-OS analogue: this is exactly Linux's i386 TLS mechanism. The C library
(musl, glibc) calls **`set_thread_area(2)`** with a `struct user_desc` to install
a GDT descriptor whose base is the thread control block (TCB), then loads the
returned selector into `%gs`; the kernel reprograms that GDT entry on every
context switch (`load_TLS()` in Linux's `__switch_to`). Rach2 mirrors this
faithfully, down to the `user_desc` ABI and the syscall number (243).

## How it works

| Piece | File | Role |
|-------|------|------|
| TLS GDT slot | `tables/gdt.c`, `tables/gdt.h` | Entry 6 (`GDT_TLS_ENTRY`): a present, DPL-3, 4 GB R/W data descriptor whose base is reprogrammed at runtime. `gdt_set_tls(base)` rewrites just the base. |
| `set_thread_area(243)` | `proc/tls.c` (validation), `sys/syscall.c` (glue) | Copies the user's `user_desc` in, validates it (`tls_resolve_base`), programs the GDT slot and the caller's PCB base, writes the entry number (6) back. The C library then loads `(6 << 3) | 3 = 0x33` into `%gs`. |
| Per-process base | `proc/process.h` (`tls_base`, appended) | Each PCB records its TLS base so the switch can restore it. `fork` copies it to the child (`sys/syscall.c`). |
| Context switch | `proc/switch.s` | On every switch, rewrites GDT entry 6 to `new->tls_base` and reloads `%gs`, with interrupts disabled so a nested IRQ never sees a half-written descriptor. |
| Trapframe `%gs` | `sys/isr.h`, `sys/isr_handler.s` | `isr_params_t` saves `gs/fs/es/ds` individually; both the ISR and IRQ stubs save them on entry and restore them on exit, so a process's `%gs` thread pointer survives any syscall or interrupt (the earlier code smeared `%gs` from the saved `%ds`). |
| Syscall `sti` window | `sys/syscall.c`, `sys/isr_handler.s` | The syscall stub stays at IF=0 through entry; `_syscall_handler` sets `in_syscall` and only then enables interrupts, so a timer IRQ cannot preempt a half-built trapframe. `iret` restores the caller's IF. |

`switch.s` hard-codes the PCB field offsets it touches (including
`PROC_TLS_BASE`); `init_proc` asserts them against `offsetof` at boot so a future
struct change fails loudly instead of silently corrupting switches. New PCB
fields must therefore be **appended**, never inserted.

## Verification

- `tests/tlsprobe.c` -- a hermetic, libc-free probe. It installs TLS, writes
  sentinels through `%gs`, then (1) hammers the kernel with thousands of
  syscalls and timer IRQs checking the sentinels survive, and (2) forks a child
  with a *different* TCB and spins so the scheduler interleaves them hundreds of
  times, each asserting it reads back only its own sentinels (proves the
  per-switch descriptor rewrite). Prints `All TLS checks passed!`.
- `tests/host/tls_hosttest.c` -- compiles `proc/tls.c` natively under
  AddressSanitizer/UBSan and checks `tls_resolve_base` accepts a musl-style
  `user_desc` and rejects malformed ones.

No musl is required to exercise any of this; Phase 3 onward links real musl
binaries against the same mechanism.
