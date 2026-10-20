/*
 * capture.c: the capture of a case's output streams, and the scan of its
 * standard output for the `child` records that name descendants to end.
 */
#include <stdlib.h>
#include <string.h>

#include "capture.h"

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
