/**
 * Keeping IPv6's stable secret across boots. See ip6secret.h.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/random.h>
#include <arpa/inet.h>

#include "ip6secret.h"

#define SECRET_SYSCTL  "/proc/sys/net/ipv6/conf/default/stable_secret"
#define IF_INET6       "/proc/net/if_inet6"
#define SECRET_TEXT    48 /* an address's text, at most 39 bytes, and room */
#define RESTART_NAMES  8
#define RESTART_LENGTH 16

/** Read the first line of `path` into `out`, of `cap` bytes, without its
 * newline: its length, or -1 when the file cannot be read. */
static int read_line(const char *path, char *out, size_t cap)
{
    int fd = open(path, O_RDONLY);
    ssize_t n;

    if (fd < 0) {
        return -1;
    }
    n = read(fd, out, cap - 1);
    close(fd);
    if (n < 0) {
        return -1;
    }
    out[n] = '\0';
    out[strcspn(out, "\n")] = '\0';
    return (int)strlen(out);
}

/** Write `text` to the file `path`, which exists, as one write: 0, or -1
 * with errno. */
static int write_text(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY);
    size_t len = strlen(text);
    ssize_t n;
    int saved;

    if (fd < 0) {
        return -1;
    }
    n = write(fd, text, len);
    saved = errno;
    close(fd);
    errno = saved;
    return n == (ssize_t)len ? 0 : -1;
}

/**
 * Keep the secret `text` in IP6SECRET_KEPT: under a temporary name beside
 * it, synced, then renamed, so that a power loss leaves the old file or the
 * new one. Returns 0, or -1 with errno.
 */
static int keep(const char *text)
{
    static const char fresh[] = IP6SECRET_KEPT ".new";
    char line[SECRET_TEXT + 1];
    int fd, ok, saved;

    (void)mkdir("/var/lib", 0755);
    snprintf(line, sizeof(line), "%s\n", text);
    fd = open(fresh, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return -1;
    }
    ok = write(fd, line, strlen(line)) == (ssize_t)strlen(line) && fsync(fd) == 0;
    saved = errno;
    close(fd);
    if (!ok || rename(fresh, IP6SECRET_KEPT) != 0) {
        saved = ok ? errno : saved;
        (void)unlink(fresh);
        errno = saved;
        return -1;
    }
    return 0;
}

/**
 * Turn IPv6 off and on again on each card of /proc/net/if_inet6 that holds
 * a global or site-local address, which a router's prefix made before the
 * secret was in force.
 */
static void restart_slaac(const char *prog)
{
    char names[RESTART_NAMES][RESTART_LENGTH];
    char line[128], addr[33], name[RESTART_LENGTH], path[96];
    unsigned index, plen, scope, flags;
    int n = 0, i, k;
    FILE *f = fopen(IF_INET6, "r");

    if (!f) {
        return;
    }
    while (n < RESTART_NAMES && fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%32s %x %x %x %x %15s", addr, &index, &plen, &scope, &flags, name) != 6 ||
            (scope != 0x00 && scope != 0x40) || strcmp(name, "lo") == 0) {
            continue;
        }
        for (k = 0; k < n && strcmp(names[k], name) != 0; k++) {
        }
        if (k == n) {
            strcpy(names[n++], name);
        }
    }
    fclose(f);
    for (i = 0; i < n; i++) {
        snprintf(path, sizeof(path), "/proc/sys/net/ipv6/conf/%.15s/disable_ipv6", names[i]);
        if (write_text(path, "1\n") != 0 || write_text(path, "0\n") != 0) {
            fprintf(stderr, "%s: %s: cannot restart IPv6 with the stable secret: %s\n", prog,
                    names[i], strerror(errno));
        }
    }
}

int ip6secret_keep(const char *prog)
{
    char text[SECRET_TEXT], now[SECRET_TEXT], line[SECRET_TEXT + 1];
    unsigned char secret[16];

    if (read_line(IP6SECRET_KEPT, text, sizeof(text)) <= 0 ||
        inet_pton(AF_INET6, text, secret) != 1) {
        if (getrandom(secret, sizeof(secret), 0) != (ssize_t)sizeof(secret)) {
            fprintf(stderr, "%s: no random bytes for IPv6's stable secret: %s\n", prog,
                    strerror(errno));
            return -1;
        }
        inet_ntop(AF_INET6, secret, text, sizeof(text));
        if (keep(text) != 0) {
            fprintf(stderr,
                    "%s: cannot keep IPv6's stable secret in %s, so it is this boot's: %s\n", prog,
                    IP6SECRET_KEPT, strerror(errno));
        }
    }
    inet_ntop(AF_INET6, secret, text, sizeof(text));
    if (read_line(SECRET_SYSCTL, now, sizeof(now)) > 0 && strcmp(now, text) == 0) {
        return 0;
    }
    snprintf(line, sizeof(line), "%s\n", text);
    if (write_text(SECRET_SYSCTL, line) != 0) {
        fprintf(stderr, "%s: cannot set IPv6's stable secret: %s\n", prog, strerror(errno));
        return -1;
    }
    restart_slaac(prog);
    return 0;
}
