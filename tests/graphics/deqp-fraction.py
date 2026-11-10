#!/usr/bin/env python3
"""deqp-fraction.py: the cases of a dEQP list that a fraction of Mesa's CI
runs, chosen as deqp-runner 0.20.3's `suite` chooses them.

    python3 -I deqp-fraction.py FRACTION LIST SKIPS... > CASES

deqp-runner reads the list, shuffles it with rand 0.7.3's StdRng, ChaCha20
seeded with 32 bytes of 0x3b, takes the first case and every FRACTIONth
after it, and runs those that no line of the skip files matches. This does
the same: LIST is one case a line, compressed with gzip when its name ends
in .gz; each skip file has a regular expression a line, `#` beginning a
comment, and a line without the characters of a regular expression has its
dots escaped, as deqp-runner does. The cases go to standard output in the
order deqp-runner runs them, and a line of counts to standard error.
"""

import gzip
import re
import struct
import sys

MASK = 0xFFFFFFFF


def chacha20_blocks(key):
    """Yield the 32-bit words of ChaCha20's key stream for the 32-byte key,
    the 64-bit block counter from 0 and the nonce 0, as rand_chacha 0.2's
    ChaCha20Rng gives them to next_u32."""
    k = list(struct.unpack("<8I", key))
    const = [0x61707865, 0x3320646E, 0x79622D32, 0x6B206574]
    counter = 0
    while True:
        init = const + k + [counter & MASK, counter >> 32, 0, 0]
        x = list(init)
        for _ in range(10):
            for a, b, c, d in ((0, 4, 8, 12), (1, 5, 9, 13), (2, 6, 10, 14), (3, 7, 11, 15),
                               (0, 5, 10, 15), (1, 6, 11, 12), (2, 7, 8, 13), (3, 4, 9, 14)):
                x[a] = (x[a] + x[b]) & MASK
                x[d] ^= x[a]
                x[d] = ((x[d] << 16) | (x[d] >> 16)) & MASK
                x[c] = (x[c] + x[d]) & MASK
                x[b] ^= x[c]
                x[b] = ((x[b] << 12) | (x[b] >> 20)) & MASK
                x[a] = (x[a] + x[b]) & MASK
                x[d] ^= x[a]
                x[d] = ((x[d] << 8) | (x[d] >> 24)) & MASK
                x[c] = (x[c] + x[d]) & MASK
                x[b] ^= x[c]
                x[b] = ((x[b] << 7) | (x[b] >> 25)) & MASK
        for i in range(16):
            yield (x[i] + init[i]) & MASK
        counter += 1


def shuffle(items, words):
    """Shuffle items in place as rand 0.7.3's SliceRandom::shuffle does,
    drawing each index with UniformInt<u32>::sample_single."""
    for i in range(len(items) - 1, 0, -1):
        rng = i + 1
        zone = ((rng << (32 - rng.bit_length())) - 1) & MASK
        while True:
            m = next(words) * rng
            if m & MASK <= zone:
                j = m >> 32
                break
        items[i], items[j] = items[j], items[i]


def skip_pattern(line):
    """A skip line as deqp-runner compiles it."""
    line = line.strip()
    if any(c in "*+?()[]{}|^$\\" for c in line):
        return line
    return line.replace(".", "\\.")


def main(argv):
    if len(argv) < 3:
        sys.stderr.write("usage: deqp-fraction.py FRACTION LIST SKIPS...\n")
        return 2
    fraction = int(argv[1])
    opener = gzip.open if argv[2].endswith(".gz") else open
    with opener(argv[2], "rt") as f:
        cases = f.read().splitlines()
    patterns = []
    for path in argv[3:]:
        with open(path) as f:
            patterns += [skip_pattern(l) for l in f.read().splitlines()
                         if l != "" and not l.startswith("#")]
    skips = re.compile("|".join("(?:%s)" % p for p in patterns)) if patterns else None
    shuffle(cases, chacha20_blocks(bytes([0x3B] * 32)))
    taken = cases[::fraction]
    kept = [c for c in taken if skips is None or not skips.search(c)]
    sys.stdout.write("".join(c + "\n" for c in kept))
    sys.stderr.write("deqp-fraction: result: list %d taken %d skipped %d kept %d\n"
                     % (len(cases), len(taken), len(taken) - len(kept), len(kept)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
