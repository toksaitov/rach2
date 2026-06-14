# Rach2 Kernel CSPRNG (`krandom`)

The kernel's source of randomness: a ChaCha20 stream cipher run as a
cryptographically-secure PRNG, seeded from the CPU's hardware entropy
(`RDSEED`/`RDRAND`) behind a CPUID feature check, with a timing-jitter fallback.
It backs `AT_RANDOM` in every process's startup auxv (so a libc gets a real stack
canary and pointer-guard seed) and the `getrandom(2)` syscall (so musl's
`arc4random` and any CSPRNG built on it have a real entropy source).

## Pieces

| File          | Responsibility                                                              |
|---------------|-----------------------------------------------------------------------------|
| `krandom.c`   | The ChaCha20 core, the seed collection (`RDSEED` -> `RDRAND` -> jitter), the keystream byte source, and `krandom_reseed`/`krandom_u32`/`krandom_bytes` |
| `krandom.h`   | The small public surface used by the auxv builder, `getrandom`, and `kmain` |

## Why ChaCha20

A teaching kernel could ship a linear PRNG, but `AT_RANDOM` and `getrandom` are
security primitives -- a predictable stream defeats the very stack/pointer
hardening musl wires them into. So `krandom` uses the same construction real
kernels use for their fast CSPRNG: a stream cipher (ChaCha20, as in Linux's
`get_random_bytes` and the BSD `arc4random`) keyed from hardware entropy. The
state is `16 x uint32` (constants, key, counter, nonce); `chacha_block` mixes it
with the standard quarter-round rounds, and `cc_next_byte` hands out the
keystream a byte at a time, refilling a block as it drains.

## Seeding

`krandom_seed` collects a 256-bit key from the best source the CPU offers,
checked at runtime so the kernel never executes an unsupported instruction:

1. **`RDSEED`** -- a true-random seed, if CPUID reports it (leaf 7). Preferred.
2. **`RDRAND`** -- the on-chip CSPRNG, if CPUID reports it (leaf 1). Used when
   `RDSEED` is absent, and as a per-word fallthrough when a `RDSEED` retry budget
   is exhausted (it can transiently fail under load).
3. **timing jitter** -- when neither exists (older CPUs / some emulators), the
   low bits of the TSC sampled across a noisy loop. Weak, but never a constant.

A CPUID max-leaf guard precedes the leaf-7 query so a CPU that does not implement
that leaf is never asked for it (`#UD` avoidance). The state is pre-keyed with the
ChaCha constants and a default key so even the first block before seeding is not
all-zero.

## Probe

The pure core is host-tested under ASan/UBSan: `tests/host/krandom_hosttest.c`
(`make verify-host`) checks the ChaCha block against known answers, that the byte
stream is reproducible for a fixed seed, and that reseeding diverges the stream.
In the guest, `tests/timeprobe.c` (`TIMEPROBE OK`) and `tests/musl/timemusl.c`
(`TIMEMUSL OK`) confirm two `getrandom` draws differ -- the CSPRNG is live behind
syscall 355.
