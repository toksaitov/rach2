# Rach2 Kernel CSPRNG (`krandom`)

The kernel's source of randomness: a ChaCha20 stream cipher run as a cryptographically-secure PRNG, seeded from the CPU's hardware entropy (`RDSEED`/`RDRAND`) behind a CPUID feature check, with a timing-jitter fallback. It backs `AT_RANDOM` in every process's startup auxv (a best-effort seed for the stack canary and pointer guard) and the `getrandom(2)` syscall and the `/dev/random` and `/dev/urandom` devices, which serve the same CSPRNG but fail closed when the entropy is not trusted (see below).

## Pieces

| File        | Responsibility                                                                                                                                               |
|-------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `krandom.c` | The ChaCha20 core, the seed collection (`RDSEED`, then `RDRAND`, then jitter), the keystream byte source, and `krandom_reseed`/`krandom_u32`/`krandom_bytes` |
| `krandom.h` | The small public surface used by the auxv builder, `getrandom`, and `kmain`                                                                                  |

## Why ChaCha20

A teaching kernel could ship a linear PRNG, but `AT_RANDOM` and `getrandom` are security primitives: a predictable stream defeats the very stack and pointer hardening musl wires them into. So `krandom` uses the same construction real kernels use for their fast CSPRNG: a stream cipher (ChaCha20, as in Linux's `get_random_bytes` and the BSD `arc4random`) keyed from hardware entropy. The state is `16 x uint32` (constants, key, counter, nonce); `chacha_block` mixes it with the standard quarter-round rounds, and `cc_next_byte` hands out the keystream a byte at a time, refilling a block as it drains.

## Seeding

`krandom_seed` collects a 256-bit key from the best source the CPU offers, checked at runtime so the kernel never executes an unsupported instruction:

1. `RDSEED`: a true-random seed, if CPUID reports it (leaf 7). Preferred.
2. `RDRAND`: the on-chip CSPRNG, if CPUID reports it (leaf 1). Used when `RDSEED` is absent, and as a per-word fallthrough when a `RDSEED` retry budget is exhausted (it can transiently fail under load).
3. Timing jitter: when neither exists (older CPUs or some emulators), the low bits of the TSC sampled across a noisy loop. Weak, but never a constant.

A CPUID max-leaf guard precedes the leaf-7 query so a CPU that does not implement that leaf is never asked for it (`#UD` avoidance). The state is pre-keyed with the ChaCha constants and a default key so even the first block before seeding is not all-zero.

## Trusted entropy and fail-closed

The CSPRNG is trusted only when it was seeded from a real hardware source (`RDSEED` or `RDRAND`); `krandom_is_trusted()` records that. `getrandom` (without `GRND_INSECURE`) and reads of `/dev/random` and `/dev/urandom` serve the stream only when it is trusted; otherwise they fail closed with `-EIO` rather than hand out keys backed by timing jitter alone. The best-effort consumers stay served either way: `AT_RANDOM` (a stack canary only needs to be unpredictable) and `getrandom(..., GRND_INSECURE)`. This is why the run scripts default to `-cpu max`: it exposes `RDRAND`, so the CSPRNG is trusted and dropbear can generate a host key.

## Probe

The pure core is host-tested under ASan and UBSan: `tests/host/krandom_hosttest.c` (`make verify-host`) checks the ChaCha block against known answers, that the byte stream is reproducible for a fixed seed, and that reseeding diverges the stream. In the guest, `tests/timeprobe.c` (`TIMEPROBE OK`) and `tests/musl/timemusl.c` (`TIMEMUSL OK`) confirm two `getrandom` draws differ, so the CSPRNG is live behind syscall 355.
