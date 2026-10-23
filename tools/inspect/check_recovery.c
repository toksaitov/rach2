/* check_recovery.c: require one disk-authority banner in a built kernel. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int banners(FILE *f, const char *want, const char *other, int found[2])
{
    unsigned char bytes[65536 + 64];
    size_t keep = 0, n, i, nw = strlen(want), no = strlen(other);
    while ((n = fread(bytes + keep, 1, 65536, f)) != 0) {
        n += keep;
        for (i = 0; i < n; i++) {
            found[0] |= nw <= n - i && !memcmp(bytes + i, want, nw);
            found[1] |= no <= n - i && !memcmp(bytes + i, other, no);
        }
        keep = n < 63 ? n : 63;
        memmove(bytes, bytes + n - keep, keep);
    }
    return ferror(f) ? -1 : 0;
}

int main(int argc, char **argv)
{
    char want[64], other[64];
    const char *opposite;
    FILE *f;
    int found[2] = {0, 0}, bad;
    if (argc != 3 || (strcmp(argv[2], "ordinary") && strcmp(argv[2], "recovery"))) {
        fprintf(stderr, "usage: check_recovery KERNEL recovery|ordinary\n");
        return 2;
    }
    opposite = !strcmp(argv[2], "recovery") ? "ordinary" : "recovery";
    snprintf(want, sizeof(want), "rach2: disk authority: %s", argv[2]);
    snprintf(other, sizeof(other), "rach2: disk authority: %s", opposite);
    f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "check-recovery: %s cannot be read\n", argv[1]);
        return 1;
    }
    bad = banners(f, want, other, found);
    if (fclose(f) || bad) {
        fprintf(stderr, "check-recovery: %s could not be read completely\n", argv[1]);
        return 2;
    }
    if (!found[0]) {
        fprintf(stderr, "check-recovery: %s does not carry the %s banner\n", argv[1], argv[2]);
        return 1;
    }
    if (found[1]) {
        fprintf(stderr, "check-recovery: %s carries both authority banners\n", argv[1]);
        return 1;
    }
    printf("check-recovery: ok: %s has %s authority\n", argv[1], argv[2]);
    return 0;
}
