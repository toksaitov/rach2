# Kernel random generator

`krandom` is the kernel's single ChaCha20-based byte stream. `krandom_seed` replaces its initial deterministic key once during boot. The generator can continue from the fallback seed when the CPU supplies no hardware random value, but the kernel marks that state untrusted and restricts which interfaces may read it.

## Generator state

The 16-word ChaCha input has four constant words, eight key words, a 64-bit block counter, and a 64-bit nonce. `chacha_block` applies 20 rounds and produces 64 bytes. `krandom_u32` and `krandom_bytes` consume one shared byte cursor, so calls of different sizes still form one continuous stream.

`krandom_reseed` accepts four words, derives four more by XOR with fixed constants, resets the counter and nonce to zero, and discards any unused output block. It does not change the recorded entropy source. `krandom_seed` sets that record after it installs either the fallback seed or a complete hardware key.

The running kernel calls `krandom_seed` once and has no later entropy accumulation or automatic rekey. The generator is global and has no lock. Callers must prevent a second random-generator call from interrupting a draw. Current callers run during boot, process setup, or a system call; no interrupt handler calls it.

## Boot seed and trust state

`krandom_seed` queries CPUID leaf 0 for the highest supported basic leaf, then tries these sources in order:

1. If leaf 7 is available and EBX bit 18 reports `RDSEED`, collect eight 32-bit words. Each word gets at most 32 instruction attempts. All eight must succeed.
2. If `RDSEED` is unavailable or any of its eight words fails, discard that attempt. If leaf 1 is available and ECX bit 30 reports `RDRAND`, collect a new eight-word key under the same per-word retry limit. All eight must succeed.
3. If neither instruction supplies a complete key, read the timestamp counter once, read the CMOS seconds field, and include the current PIT tick. Four values derived from those readings go through `krandom_reseed`.

The recorded source is `KRANDOM_SRC_RDSEED`, `KRANDOM_SRC_RDRAND`, or `KRANDOM_SRC_JITTER`. The project treats the two hardware results as trusted. The fallback is best-effort boot variation and remains untrusted. The boot log prints the selected source; the diagnostic summary reports whether it is trusted.

`rach2/run.sh`, `rach2/run_from_disk.sh`, and the default test runner use QEMU's `-cpu max`, which exposes a hardware random instruction and normally produces a trusted seed. `CPU` can select another QEMU model. Dropbear host-key generation needs the trusted interfaces described below.

## Consumer policy

The following consumers draw from the stream regardless of the trust state:

* Each newly built user image receives 16 bytes through `AT_RANDOM` on its initial stack. musl uses them for the stack canary and the mallocng allocator secret. `fork` copies an existing image and does not build a new auxiliary vector.
* Netconsole draws one 32-bit boot identifier during initialization.
* `getrandom` serves the fallback stream only when the caller passes `GRND_INSECURE`.

System call 355, `getrandom`, normally requires a trusted source. Without one it returns `-EIO`. It never blocks: `GRND_NONBLOCK` is a no-op, and `GRND_RANDOM` selects no different pool. The three defined flags may be combined; any other flag bit returns `-EINVAL`. A request larger than `INT32_MAX` also returns `-EINVAL`. Successful and partial copies return the number of bytes written.

`/dev/random` and `/dev/urandom` use the same read function and the same global stream. Neither blocks or has separate depletion accounting. Both return `-EIO` while the source is untrusted; neither has an insecure-read option. Writes to either node are accepted and discarded by the common device sink.

## Source map

* `rach2/sys/lib/krandom.c` implements ChaCha20, seed selection, source tracking, and the byte stream; `rach2/sys/lib/krandom.h` declares the interface.
* `rach2/kernel.c` seeds the generator and prints its source.
* `rach2/sys/proc/uimage.c` places caller-supplied random bytes in `AT_RANDOM`; `rach2/kernel.c` and `rach2/sys/syscall.c` draw those bytes for new images.
* `rach2/sys/syscall.c` implements `getrandom`; `rach2/sys/fs/devfs.c` implements the two random device nodes.
* `rach2/sys/network/netconsole.c` draws the netconsole boot identifier; `rach2/sys/diag.c` reports the trust state.

## Verification

`tests/run host` compiles `tests/host/krandom_hosttest.c` with `rach2/sys/lib/krandom.c` under AddressSanitizer and UndefinedBehaviorSanitizer. It checks repeatability for one explicit seed, separation for a different seed, nonzero output from an all-zero seed, a 37-byte fill, and a fixed 512-byte monobit range. It has no ChaCha known-answer vector and does not exercise CPUID, the hardware instructions, or boot source selection.

`make test` boots with `-cpu max`. When the hardware seed succeeds, `rngprobe` takes its trusted branch, reads two 32-byte buffers from `/dev/urandom`, checks that they differ, and applies a broad monobit range. `timeprobe` and `musl/timemusl` each require two 16-byte `getrandom` calls to differ, so the complete target requires a trusted source. `auxvprobe` checks that `AT_RANDOM` points to a 16-byte value in the initial stack page and that the value is not all zero. `devprobe` checks that successive `/dev/urandom` reads differ.

`tests/run noentropy` boots the test image with `-cpu qemu64`. `musl/saltnoent` requires ordinary `getrandom` to fail with `EIO`, then checks that salt creation changes no output or credential file. The same target runs the fail-closed branch of `musl/passwdmusl`.

`tests/musl/rngmusl.c` is staged for a manual trusted-source run. It requests one 32-byte buffer through musl `getrandom` and another through `/dev/urandom`, then requires full, nonzero, different results. No Make target runs it.

The untrusted branch in `tests/rngprobe.c` also checks `/dev/urandom` for `-EIO` and `getrandom(..., GRND_INSECURE)` for success, but no Make target currently runs that branch. The automated tests do not read `/dev/random` directly or distinguish the `GRND_NONBLOCK` and `GRND_RANDOM` flag behavior.
