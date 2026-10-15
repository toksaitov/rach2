#ifndef _FUZZ_H_
#define _FUZZ_H_

/**
 * The entry every harness under tests/fuzz defines: it takes an input and its
 * length, runs the parser under test over it, and returns 0. libFuzzer calls
 * it from its own main; built with FUZZ_REPLAY, the main below calls it once
 * for each file named on the command line, which is how the host suite
 * replays a committed corpus without libFuzzer.
 *
 * The length is the host's size_t, written as unsigned long, its type on the
 * 64-bit hosts the harnesses are built on, because this header includes no
 * system header.
 */
int LLVMFuzzerTestOneInput(const unsigned char *data, unsigned long size);

#ifdef FUZZ_REPLAY
/* The host calls a harness needs, declared here rather than through the
   system headers, whose types collide with the kernel's. */
extern int open(const char *path, int flags, ...);
extern long read(int fd, void *buf, unsigned long n);
extern int close(int fd);
extern void *malloc(unsigned long n);
extern void free(void *p);
extern int printf(const char *format, ...);

#define FUZZ_INPUT_MAX (4u << 20)

/**
 * Replay each file named on the command line through the harness, in order.
 *
 * Returns 0 when every file was replayed, and 2 when one cannot be opened or
 * read or is longer than `FUZZ_INPUT_MAX` bytes. The buffer is one byte
 * larger than that, so a file that fills it is seen to be too long rather
 * than replayed short: a shortened input is a different input from the one
 * the fuzzer found, and a regression test that passes on it proves nothing.
 */
int main(int argc, char **argv)
{
    unsigned char *buf = malloc(FUZZ_INPUT_MAX + 1);
    int i;

    if (buf == 0) {
        return 2;
    }
    for (i = 1; i < argc; i++) {
        int fd = open(argv[i], 0);
        long got, n = 0;

        if (fd < 0) {
            printf("fuzz: cannot open %s\n", argv[i]);
            free(buf);
            return 2;
        }
        while ((got = read(fd, buf + n, FUZZ_INPUT_MAX + 1 - (unsigned long)n)) > 0) {
            n += got;
        }
        close(fd);
        if (got < 0) {
            printf("fuzz: cannot read %s\n", argv[i]);
            free(buf);
            return 2;
        }
        if ((unsigned long)n > FUZZ_INPUT_MAX) {
            printf("fuzz: %s is longer than %u bytes\n", argv[i], FUZZ_INPUT_MAX);
            free(buf);
            return 2;
        }
        LLVMFuzzerTestOneInput(buf, (unsigned long)n);
    }
    printf("fuzz: replayed %d input(s)\n", argc - 1);
    free(buf);
    return 0;
}
#endif

#endif
