/* wire.c: CRC-32, base64 and the frames of include/etude/frame.h. */
#include "wire.h"
#include <etude/frame.h>
#include <stdio.h>
#include <string.h>

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

uint32_t wire_crc32(const void *p, size_t n)
{
    const unsigned char *b = p;
    uint32_t c = 0xFFFFFFFFu;
    int k;

    while (n--) {
        c ^= *b++;
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}

size_t wire_base64_len(size_t n)
{
    return (n + 2) / 3 * 4;
}

void wire_base64(const unsigned char *in, size_t n, char *out)
{
    size_t i;

    for (i = 0; i + 2 < n; i += 3) {
        *out++ = b64[in[i] >> 2];
        *out++ = b64[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
        *out++ = b64[((in[i + 1] & 15) << 2) | (in[i + 2] >> 6)];
        *out++ = b64[in[i + 2] & 63];
    }
    if (i < n) {
        *out++ = b64[in[i] >> 2];
        if (i + 1 < n) {
            *out++ = b64[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
            *out++ = b64[(in[i + 1] & 15) << 2];
        } else {
            *out++ = b64[(in[i] & 3) << 4];
            *out++ = '=';
        }
        *out++ = '=';
    }
    *out = '\0';
}

/** The value of the base64 digit `c`, or -1. */
static int b64_value(char c)
{
    const char *p = c ? strchr(b64, c) : NULL;

    return p ? (int)(p - b64) : -1;
}

long wire_unbase64(const char *in, unsigned char *out)
{
    size_t len = strlen(in);
    size_t i;
    long n = 0;

    if (len == 0 || len % 4 != 0)
        return -1;
    for (i = 0; i < len; i += 4) {
        int a = b64_value(in[i]);
        int b = b64_value(in[i + 1]);
        int c = in[i + 2] == '=' ? -2 : b64_value(in[i + 2]);
        int d = in[i + 3] == '=' ? -2 : b64_value(in[i + 3]);
        int last = i + 4 == len;

        if (a < 0 || b < 0 || c == -1 || d == -1 || (c == -2 && d != -2) ||
            ((c == -2 || d == -2) && !last))
            return -1;
        out[n++] = (unsigned char)((a << 2) | (b >> 4));
        if (c == -2) {
            if (b & 15)
                return -1;
            break;
        }
        out[n++] = (unsigned char)(((b & 15) << 4) | (c >> 2));
        if (d == -2) {
            if (c & 3)
                return -1;
            break;
        }
        out[n++] = (unsigned char)(((c & 3) << 6) | d);
    }
    return n;
}

/** Whether `s` is one or more bytes from '!' to '~'. */
static int printable(const char *s)
{
    if (!*s)
        return 0;
    for (; *s; s++)
        if (*s < '!' || *s > '~')
            return 0;
    return 1;
}

/** Append `s` to `out` at `*pos` within `cap` less a terminator; 0 when it
 * does not fit. */
static int put(char *out, size_t cap, size_t *pos, const char *s)
{
    size_t n = strlen(s);

    if (*pos + n + 1 > cap)
        return 0;
    memcpy(out + *pos, s, n);
    *pos += n;
    out[*pos] = '\0';
    return 1;
}

size_t wire_encode(char *out, size_t cap, const char *session, const char *boot, uint64_t seq,
                   int has_ack, uint64_t ack, const char *type, int nfields,
                   const char *const *field)
{
    char num[48];
    size_t pos = 0;
    int i;

    if (!printable(type))
        return 0;
    if (has_ack)
        snprintf(num, sizeof(num), " %llu %llu ", (unsigned long long)seq, (unsigned long long)ack);
    else
        snprintf(num, sizeof(num), " %llu - ", (unsigned long long)seq);
    if (!put(out, cap, &pos, ETUDE_FRAME_MAGIC " ") || !put(out, cap, &pos, session) ||
        !put(out, cap, &pos, " ") || !put(out, cap, &pos, boot) || !put(out, cap, &pos, num) ||
        !put(out, cap, &pos, type))
        return 0;
    for (i = 0; i < nfields; i++)
        if (!printable(field[i]) || !put(out, cap, &pos, " ") || !put(out, cap, &pos, field[i]))
            return 0;
    if (!put(out, cap, &pos, " "))
        return 0;
    snprintf(num, sizeof(num), "*%08lx\n", (unsigned long)wire_crc32(out, pos));
    if (!put(out, cap, &pos, num))
        return 0;
    return pos;
}

int wire_number(const char *s, uint64_t *v)
{
    uint64_t n = 0;
    size_t len = strlen(s);
    size_t i;

    if (len == 0 || len > 18 || (len > 1 && s[0] == '0'))
        return 0;
    for (i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return 0;
        n = n * 10 + (uint64_t)(s[i] - '0');
    }
    *v = n;
    return 1;
}

int wire_hex(const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return 0;
    return s[n] == '\0';
}

/** The value of eight hexadecimal digits that wire_hex has checked. */
static uint32_t hex32(const char *s)
{
    uint32_t v = 0;
    int i;

    for (i = 0; i < 8; i++)
        v = (v << 4) | (uint32_t)(s[i] <= '9' ? s[i] - '0' : s[i] - 'a' + 10);
    return v;
}

int wire_parse(char *line, size_t len, struct wire_frame *f)
{
    char *word[5 + WIRE_FIELDS_MAX] = {0};
    char *crc;
    uint64_t sum;
    size_t i;
    int n = 0;

    if (len < 4 || memcmp(line, ETUDE_FRAME_MAGIC " ", 4) != 0 || memchr(line, '\0', len))
        return -1;
    for (i = 0; i < len; i++)
        if (line[i] != ' ' && (line[i] < '!' || line[i] > '~'))
            return -1;
    crc = NULL;
    for (i = len; i > 0; i--)
        if (line[i - 1] == ' ') {
            crc = line + i;
            break;
        }
    if (!crc || crc[0] != '*' || line + len - crc != 9)
        return -1;
    line[len] = '\0';
    if (!wire_hex(crc + 1, 8) || wire_crc32(line, (size_t)(crc - line)) != hex32(crc + 1))
        return -1;
    crc[-1] = '\0';
    for (i = 4; line[i];) {
        if (line[i] == ' ' || n == (int)(sizeof(word) / sizeof(word[0])))
            return -1;
        word[n++] = line + i;
        while (line[i] && line[i] != ' ')
            i++;
        if (line[i])
            line[i++] = '\0';
    }
    if (n < 5 || !wire_hex(word[0], ETUDE_FRAME_TOKEN_LEN) ||
        !wire_hex(word[1], ETUDE_FRAME_TOKEN_LEN) || !wire_number(word[2], &f->seq))
        return -1;
    f->session = word[0];
    f->boot = word[1];
    f->has_ack = strcmp(word[3], "-") != 0;
    if (f->has_ack && !wire_number(word[3], &sum))
        return -1;
    f->ack = f->has_ack ? sum : 0;
    f->type = word[4];
    f->nfields = n - 5;
    for (n = 0; n < f->nfields; n++)
        f->field[n] = word[5 + n];
    return 0;
}
