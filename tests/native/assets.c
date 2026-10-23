/* assets.c: validate the immutable preparation before running its cases. */
#define _POSIX_C_SOURCE 200809L
#include "collection.h"
#include "../../utils/sha256.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
static int digest(const char *path, char hex[65])
{
    struct sha256 hash;
    unsigned char bytes[65536], sum[32];
    size_t n;
    FILE *fp = fopen(path, "rb");
    struct stat st;
    int ok;
    if (!fp)
        return 0;
    if (fstat(fileno(fp), &st) || !S_ISREG(st.st_mode)) {
        fclose(fp);
        return 0;
    }
    sha256_init(&hash);
    while ((n = fread(bytes, 1, sizeof(bytes), fp)))
        sha256_update(&hash, bytes, n);
    ok = !ferror(fp);
    fclose(fp);
    sha256_final(&hash, sum);
    sha256_hex(sum, hex);
    return ok;
}
int nc_assets(const struct nc_config *conf)
{
    char path[8192], line[8192], hex[65];
    FILE *fp;
    unsigned count = 0;
    int ok = 1;
    snprintf(path, sizeof(path), "%s/assets.sha256", conf->tree);
    fp = fopen(path, "r");
    if (!fp) {
        perror(path);
        return 0;
    }
    while (fgets(line, sizeof(line), fp)) {
        char *name;
        line[strcspn(line, "\r\n")] = 0;
        if (strlen(line) < 67 || line[64] != ' ' || line[65] != ' ' ||
            strspn(line, "0123456789abcdef") != 64) {
            ok = 0;
            break;
        }
        line[64] = 0;
        name = line + 66;
        if (!*name || *name == '/' || strstr(name, "../") || !strcmp(name, "..")) {
            ok = 0;
            break;
        }
        snprintf(path, sizeof(path), "%s/%s", conf->tree, name);
        if (!digest(path, hex) || strcmp(hex, line)) {
            fprintf(stderr, "native: prepared input differs: %s\n", path);
            ok = 0;
        }
        count++;
    }
    ok &= !ferror(fp) && count > 0;
    fclose(fp);
    return ok;
}
