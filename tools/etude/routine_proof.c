/* Hash retained bytes without substituting an input identity for a proof. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "routine_proof.h"
#include "sha256.h"
#ifdef _WIN32
#include "win/files.h"
#endif

void routine_quote(struct jbuf *b, const char *text)
{
    struct json value = {.type = JSON_STRING, .string = (char *)text};
    json_canonical(&value, b);
}

void routine_reading(struct jbuf *b, const struct etude_reading *r)
{
    char numbers[160];
    jbuf_puts(b, "{\"utc\":");
    routine_quote(b, r->utc);
    snprintf(numbers, sizeof numbers, ",\"monotonic_us\":%llu,\"resolution_us\":%llu}",
              (unsigned long long)r->monotonic_us, (unsigned long long)r->resolution_us);
    jbuf_puts(b, numbers);
}

int routine_ref(struct jbuf *b, const char *path)
{
    FILE *file = path ? fopen(path, "rb") : NULL;
    if (!file)
        return -1;
    struct sha256 hash;
    sha256_init(&hash);
    unsigned char bytes[65536], sum[32];
    uint64_t size = 0;
    size_t n;
    while ((n = fread(bytes, 1, sizeof bytes, file))) {
        if (size > ETUDE_HISTORY_MAX - n) {
            fclose(file);
            errno = EFBIG;
            return -1;
        }
        size += n;
        sha256_update(&hash, bytes, n);
    }
    int error = ferror(file);
    if (fclose(file))
        error = 1;
    if (error)
        return -1;
    sha256_final(&hash, sum);
    char hex[65], text[180];
    sha256_hex(sum, hex);
    snprintf(text, sizeof text, "{\"sha256\":\"%s\",\"size_bytes\":%llu,\"locations\":[",
              hex, (unsigned long long)size);
    jbuf_puts(b, text);
    routine_quote(b, path);
    jbuf_puts(b, "]}");
    return 0;
}

int routine_create(const char *path, struct jbuf *b)
{
    char temporary[4096];
    int n = snprintf(temporary, sizeof temporary, "%s.proof-%ld", path, (long)getpid());
    if (n < 0 || (size_t)n >= sizeof temporary)
        return -1;
    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    FILE *file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        unlink(temporary);
        return -1;
    }
    int error = fwrite(b->data, 1, b->len, file) != b->len || fflush(file) || fsync(fd);
    if (fclose(file))
        error = 1;
    if (!error && link(temporary, path))
        error = 1;
    if (unlink(temporary))
        error = 1;
    return error ? -1 : 0;
}

int routine_json(const char *path, struct json **document)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return -1;
    struct jbuf b = {0};
    char bytes[8192], error[256];
    size_t n;
    int rc = -1;
    while ((n = fread(bytes, 1, sizeof bytes, file))) {
        if (b.len + n > 16u << 20)
            goto done;
        jbuf_put(&b, bytes, n);
    }
    if (!ferror(file))
        rc = json_parse(b.data, b.len, 1, document, error, sizeof error);
done:
    if (fclose(file))
        rc = -1;
    jbuf_free(&b);
    return rc;
}

int routine_copy(const char *from, const char *to)
{
    FILE *input = fopen(from, "rb");
    if (!input)
        return -1;
    int fd = open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    FILE *output = fd < 0 ? NULL : fdopen(fd, "wb");
    int error = !output;
    if (!output && fd >= 0)
        close(fd);
    char bytes[65536];
    size_t n;
    while (!error && (n = fread(bytes, 1, sizeof bytes, input)))
        if (fwrite(bytes, 1, n, output) != n)
            error = 1;
    if (ferror(input) || fclose(input))
        error = 1;
    if (output) {
        if (fflush(output) || fsync(fileno(output)))
            error = 1;
        if (fclose(output))
            error = 1;
    }
    return error ? -1 : 0;
}
