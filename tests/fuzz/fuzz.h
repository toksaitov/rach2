#ifndef _FUZZ_H_
#define _FUZZ_H_

/**
 * What every harness under tests/fuzz defines: one entry that takes an
 * input and its length, runs the parser under test over it, and returns 0.
 * libFuzzer calls it from its own main; built with FUZZ_REPLAY, the main
 * below calls it once for each file named on the command line, which is
 * how the host tier replays a committed corpus in seconds without libFuzzer.
 *
 * The length is the host's size_t, spelled as unsigned long: the kernel's
 * headers define size_t as the machine's 32-bit one, and this is the one
 * place the two meet.
 *
 * The replay buffer is one byte larger than `FUZZ_INPUT_MAX`, so a file that
 * fills it is seen to be too long rather than replayed short: an input the
 * fuzzer found and a replay shortened is a different input, and a regression
 * test that passes on it proves nothing.
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
