/**
 * asciiscan: report lines containing bytes outside the project's ASCII set.
 *
 * The allowed bytes are tab, newline, carriage return, and the printable
 * range 0x20 through 0x7e, the set the text checks allow. The scanner is in C
 * because the shell text tools differ between platforms in how they treat NUL
 * bytes, high bytes and literal control characters in patterns, and a checker
 * that skips a NUL passes a corrupted file.
 */
#include <stdio.h>

/** Whether `byte` is one of the allowed bytes. */
static int allowed(int byte)
{
    return byte == '\t' || byte == '\n' || byte == '\r' || (byte >= 0x20 && byte <= 0x7e);
}

/**
 * Report each line of each file named in `argv` that has a byte outside the
 * set, once, as `file:line: non-ASCII byte` on standard output; a file that
 * cannot be opened is reported on standard error. The exit status is 1 when
 * anything was reported and 0 otherwise.
 */
int main(int argc, char **argv)
{
    int found = 0;
    int i;

    for (i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        long line = 1;
        int reported = 0;
        int byte;

        if (f == NULL) {
            fprintf(stderr, "asciiscan: cannot open %s\n", argv[i]);
            found = 1;
            continue;
        }
        while ((byte = fgetc(f)) != EOF) {
            if (byte == '\n') {
                line++;
                reported = 0;
            } else if (!reported && !allowed(byte)) {
                printf("%s:%ld: non-ASCII byte\n", argv[i], line);
                found = 1;
                reported = 1;
            }
        }
        fclose(f);
    }
    return found;
}
