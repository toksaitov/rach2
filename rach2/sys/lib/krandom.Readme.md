# Kernel random generator

`krandom` is the kernel's one source of random bytes: a ChaCha20 keystream that the boot keys once, from the processor's hardware random instructions when it has them and from boot-time timing when it does not. The kernel records which source keyed it. The interfaces a cryptographic key could be made from, `getrandom` and the random devices, refuse to answer from the timing fallback; the uses that need only variation between boots draw from the stream either way.

## Interfaces

`krandom.h` declares the generator:

* `krandom_seed()` keys it. The boot calls it once.
* `krandom_u32()` and `krandom_bytes(buf, n)` draw from the stream.
* `krandom_reseed(a, b, c, d)` keys it from four words. Only `krandom_seed` and the host test call it.
* `krandom_source()` returns the source that keyed it, `KRANDOM_SRC_RDSEED`, `KRANDOM_SRC_RDRAND` or `KRANDOM_SRC_JITTER`, and `krandom_is_trusted()` is 1 for the first two.

Three kernel consumers draw whatever the source:

* Each program image the kernel builds gets 16 bytes on its initial stack, which the `AT_RANDOM` auxiliary vector entry points at: `exec_build` in `sys/syscall_proc.c` for `execve`, and `boot_stack` in `kernel.c` for the programs the boot starts. musl takes its stack-protector canary and the mallocng allocator's secret from them. `fork` copies the parent's stack and draws nothing.
* TCP draws four words once, at the first `listen` or `connect`, as the key of the hash that chooses initial sequence numbers (`tcp_isn_key` in `sys/network/tcp/tcp.c`).
* Netconsole draws one word as its boot identifier when `rach2.netconsole` enables it (`netconsole_enable` in `sys/network/netconsole.c`).

Two user interfaces refuse an untrusted source:

* `getrandom`, system call 355 on i386 and 318 on x86-64 (`sys_getrandom` in `sys/syscall.c`), fails with `EIO` while the source is untrusted unless the caller passes `GRND_INSECURE`. Its flags and errors are in [getrandom(2)](../../../man/man2/getrandom.2).
* `/dev/random` and `/dev/urandom` share one read function (`dev_rand_read` in `sys/fs/devfs.c`), which fails with `EIO` while the source is untrusted and has no insecure mode. See [random(4)](../../../man/man4/random.4).

The boot logs the source as an `entropy: ready:` record, at warning level for the timing fallback; getrandom(2) lists its three forms. The summary of the `rach2.diag` report repeats it as `diag: entropy: trusted` or `diag: entropy: not trusted` (`sys/arch/x86/diag.c`).

## Design

### The generator

`krandom.c` keeps one ChaCha20 input block of 16 words: the four constant words of "expand 32-byte k", eight key words, a 64-bit block counter and a 64-bit nonce, always zero. `chacha_block` runs the 20 rounds as ten column and diagonal double rounds and adds the input back, giving 64 bytes of keystream. A byte cursor indexes that block; when it reaches the end, the next block is computed and the counter advanced. `krandom_u32` takes four bytes from the same cursor as `krandom_bytes`, least significant first, so draws of any size form one stream.

`krandom_reseed` builds the eight-word key from its four words and the same four XORed with fixed constants, sets the counter and nonce to zero and discards the rest of the current block. It leaves the recorded source as it was. Before the boot keys it the block has a fixed key and the source reads as the fallback, so an early draw gives a fixed stream and `krandom_is_trusted` is 0.

### Seeding

The boot calls `krandom_seed` after it has started the timer and read the clock (`kernel.c`). `hw_key` asks for eight words from the processor's seed source, RDSEED, allowing each word 32 attempts; if any word fails, it asks for eight new words from the generator, RDRAND, on the same terms. `arch_random` in `sys/arch/x86/cpu.c` executes an instruction only when CPUID reports it, RDSEED in leaf 7 EBX bit 18 and RDRAND in leaf 1 ECX bit 30, and asks a leaf only when leaf 0's highest basic leaf covers it, since a leaf past that returns another leaf's bits. A complete key is installed as it came, and the source recorded as RDSEED or RDRAND.

Without a complete key, `krandom_seed` reads the time-stamp counter (`arch_cycles`, RDTSC), the real time in seconds that the CMOS clock gave at boot (`rtc_boot_time` in `sys/clock.c`, 0 when the clock could not be read) and the tick count `kernel_ticks`, which the local APIC timer or the 8254 advances. It mixes them into four words for `krandom_reseed` and records `KRANDOM_SRC_JITTER`. These values differ between boots but an observer can estimate them, so the source is untrusted.

The host build (`RACH2_HOSTTEST`) replaces `krandom_seed` with one that keys the generator from four fixed words; everything else in the file is the same code.

### Locking and context

The state is static in `krandom.c` and has no lock. Every caller runs in process context, at boot or in a system call, and holds the big lock (`sys/proc/smp.h`), so two processors never draw at once. An interrupt handler nests inside the big lock on its own processor, and a draw from one could take bytes from the middle of a block another draw is reading; no handler calls `krandom`, and none may. For that reason TCP draws its key in `tcp_listen` and `tcp_connect`, in a system call, and not when a passive open chooses its sequence number, which happens in interrupt context as the SYN arrives.

## Limits

* The generator is keyed once. It has no entropy pool, takes no later input and never rekeys.
* The key is not replaced after a draw, so whoever reads kernel memory can compute earlier output.
* A processor without RDSEED and RDRAND, such as QEMU's `qemu64` model, leaves `getrandom` and both devices failing with `EIO`, and Dropbear cannot make a host key. `rach2/run.sh`, `run_from_disk.sh` and `run_from_iso.sh` pass `-cpu max` for that reason; `CPU=` selects another model.
* Bytes written to `/dev/random` or `/dev/urandom` are discarded and do not change the state.

## Tests

* `tests/run host` compiles `tests/host/krandom_hosttest.c` with `krandom.c` under AddressSanitizer and UndefinedBehaviorSanitizer. It checks that one seed repeats its stream, that another seed gives a different one, that an all-zero seed gives nonzero output, a 37-byte fill, and that 512 bytes have between 1844 and 2252 one bits. It has no ChaCha20 known-answer vector and runs neither CPUID, the instructions nor the seeding.
* `tests/run default` boots the test image under `-cpu max`, the runner's default, so the source is trusted. `rngprobe` reads `/dev/urandom` twice, requires the two 32-byte reads to differ and to have between 180 and 332 one bits together, and compares the kernel's `kcpu_id` (`sys/arch/x86/kcpu.c`) for leaf 7 with a direct query of subleaf zero. `timeprobe` and `musl/timemusl` require two 16-byte `getrandom` calls to differ, `devprobe` two `/dev/urandom` reads, and `auxvprobe` requires `AT_RANDOM` to point into the initial stack page at bytes that are not all zero.
* `tests/run noentropy` boots under `-cpu qemu64`, where the source is the fallback. `musl/saltnoent` requires `getrandom` to fail with `EIO` and password salt creation to change no file; `musl/passwdmusl` takes its fail-closed side; `rngprobe` requires `getrandom`, `/dev/urandom` and `/dev/random` to fail with `EIO` and `GRND_INSECURE` to return bytes; and on os-real, `dropbearkey` must refuse and write no key.
* `sh tests/system/noentropy_mutate.sh` compiles three faults into the kernel in turn: the devices answer while untrusted, `/dev/random` reads as `/dev/zero`, and `GRND_INSECURE` is refused. Each must fail its steps of the noentropy suite, which passes before the faults and after each source is restored.
* `sh tests/system/rng_mutate.sh` changes `kcpu_id`'s subleaf operand from input to output and requires `rngprobe` to fail its CPUID check under `-cpu max`, then to pass with the source restored.
* `tests/musl/rngmusl.c` is staged for a run by hand on a trusted boot; no suite runs it. No test reads `/dev/random` on a trusted boot or passes `GRND_NONBLOCK` or `GRND_RANDOM`.

Run a suite with `tests/run NAME`, and a mutation script with `sh` from the repository root.
