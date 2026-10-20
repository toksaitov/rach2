/*
 * wire.h: the codec of etude's frames (include/etude/frame.h): CRC-32,
 * base64, and the encoding and parsing of one frame. etude-guest, the
 * responder and the controller's side of remote execution use it.
 */
#ifndef ETUDE_WIRE_H
#define ETUDE_WIRE_H

#include <stddef.h>
#include <stdint.h>

/* The most fields a frame may have after its type: a `stream`'s six. */
#define WIRE_FIELDS_MAX 6

/* One parsed frame. The strings point into the line the parser was given. */
struct wire_frame {
    const char *session; /* ETUDE_FRAME_TOKEN_LEN hexadecimal digits */
    const char *boot;    /* the same */
    uint64_t seq;
    int has_ack; /* 0 for the "-" of a sender that has received nothing */
    uint64_t ack;
    const char *type;
    int nfields;
    const char *field[WIRE_FIELDS_MAX];
};

/** The CRC-32 of `n` bytes at `p`, as zlib's crc32(0, p, n) computes it. */
uint32_t wire_crc32(const void *p, size_t n);

/** The length of the base64 text of `n` bytes, padding included. */
size_t wire_base64_len(size_t n);

/** Write the base64 text of `n` bytes at `in` to `out`, which has room for
 * wire_base64_len(n) bytes and a terminator. */
void wire_base64(const unsigned char *in, size_t n, char *out);

/**
 * Decode the padded base64 text `in` into `out`, which has room for
 * 3 * strlen(in) / 4 bytes.
 *
 * @return the bytes decoded, or -1 for text that is not padded base64 or has
 * bits set past its last byte.
 */
long wire_unbase64(const char *in, unsigned char *out);

/**
 * Encode a frame into `out`, of `cap` bytes, with its newline and a
 * terminator.
 *
 * `ack` is ignored when `has_ack` is 0. Each of the `nfields` strings of
 * `field` must be one or more bytes from '!' to '~', and so must `type`.
 *
 * @return the frame's length with its newline, or 0 when a field is not
 * printable or the frame does not fit in `cap` less the terminator.
 */
size_t wire_encode(char *out, size_t cap, const char *session, const char *boot, uint64_t seq,
                   int has_ack, uint64_t ack, const char *type, int nfields,
                   const char *const *field);

/**
 * Parse the frame `line`, of `len` bytes without its newline, in place.
 *
 * The spaces of the line become terminators, so the strings of `f` point into
 * it. The magic, the tokens, the decimal numbers, the printable fields and the
 * CRC are checked.
 *
 * @return 0 for a frame, or -1 for a line that is not a valid frame or has
 * more than WIRE_FIELDS_MAX fields.
 */
int wire_parse(char *line, size_t len, struct wire_frame *f);

/** Whether `s` is a decimal number of at most 18 digits; its value goes to
 * `*v`. */
int wire_number(const char *s, uint64_t *v);

/** Whether `s` is `n` lowercase hexadecimal digits and nothing else. */
int wire_hex(const char *s, size_t n);

#endif
