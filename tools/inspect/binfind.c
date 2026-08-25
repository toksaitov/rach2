/**
 * binfind: exit 0 when a file contains a byte string, 1 when it does not.
 *
 * Usage: `binfind STRING FILE`. The TinyCC policy check needs to know that
 * the shipped compiler carries its four refusal diagnostics, which is a
 * byte search in a binary. strings(1) piped to grep answered it before,
 * but that put a whole extra host tool in the required set for one search,
 * and the shell text tools differ between platforms on binary input; fifty
 * lines of C differ nowhere.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *needle;
    size_t need, have, keep;
    unsigned char *buf;
    FILE *f;

    if (argc != 3) {
        fprintf(stderr, "usage: binfind STRING FILE\n");
        return 2;
    }
    needle = argv[1];
    need = strlen(needle);
    if (need == 0 || need > 4096) {
        fprintf(stderr, "binfind: the string must be 1 through 4096 bytes\n");
        return 2;
    }
    f = fopen(argv[2], "rb");
    if (f == NULL) {
        fprintf(stderr, "binfind: cannot open %s\n", argv[2]);
        return 2;
    }
    buf = malloc(65536 + need);
    if (buf == NULL) {
        fclose(f);
        return 2;
    }
    /* Each read keeps the previous block's tail, so a match spanning two
       reads is still seen. */
    keep = 0;
    while ((have = fread(buf + keep, 1, 65536, f)) > 0) {
        size_t total = keep + have;
        size_t i;

        for (i = 0; i + need <= total; i++) {
            if (memcmp(buf + i, needle, need) == 0) {
                free(buf);
                fclose(f);
                return 0;
            }
        }
        keep = need - 1 < total ? need - 1 : total;
        memmove(buf, buf + total - keep, keep);
    }
    free(buf);
    fclose(f);
    return 1;
}
