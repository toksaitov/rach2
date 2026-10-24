/* Apple system tools execute at their native path, with snapshot verification. */
#include "sanitizer_tool.h"
#ifdef __APPLE__
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "sha256.h"

static int tool_digest(const char *path, char hex[65])
{
    unsigned char bytes[8192], digest[32];
    struct sha256 hash;
    size_t n;
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    sha256_init(&hash);
    while ((n = fread(bytes, 1, sizeof(bytes), f)) != 0)
        sha256_update(&hash, bytes, n);
    int bad = ferror(f);
    fclose(f);
    if (bad)
        return -1;
    sha256_final(&hash, digest);
    sha256_hex(digest, hex);
    return 0;
}
#endif

int host_sanitizer_source(const char *snapshot, char *source, size_t size)
{
#ifdef __APPLE__
    char path[4096], resolved[4096], data[16384], why[256], actual[65], saved[65];
    struct json *record = NULL;
    int ok = 0;
    int length = snprintf(path, sizeof(path), "%s.provenance.json", snapshot);
    if (length < 0 || length >= (int)sizeof(path))
        return -1;
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t n = fread(data, 1, sizeof(data), f);
    int complete = !ferror(f) && feof(f);
    fclose(f);
    if (!complete || json_parse(data, n, 1, &record, why, sizeof(why)))
        return -1;
    const char *native = json_text(record, "source");
    const char *digest = json_text(record, "sha256");
    if (native && native[0] == '/' && digest && strlen(digest) == 64 && strlen(native) < size &&
        realpath(native, resolved) && !strcmp(native, resolved) && !tool_digest(snapshot, saved) &&
        !strcmp(saved, digest) && !tool_digest(native, actual) && !strcmp(actual, digest)) {
        strcpy(source, native);
        ok = 1;
    }
    json_free(record);
    return ok ? 0 : -1;
#else
    (void)snapshot;
    (void)source;
    (void)size;
    return -1;
#endif
}
