/*
 * capture.c: the capture of a case's output streams, and the scan of its
 * standard output for the `child` records that name descendants to end.
 */
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "capture.h"
#ifdef _WIN32
#include "win/files.h"
#endif

int capture_init(struct etude_output *o, size_t max)
{
    memset(o, 0, sizeof(*o));
    o->data = malloc(max + 1);
    if (o->data == NULL)
        return -1;
    o->data[0] = '\0';
    return 0;
}

void capture_add(struct etude_output *o, size_t max, const char *buf, size_t n)
{
    size_t room = max - o->kept;
    size_t take = n < room ? n : room;

    memcpy(o->data + o->kept, buf, take);
    o->kept += take;
    o->data[o->kept] = '\0';
    o->total += n;
}

void capture_free(struct etude_output *o)
{
    free(o->data);
    o->data = NULL;
}

/**
 * Give the PID of a `child` record, the line without its newline, or -1
 * when the line is not one.
 */
static long child_record(const char *line, size_t len)
{
    static const char head[] = ETUDE_REPORT_PREFIX ETUDE_REPORT_CHILD " ";
    size_t i = sizeof(head) - 1;
    long pid = 0;

    if (len <= i || len - i > 9 || memcmp(line, head, i) != 0 || line[i] == '0')
        return -1;
    for (; i < len; i++) {
        if (line[i] < '0' || line[i] > '9')
            return -1;
        pid = pid * 10 + (line[i] - '0');
    }
    return pid;
}

void capture_scan(struct capture_lines *s, const char *buf, size_t n,
                  void (*found)(void *ctx, long pid), void *ctx)
{
    size_t i;
    long pid;

    for (i = 0; i < n; i++) {
        if (buf[i] != '\n') {
            if (s->len < sizeof(s->line))
                s->line[s->len++] = buf[i];
            else
                s->overlong = 1;
            continue;
        }
        pid = s->overlong ? -1 : child_record(s->line, s->len);
        if (pid > 0)
            found(ctx, pid);
        s->len = 0;
        s->overlong = 0;
    }
}

int capture_files_open(struct capture_files *f, const struct etude_exec_spec *s)
{
    memset(f, 0, sizeof(*f));
    if (s->stdout_path != NULL && (f->out = fopen(s->stdout_path, "wb")) == NULL)
        return -1;
    if (s->stderr_path != NULL) {
        if (s->stdout_path != NULL && strcmp(s->stdout_path, s->stderr_path) == 0)
            f->err = f->out;
        else if ((f->err = fopen(s->stderr_path, "wb")) == NULL) {
            int saved = errno;
            capture_files_close(f);
            errno = saved;
            return -1;
        }
    }
#ifndef _WIN32
    if (f->out != NULL)
        fcntl(fileno(f->out), F_SETFD, FD_CLOEXEC);
    if (f->err != NULL && f->err != f->out)
        fcntl(fileno(f->err), F_SETFD, FD_CLOEXEC);
#else
    if (f->out != NULL)
        SetHandleInformation((HANDLE)_get_osfhandle(_fileno(f->out)), HANDLE_FLAG_INHERIT, 0);
    if (f->err != NULL && f->err != f->out)
        SetHandleInformation((HANDLE)_get_osfhandle(_fileno(f->err)), HANDLE_FLAG_INHERIT, 0);
#endif
    return 0;
}

int capture_files_add(FILE *f, const char *bytes, size_t n)
{
    if (f == NULL)
        return 0;
    if (fwrite(bytes, 1, n, f) != n || fflush(f) != 0)
        return errno != 0 ? errno : EIO;
    return 0;
}

void capture_files_close(struct capture_files *f)
{
    if (f->err != NULL && f->err != f->out)
        fclose(f->err);
    if (f->out != NULL)
        fclose(f->out);
    f->out = f->err = NULL;
}
