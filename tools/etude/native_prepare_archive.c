/*
 * native_prepare_archive.c: the transport archive of a prepared native
 * payload, a POSIX ustar stream with stable owners and times, compressed by
 * the host's gzip.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "etude/exec.h"
#include "native_prepare.h"

#define BLOCK  512
#define RECORD (20 * BLOCK)

/* A regular file with more than one link, by the name it was first archived
 * under. */
struct seen {
    dev_t dev;
    ino_t ino;
    char *name;
};

struct sink {
    struct np_context *c;
    struct etude_exec *ex;
    struct etude_proc *gzip;
    int fd;           /* the write end of gzip's standard input */
    int ended;        /* gzip has finished */
    uint64_t written; /* bytes of the tar stream so far */
    struct seen *seen;
    size_t nseen;
};

/* Write all of `bytes` to gzip, serving the executor while the pipe is full. */
static int put(struct sink *s, const void *bytes, size_t n)
{
    const char *p = bytes;
    while (n) {
        if (s->c->stop && *s->c->stop)
            return -1;
        ssize_t w = write(s->fd, p, n);
        if (w > 0) {
            p += w;
            n -= (size_t)w;
            s->written += (uint64_t)w;
            continue;
        }
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            return -1;
        struct etude_watch watch = {s->fd, ETUDE_WATCH_OUT, 0};
        /* interval: serve gzip's output while its input is full */
        if (etude_exec_watch(s->ex, 100, &watch, 1) == s->gzip) {
            s->ended = 1;
            return -1;
        }
    }
    return 0;
}

static void octal(char *field, size_t size, unsigned long long value)
{
    snprintf(field, size, "%0*llo", (int)size - 1, value);
}

/* Write one header; `name` is split into ustar's prefix and name. */
static int header(struct sink *s, const char *name, const struct stat *st, char type,
                  const char *link, unsigned long long size)
{
    unsigned char h[BLOCK];
    size_t n = strlen(name), split = 0;
    unsigned sum = 0;

    memset(h, 0, sizeof(h));
    if (n > 100) {
        for (size_t i = n - 1; i > 0 && !split; i--)
            if (i <= 155 && name[i] == '/' && n - i - 1 <= 100 && n - i - 1 > 0)
                split = i;
        if (!split)
            return -1;
        memcpy(h + 345, name, split);
        memcpy(h, name + split + 1, n - split - 1);
    } else
        memcpy(h, name, n);
    if (link && strlen(link) > 100)
        return -1;
    octal((char *)h + 100, 8, (unsigned long long)(st->st_mode & 07777));
    octal((char *)h + 108, 8, 0);
    octal((char *)h + 116, 8, 0);
    octal((char *)h + 124, 12, size);
    octal((char *)h + 136, 12, 0);
    memset(h + 148, ' ', 8);
    h[156] = (unsigned char)type;
    if (link)
        memcpy(h + 157, link, strlen(link));
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    octal((char *)h + 329, 8, 0);
    octal((char *)h + 337, 8, 0);
    for (size_t i = 0; i < BLOCK; i++)
        sum += h[i];
    snprintf((char *)h + 148, 8, "%06o", sum);
    h[155] = ' ';
    return put(s, h, sizeof(h));
}

static int contents(struct sink *s, const char *path, unsigned long long size)
{
    char bytes[1 << 16];
    unsigned long long left = size;
    FILE *f = fopen(path, "rb");
    int bad = f == NULL;
    while (!bad && left) {
        size_t n = fread(bytes, 1, left < sizeof(bytes) ? (size_t)left : sizeof(bytes), f);
        bad = n == 0 || put(s, bytes, n);
        left -= n;
    }
    if (f) {
        bad = bad || fgetc(f) != EOF;
        fclose(f);
    }
    static const char zeros[BLOCK];
    if (!bad && size % BLOCK)
        bad = put(s, zeros, BLOCK - size % BLOCK);
    return bad ? -1 : 0;
}

static int by_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Archive `path` as `name`, and a directory's entries after it in the order
 * of their names. */
static int entry(struct sink *s, const char *path, const char *name)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return -1;
    if (S_ISREG(st.st_mode)) {
        for (size_t i = 0; st.st_nlink > 1 && i < s->nseen; i++)
            if (s->seen[i].dev == st.st_dev && s->seen[i].ino == st.st_ino)
                return header(s, name, &st, '1', s->seen[i].name, 0);
        if (st.st_nlink > 1) {
            struct seen *v = realloc(s->seen, (s->nseen + 1) * sizeof(*v));
            if (!v)
                return -1;
            s->seen = v;
            s->seen[s->nseen++] = (struct seen){st.st_dev, st.st_ino, np_strdup(name)};
        }
        return header(s, name, &st, '0', NULL, (unsigned long long)st.st_size) ||
                       contents(s, path, (unsigned long long)st.st_size)
                   ? -1
                   : 0;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "etude prepare native: %s: not a file or directory\n", path);
        return -1;
    }
    char directory[4096];
    if (snprintf(directory, sizeof(directory), "%s/", name) >= (int)sizeof(directory) ||
        header(s, directory, &st, '5', NULL, 0))
        return -1;
    DIR *d = opendir(path);
    if (d == NULL)
        return -1;
    char **names = NULL;
    size_t n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char **v = realloc(names, (n + 1) * sizeof(*v));
        if (!v)
            exit(1);
        names = v;
        names[n++] = np_strdup(e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof(*names), by_name);
    int bad = 0;
    for (size_t i = 0; i < n; i++) {
        char child[4096], member[4096];
        if (!bad)
            bad =
                snprintf(child, sizeof(child), "%s/%s", path, names[i]) >= (int)sizeof(child) ||
                snprintf(member, sizeof(member), "%s/%s", name, names[i]) >= (int)sizeof(member) ||
                entry(s, child, member);
        free(names[i]);
    }
    free(names);
    return bad ? -1 : 0;
}

/* Start `gzip -n -c` writing `archive`, with the write end of its standard
 * input, nonblocking, in `s->fd`. */
static int start_gzip(struct sink *s, const char *archive)
{
    struct etude_exec_spec spec;
    int channel[2], original, bad;

    char *argv[] = {s->c->tools[NP_GZIP], "-n", "-c", NULL};
    memset(&spec, 0, sizeof(spec));
    spec.attempt_id = "native-archive";
    spec.argv = argv;
    spec.envp = s->c->environment;
    spec.stdin_path = "-";
    spec.stdout_path = archive;
    spec.output_max = 4096;
    spec.grace_us = 5000000;
    if (pipe(channel) != 0)
        return -1;
    s->fd = channel[1];
    original = dup(STDIN_FILENO);
    bad = original < 0 || fcntl(s->fd, F_SETFD, FD_CLOEXEC) || fcntl(s->fd, F_SETFL, O_NONBLOCK) ||
          dup2(channel[0], STDIN_FILENO) < 0;
    close(channel[0]);
    bad = bad || etude_exec_start(s->ex, &spec, &s->gzip) != 0;
    if (original >= 0 && (dup2(original, STDIN_FILENO) < 0 || close(original)))
        bad = 1;
    return bad ? -1 : 0;
}

/* Wait for gzip, cancelling it when the archive failed or a stop came, and
 * return -1 unless it exited 0 and its processes are gone. */
static int finish_gzip(struct sink *s, int bad)
{
    int cancelled = bad && !s->ended;
    if (cancelled)
        etude_exec_cancel(s->gzip);
    while (!s->ended && etude_exec_wait(s->ex, 100) != s->gzip)
        if (s->c->stop && *s->c->stop && !cancelled) {
            etude_exec_cancel(s->gzip);
            cancelled = 1;
        }
    const struct etude_exec_result *r = etude_proc_result(s->gzip);
    bad = bad || r->end != ETUDE_END_EXITED || r->value != 0 ||
          r->cleanup != ETUDE_CLEANUP_CONFIRMED || r->output_error;
    if (r->err.kept)
        fprintf(stderr, "%s", r->err.data);
    etude_proc_free(s->gzip);
    return bad ? -1 : 0;
}

/**
 * Write `payload` as the gzip-compressed ustar archive `archive`, with the
 * payload as `.`, every owner 0 and every time 0, a file's later links as
 * hard links, and the entries in the order of their names, so that the same
 * payload gives the same bytes on one host.
 *
 * @return 0 with the archive's SHA-256 in `hex`, or -1 after a diagnostic.
 */
int np_archive(struct np_context *c, const char *payload, const char *archive, char hex[65])
{
    struct sink s;

    memset(&s, 0, sizeof(s));
    s.c = c;
    s.fd = -1;
    if ((s.ex = etude_exec_new()) == NULL)
        return -1;
    int bad = start_gzip(&s, archive);
    if (!bad) {
        static const char zeros[RECORD];
        bad = entry(&s, payload, ".") || put(&s, zeros, 2 * BLOCK) ||
              (s.written % RECORD && put(&s, zeros, RECORD - s.written % RECORD));
    }
    if (s.fd >= 0)
        close(s.fd);
    if (s.gzip)
        bad = finish_gzip(&s, bad);
    etude_exec_free(s.ex);
    for (size_t i = 0; i < s.nseen; i++)
        free(s.seen[i].name);
    free(s.seen);
    if (bad) {
        fprintf(stderr, "etude prepare native: %s: archive not written\n", archive);
        return -1;
    }
    return np_file_hash(archive, hex);
}
