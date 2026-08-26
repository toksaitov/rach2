/**
 * binfind: find a byte string in a file, and optionally replace it.
 *
 * `binfind STRING FILE` exits 0 when the file contains the string and 1
 * when it does not. The TinyCC policy check needs to know that the shipped
 * compiler carries its four refusal diagnostics, which is a byte search in
 * a binary. strings(1) piped to grep answered it before, but that put a
 * whole extra host tool in the required set for one search, and the shell
 * text tools differ between platforms on binary input; a hundred lines of
 * C differ nowhere.
 *
 * `binfind --offset STRING FILE` prints the byte offset of the first match
 * instead, and `binfind --replace OLD NEW FILE` overwrites the first match
 * in place. The replacement must be the same length as what it replaces:
 * the callers are suite scripts marking a kernel's banner or a staged
 * menu, where every offset after the edit has to stay where it was.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK 65536

/* The offset of the first occurrence of `needle`, or -1. Each read keeps
   the previous block's tail, so a match spanning two reads is still
   seen. */
static long find_in(FILE *f, const char *needle, size_t need)
{
    unsigned char *buf = malloc(CHUNK + need);
    long base = 0;
    size_t keep = 0, have;

    if (buf == NULL)
        return -2;
    while ((have = fread(buf + keep, 1, CHUNK, f)) > 0) {
        size_t total = keep + have;
        size_t i;

        for (i = 0; i + need <= total; i++) {
            if (memcmp(buf + i, needle, need) == 0) {
                free(buf);
                return base + (long) i;
            }
        }
        keep = need - 1 < total ? need - 1 : total;
        memmove(buf, buf + total - keep, keep);
        base += (long) (total - keep);
    }
    free(buf);
    return -1;
}

int main(int argc, char **argv)
{
    const char *needle, *path;
    size_t need;
    long at;
    FILE *f;
    int offset_mode = 0, replace_mode = 0;

    if (argc == 4 && strcmp(argv[1], "--offset") == 0) {
        offset_mode = 1;
        needle = argv[2];
        path = argv[3];
    } else if (argc == 5 && strcmp(argv[1], "--replace") == 0) {
        replace_mode = 1;
        needle = argv[2];
        path = argv[4];
        if (strlen(argv[3]) != strlen(needle)) {
            fprintf(stderr, "binfind: the replacement is %lu bytes and the "
                    "string is %lu; an in-place edit cannot change a "
                    "length\n", (unsigned long) strlen(argv[3]),
                    (unsigned long) strlen(needle));
            return 2;
        }
    } else if (argc == 3) {
        needle = argv[1];
        path = argv[2];
    } else {
        fprintf(stderr, "usage: binfind [--offset] STRING FILE | "
                "binfind --replace OLD NEW FILE\n");
        return 2;
    }
    need = strlen(needle);
    if (need == 0 || need > 4096) {
        fprintf(stderr, "binfind: the string must be 1 through 4096 bytes\n");
        return 2;
    }
    f = fopen(path, replace_mode ? "r+b" : "rb");
    if (f == NULL) {
        fprintf(stderr, "binfind: cannot open %s\n", path);
        return 2;
    }
    at = find_in(f, needle, need);
    if (at == -2) {
        fclose(f);
        return 2;
    }
    if (at < 0) {
        fclose(f);
        return 1;
    }
    if (offset_mode)
        printf("%ld\n", at);
    if (replace_mode) {
        if (fseek(f, at, SEEK_SET) != 0 ||
            fwrite(argv[3], 1, need, f) != need || fclose(f) != 0) {
            fprintf(stderr, "binfind: cannot write %s\n", path);
            return 2;
        }
        return 0;
    }
    fclose(f);
    return 0;
}
