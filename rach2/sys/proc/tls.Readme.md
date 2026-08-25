# Thread-local storage on i386

Rach2 implements the part of the Linux i386 `set_thread_area` ABI that static musl uses during process startup. The kernel gives each process a `%gs` base. musl points that base at its thread control block (TCB), loads the returned segment selector into `%gs`, and then reaches the TCB through `%gs`-relative addresses. This supplies the initial musl thread's `errno`, `pthread_self()`, locale state, and stack canary.

Rach2 has one execution thread per process. It does not implement `clone`, multiple TLS descriptors, `get_thread_area`, or `modify_ldt`. This interface therefore provides process-local TLS and musl startup compatibility, not Linux thread creation or the complete Linux TLS API.

## The `set_thread_area` subset

System call 243 takes a pointer in `EBX` to the 16-byte i386 `user_desc` layout:

```c
struct user_desc {
    uint32_t entry_number;
    uint32_t base_addr;
    uint32_t limit;
    uint32_t flags;
};
```

The kernel accepts `entry_number` equal to `-1`, meaning allocate, or the explicit entry number 6. It requires a present, 32-bit, writable data segment. Requests for another entry, a code or expand-down segment, a read-only segment, a non-present segment, or a descriptor without the 32-bit flag fail with `EINVAL`.

The descriptor itself has a fixed shape: present, DPL 3, writable data, and a 4 GiB limit. Only `base_addr` is taken from the request. The caller's `limit`, limit-granularity bit, usable bit, and any unexamined flag bits do not change the hardware descriptor. On success the kernel stores the base in the process control block and writes entry number 6 back to `user_desc.entry_number`.

The user selector is `(6 << 3) | 3`, or `0x33`. `set_thread_area` does not replace the saved user `%gs` selector. The caller must load `0x33`; musl's i386 `__set_thread_area` does this immediately after the system call succeeds. Its first request uses entry `-1`, limit `0xfffff`, and flags `0x51`. Later requests reuse entry 6.

An unreadable input descriptor or unwritable entry-number field returns `EFAULT`; a call without a current process returns `ESRCH`. The base is installed before the entry number is copied back. A descriptor that can be read but not written can therefore return `EFAULT` after changing the process's TLS base.

## Descriptor and process state

`rach2/sys/tables/gdt.c` creates GDT entry 6 at boot with base zero. `gdt_set_tls` rewrites only its three base fields. Paging, rather than the TLS descriptor limit, enforces access to the resulting linear addresses.

`process_t.tls_base` records the value for each process. A newly allocated process starts with base zero. The kernel's direct `exec_program` path, used for the initial init process and programs launched from the debug shell, passes that base to `gdt_set_tls` because it does not pass through `switch_context`.

On every process switch, `rach2/sys/proc/switch.s` writes the incoming process's base into entry 6. It disables interrupts while updating the three descriptor fields and reloads `%gs` so the processor's hidden descriptor cache uses the new base. The interrupt-return frame later restores the incoming process's user selector.

System call and interrupt entry save `%gs`, `%fs`, `%es`, and `%ds` separately before loading the kernel data selector. Their return paths restore the four saved selectors. The system call handler does not enable interrupts until it has marked the current process non-preemptible, so a timer interrupt cannot switch away while the entry frame is incomplete. Signal delivery also saves and restores the `%gs` selector through its user frame.

`fork` copies both `tls_base` and the saved `%gs` selector. The child initially uses the same virtual TCB address in its copied address space and may then install a different base. A successful `execve` clears `tls_base`, resets the live descriptor base to zero, and resets `%gs`, `%fs`, `%es`, and `%ds` to the flat user-data selector `0x23`. The new program's libc establishes its own TLS before `main`.

## Source map

* `rach2/sys/proc/tls.h` defines the `user_desc` layout and flags.
* `rach2/sys/proc/tls.c` validates the supported descriptor fields and returns the requested base.
* `rach2/sys/tables/gdt.c` owns GDT entry 6 and its base update.
* `rach2/sys/syscall.c` implements system call 243 and the `fork` and `execve` TLS rules.
* `rach2/sys/proc/process.h` stores the per-process base; `rach2/sys/proc/process.c` checks the assembly-visible field offsets at boot.
* `rach2/sys/proc/switch.s` applies the incoming base during a context switch.
* `rach2/sys/isr.h` and `rach2/sys/isr_handler.s` define and preserve the saved segment-register frame.

## Verification

`make test` runs `tests/tlsprobe.c`. The probe installs entry 6, checks eight `%gs`-relative words through 40,000 system-call iterations and timer interrupts, then forks. The child first checks the inherited base, installs a different TCB, and runs 3,000 iterations in parallel with 3,000 parent iterations. Each process checks only its own sentinels. The probe succeeds with `All TLS checks passed!`.

The same test run starts its musl-linked programs through the real i386 startup code. Each program calls musl's `__init_tls` and `__set_thread_area` before `main`.

`tests/run host` runs `tests/host/tls_hosttest.c` under AddressSanitizer and UndefinedBehaviorSanitizer. It checks that `tls_resolve_base` accepts musl's `-1`/`0x51` request and explicit entry 6, then rejects a wrong entry, missing 32-bit flag, non-data segment, read-only segment, non-present segment, and null arguments.
