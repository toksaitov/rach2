/**
 * binfind: find a byte string in a file, and optionally replace it.
 *
 * `binfind STRING FILE` exits 0 when the file contains the string, 1 when
 * it does not, and 2 on an error. `make check-tcc` searches the shipped
 * TinyCC with it for the four diagnostics of its output policy, and
 * check_diskpolicy.sh and check_recovery.sh search a kernel for its
 * banners. The shell text tools differ between platforms on binary input,
 * and strings(1) would be one more host tool for one search; this program
 * behaves the same on every host. The string is 1 to 4096 bytes.
 *
 * `binfind --offset STRING FILE` prints the byte offset of the first match
 * instead, and `binfind --replace OLD NEW FILE` overwrites the first match
 * in place. The replacement must be the same length as what it replaces:
 * the callers are suite scripts marking a kernel's banner or finding a
 * staged menu, where every offset after the edit has to stay where it was.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK 65536

/**
 * The offset of the first occurrence of the `need` bytes at `needle` in `f`,
 * from its current position; -1 when there is none, or -2 when the buffer
 * cannot be allocated. Each read keeps the last `need - 1` bytes of the one
 * before, so a match across two reads is found.
 */
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
                return base + (long)i;
            }
        }
        keep = need - 1 < total ? need - 1 : total;
        memmove(buf, buf + total - keep, keep);
        base += (long)(total - keep);
    }
    free(buf);
    return -1;
}

/**
 * Search, print or replace as the opening comment says. Exit 0 when the
 * string is found, 1 when it is not, or 2 on a usage error or when the file
 * cannot be opened or written or the buffer allocated.
 */
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
            fprintf(stderr,
                    "binfind: the replacement is %lu bytes and the "
                    "string is %lu; an in-place edit cannot change a "
                    "length\n",
                    (unsigned long)strlen(argv[3]), (unsigned long)strlen(needle));
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
        if (fseek(f, at, SEEK_SET) != 0 || fwrite(argv[3], 1, need, f) != need || fclose(f) != 0) {
            fprintf(stderr, "binfind: cannot write %s\n", path);
            return 2;
        }
        return 0;
    }
    fclose(f);
    return 0;
}
