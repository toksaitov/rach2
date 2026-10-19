/*
 * frame.h: the frames that the controller and etude-guest exchange, over
 * Rach2's serial console under QEMU and over standard input and output
 * through SSH. tools/etude/Readme.md gives the commands and their rules.
 */
#ifndef ETUDE_FRAME_H
#define ETUDE_FRAME_H

/*
 * A frame is one line of printable ASCII:
 *
 *   #E1 SESSION BOOT SEQ ACK TYPE FIELD... *CRC
 *
 * SESSION and BOOT are ETUDE_FRAME_TOKEN_LEN lowercase hexadecimal digits.
 * SEQ is the sender's decimal sequence number in this session, from 0; ACK
 * is the highest SEQ received in order from the peer, or "-" before any. A
 * FIELD is one or more bytes from '!' to '~'; bytes that are not are sent in
 * base64 (RFC 4648, with padding, without line breaks). CRC is eight
 * lowercase hexadecimal digits of the CRC-32 that zlib's crc32 computes, of
 * every byte from '#' to the space before '*'.
 */
#define ETUDE_FRAME_MAGIC     "#E1"
#define ETUDE_FRAME_VERSION   1
#define ETUDE_FRAME_TOKEN_LEN 16

/* The session a frame has before the controller's hello gives one. */
#define ETUDE_FRAME_NO_SESSION "0000000000000000"

/* The longest frame, its newline included, on the serial console: one
 * console write of 256 bytes. */
#define ETUDE_FRAME_SERIAL_MAX 255

/* The longest frame either side may offer for standard input and output. */
#define ETUDE_FRAME_STDIO_MAX 65536

/* The commands, from the controller to etude-guest. */
#define ETUDE_CMD_HELLO  "hello"  /* CMD VERSIONS MAXFRAME */
#define ETUDE_CMD_PUT    "put"    /* CMD NAME SIZE SHA256 MODE */
#define ETUDE_CMD_DATA   "data"   /* CMD OFFSET BASE64, in both directions */
#define ETUDE_CMD_SEAL   "seal"   /* CMD */
#define ETUDE_CMD_RUN    "run"    /* CMD ATTEMPT INVOCATION-SHA256 */
#define ETUDE_CMD_EXEC   "exec"   /* CMD ATTEMPT INVOCATION-SHA256 */
#define ETUDE_CMD_CANCEL "cancel" /* CMD ATTEMPT */
#define ETUDE_CMD_STATUS "status" /* CMD ATTEMPT */
#define ETUDE_CMD_GET    "get"    /* CMD NAME */
#define ETUDE_CMD_REMOVE "remove" /* CMD ATTEMPT */
#define ETUDE_CMD_BYE    "bye"    /* CMD, in both directions */
#define ETUDE_CMD_ACK    "ack"    /* no fields, in both directions */

/* The events, from etude-guest to the controller. */
#define ETUDE_EV_ALIVE   "alive"   /* VERSIONS PID START */
#define ETUDE_EV_READY   "ready"   /* CMD VERSION MAXFRAME PID START */
#define ETUDE_EV_STORED  "stored"  /* CMD NAME SHA256 */
#define ETUDE_EV_STARTED "started" /* CMD ATTEMPT PID START */
#define ETUDE_EV_OUT     "out"     /* CMD ATTEMPT FD OFFSET BASE64 */
#define ETUDE_EV_EXIT    "exit"    /* CMD ATTEMPT KIND VALUE */
#define ETUDE_EV_USAGE   "usage"   /* CMD ATTEMPT CPU-US PEAK-BYTES */
#define ETUDE_EV_CLEANED "cleaned" /* CMD ATTEMPT confirmed|unconfirmed */
#define ETUDE_EV_STATE   "state"   /* CMD ATTEMPT STATE */
#define ETUDE_EV_SENT    "sent"    /* CMD NAME SIZE SHA256 */
#define ETUDE_EV_REMOVED "removed" /* CMD ATTEMPT */
#define ETUDE_EV_ERROR   "error"   /* CMD CODE */

/* The codes of an error event. */
#define ETUDE_ERR_VERSION  "version"  /* no version in common */
#define ETUDE_ERR_CONFLICT "conflict" /* a command number reused with other fields */
#define ETUDE_ERR_DAMAGED  "damaged"  /* content whose size or digest does not match */
#define ETUDE_ERR_UNKNOWN  "unknown"  /* no such attempt, file or command */
#define ETUDE_ERR_REFUSED  "refused"  /* an invocation this machine cannot run */
#define ETUDE_ERR_LIMIT    "limit"    /* a frame, a file or a workspace over its bound */
#define ETUDE_ERR_FAILED   "failed"   /* the command's own operation failed */

#endif
