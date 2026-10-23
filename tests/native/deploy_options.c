/* Validate deployment inputs before any connection or write. */
#define _POSIX_C_SOURCE 200809L
#include "deploy.h"
#include "json.h"
#include "sha256.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define WORDS "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._+-"
int nd_safe(const char *text, const char *allowed)
{
    return text != NULL && text[0] != '\0' && text[strspn(text, allowed)] == '\0';
}
static int port(const char *text, unsigned *value)
{
    char *end;
    unsigned long n = strtoul(text, &end, 10);
    if (!*text || *end || n == 0 || n > 65535)
        return 0;
    *value = (unsigned)n;
    return 1;
}
static int option(struct nd_config *c, const char *name, const char *text)
{
    struct named {
        const char *name;
        const char **value;
    } options[] = {
        {"--peer", &c->peer},           {"--hostpeer", &c->hostpeer}, {"--network", &c->network},
        {"--interface", &c->interface}, {"--gateway", &c->gateway},   {"--far", &c->far},
        {"--empty", &c->empty},         {"--silent", &c->silent},
    };
    size_t n;
    if (!strcmp(name, "--echo-port"))
        return port(text, &c->echo_port);
    if (!nd_safe(text, WORDS "/:"))
        return 0;
    for (n = 0; n < sizeof(options) / sizeof(options[0]); n++)
        if (!strcmp(name, options[n].name)) {
            *options[n].value = text;
            return 1;
        }
    return 0;
}
static int peers_configured(const struct nd_config *c)
{
    return c->hostpeer && c->peer && c->network && c->gateway && c->far && c->empty && c->silent &&
           c->echo_port && access(c->hostpeer, X_OK) == 0;
}
static int configuration(const struct nd_config *c)
{
    int n;
    if (strcmp(c->mode, "stage") && strcmp(c->mode, "run") && strcmp(c->mode, "proof"))
        return 0;
    if (strcmp(c->slot, "candidate") && strcmp(c->slot, "fallback"))
        return 0;
    if (!nd_safe(c->host, WORDS) || !nd_safe(c->run, WORDS) || strlen(c->run) > 48 ||
        !nd_safe(c->target, WORDS "/") || c->target[0] != '/' || strlen(c->target) > 16 ||
        strstr(c->target, "..") || !strcmp(c->target, "/") || c->logs[0] != '/')
        return 0;
    if (!strcmp(c->mode, "proof") && c->ncases)
        return 0;
    for (n = 0; n < c->ncases; n++)
        if (!nd_safe(c->cases[n], WORDS "/"))
            return 0;
    return !strcmp(c->mode, "stage") || peers_configured(c);
}
int nd_options(struct nd_config *c, int argc, char **argv)
{
    int n;
    memset(c, 0, sizeof(*c));
    if (argc < 10)
        return 0;
    c->mode = argv[1];
    c->payload = argv[2];
    c->host = argv[3];
    c->slot = argv[4];
    c->kernel = argv[5];
    c->key = argv[6];
    c->target = argv[7];
    c->run = argv[8];
    c->logs = argv[9];
    c->mac = "28:d2:44:7e:e0:af";
    c->ssh = "/usr/bin/ssh";
    c->interface = "eth0";
    for (n = 10; n < argc; n += 2) {
        if (!strcmp(argv[n], "--")) {
            n++;
            break;
        }
        if (n + 1 == argc || !option(c, argv[n], argv[n + 1]))
            return 0;
    }
    c->cases = argv + n;
    c->ncases = argc - n;
    return configuration(c);
}
static int hash_file(const char *path, char hex[65], uint64_t *size)
{
    struct sha256 hash;
    struct stat st;
    unsigned char buffer[65536], raw[32];
    FILE *file = fopen(path, "rb");
    size_t n;
    int ok;
    if (file == NULL)
        return 0;
    if (fstat(fileno(file), &st) != 0 || !S_ISREG(st.st_mode)) {
        fclose(file);
        return 0;
    }
    sha256_init(&hash);
    *size = 0;
    while ((n = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        sha256_update(&hash, buffer, n);
        *size += n;
    }
    ok = !ferror(file) && *size == (uint64_t)st.st_size;
    fclose(file);
    sha256_final(&hash, raw);
    sha256_hex(raw, hex);
    return ok;
}
static int metadata(struct nd_config *c)
{
    struct json *document = NULL;
    char path[4096], bytes[65536], error[256];
    const char *architecture, *archive;
    FILE *file;
    size_t n;
    int ok = 0;
    if (!nd_format(path, sizeof(path), "%s/prepared.json", c->payload))
        return 0;
    file = fopen(path, "rb");
    if (file == NULL)
        return 0;
    n = fread(bytes, 1, sizeof(bytes), file);
    if (!ferror(file) && n < sizeof(bytes) &&
        json_parse(bytes, n, 1, &document, error, sizeof(error)) == 0) {
        architecture = json_text(document, "architecture");
        archive = json_text(document, "archive_sha256");
        if (archive != NULL && strlen(archive) == 64 && strspn(archive, "0123456789abcdef") == 64 &&
            architecture != NULL &&
            (!strcmp(architecture, "i686") || !strcmp(architecture, "x86_64"))) {
            c->architecture = !strcmp(architecture, "i686") ? "i686" : "x86_64";
            memcpy(c->expected_archive, archive, 65);
            ok = 1;
        }
    }
    json_free(document);
    fclose(file);
    return ok;
}
static int maps(struct nd_config *c)
{
    char path[4096], line[4096], *save, *word;
    unsigned accounts = 0, ports = 0;
    FILE *file;
    if (!nd_format(path, sizeof(path), "%s/tree/tests/native/maps", c->payload))
        return 0;
    file = fopen(path, "r");
    if (file == NULL)
        return 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (sscanf(line, "account %63s %u", c->account, &c->uid) == 2)
            accounts++;
        word = strtok_r(line, " \t\r\n", &save);
        if (word == NULL || strcmp(word, "portmap"))
            continue;
        while ((word = strtok_r(NULL, " \t\r\n", &save)) != NULL) {
            unsigned old, mapped;
            char tail;
            if (sscanf(word, "%u:%u%c", &old, &mapped, &tail) == 2 && old == 7502) {
                c->datagram_port = mapped;
                ports++;
            }
        }
    }
    fclose(file);
    return accounts == 1 && ports == 1 && c->uid > 0 && c->uid <= 65535 && c->datagram_port > 0 &&
           c->datagram_port <= 65535 && nd_safe(c->account, WORDS);
}
int nd_inputs(struct nd_config *c)
{
    char path[4096], wanted[66];
    uint64_t size;
    struct stat key;
    FILE *file;
    if (!metadata(c) || !maps(c) || !hash_file(c->kernel, c->kernel_sha, &c->kernel_size) ||
        c->kernel_size == 0 || stat(c->key, &key) || !S_ISREG(key.st_mode) || (key.st_mode & 077))
        return 0;
    if (!nd_format(c->archive, sizeof(c->archive), "%s.tar.gz", c->payload) ||
        !nd_format(path, sizeof(path), "%s.sha256", c->archive))
        return 0;
    file = fopen(path, "r");
    if (file == NULL)
        return 0;
    wanted[0] = '\0';
    if (fgets(wanted, sizeof(wanted), file) == NULL) {
        fclose(file);
        return 0;
    }
    fclose(file);
    wanted[strcspn(wanted, "\r\n")] = '\0';
    return strlen(wanted) == 64 && strspn(wanted, "0123456789abcdef") == 64 &&
           hash_file(c->archive, c->archive_sha, &size) && !strcmp(wanted, c->archive_sha) &&
           !strcmp(c->expected_archive, c->archive_sha);
}
