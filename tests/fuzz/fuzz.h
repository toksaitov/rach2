#ifndef _FUZZ_H_
#define _FUZZ_H_

/*
 * What every harness under tests/fuzz defines: one entry that takes an
 * input and its length, runs the parser under test over it, and returns 0.
 * libFuzzer calls it from its own main; built with FUZZ_REPLAY, the main
 * below calls it once for each file named on the command line, which is
 * how the host tier replays a committed corpus in seconds without libFuzzer.
 *
 * The length is the host's size_t, spelled as unsigned long: the kernel's
 * headers define size_t as the machine's 32-bit one, and this is the one
 * place the two meet.
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

#define FUZZ_INPUT_MAX (1u << 20)

int main(int argc, char **argv)
{
    unsigned char *buf = malloc(FUZZ_INPUT_MAX);
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
        while ((got = read(fd, buf + n, FUZZ_INPUT_MAX - (unsigned long)n)) > 0) {
            n += got;
        }
        close(fd);
        LLVMFuzzerTestOneInput(buf, (unsigned long)n);
    }
    printf("fuzz: replayed %d input(s)\n", argc - 1);
    free(buf);
    return 0;
}
#endif

#endif
