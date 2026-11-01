/* Generic-netlink family registration, request dispatch and attribute helpers. */
#ifndef _GENETLINK_H_
#define _GENETLINK_H_

#include "../../lib/kint.h"

struct socket;
struct genl_family;
struct genl_info;
struct genl_dump;
struct genl_msg;

#define GENL_NAMSIZ         16
#define GENL_ID_CTRL        0x10
#define GENL_HDRLEN         4
#define GENL_ADMIN_PERM     0x01
#define NLA_F_NESTED        0x8000
#define NLA_F_NET_BYTEORDER 0x4000
#define NLA_TYPE_MASK       0x3fff
#define NLA_HDRLEN          4
#define NLA_ALIGNTO         4
#define GENL_NEST_INVALID   0xffffffffu

/* These wire headers use native-endian integers, as Linux netlink does.
 * Helpers read unaligned attributes by bytes, not by casting a datagram. */
struct genlmsghdr {
    uint8_t cmd, version;
    uint16_t reserved;
};
struct nlattr {
    uint16_t nla_len;  /* header plus payload, excluding alignment padding */
    uint16_t nla_type; /* id plus NLA_F_* */
};

enum genl_nla_type {
    GENL_NLA_UNSPEC,
    GENL_NLA_U8,
    GENL_NLA_U16,
    GENL_NLA_U32,
    GENL_NLA_U64,
    GENL_NLA_S8,
    GENL_NLA_S16,
    GENL_NLA_S32,
    GENL_NLA_S64,
    GENL_NLA_FLAG,
    GENL_NLA_STRING,
    GENL_NLA_BINARY,
    GENL_NLA_NESTED
};
struct nla_policy {
    enum genl_nla_type type;
    uint16_t min_len, max_len; /* payload bytes; zero max means no extra limit */
};
struct genl_multicast_group {
    const char *name; /* static, at most GENL_NAMSIZ-1 bytes */
    uint32_t id;      /* assigned at registration, globally unique, may exceed 32 */
};
struct genl_ops {
    uint8_t cmd;
    unsigned flags;                  /* GENL_ADMIN_PERM checks net_admin before either handler */
    const struct nla_policy *policy; /* maxattr+1 entries; NULL uses family's */
    /** Process one request; info and attributes borrow its datagram.
     * Return 0 or negative errno. Explicit replies use genl_reply; dispatch
     * sends the requested ACK, or an error, after return. No retained info. */
    int (*doit)(const struct genl_info *info);
    /** Append complete messages to msg, advancing dump->args only after a
     * message fits. Return 1 for more, 0 when complete or negative errno.
     * Never emit NLMSG_DONE: dispatch owns framing and terminal errors. */
    int (*dumpit)(struct genl_dump *dump, struct genl_msg *msg);
    /** Optional cleanup after success, error or socket close; called once
     * for a started dump. Must not retain dump after return. */
    void (*done)(struct genl_dump *dump);
};

/* Family and all referenced tables remain writable/live until unregister.
 * Dispatch calls handlers in process context under the kernel lock, without
 * net_lock. Handlers may sleep but must copy a request before queuing work.
 * Registry and delivery helpers below take net_lock internally as needed. */
struct genl_family {
    const char *name; /* static, 1 to GENL_NAMSIZ-1 non-NUL bytes */
    uint16_t id;      /* assigned on registration; caller initializes to zero */
    uint8_t version;
    unsigned hdrsize; /* family-specific bytes following genlmsghdr */
    unsigned maxattr;
    const struct nla_policy *policy; /* maxattr+1 entries, indexed by id */
    const struct genl_ops *ops;
    unsigned n_ops;
    struct genl_multicast_group *groups;
    unsigned n_groups;
    void *priv; /* family-owned */
    /** Optional: forget what the generic socket of portid left behind, such
     * as its frame registrations, once it closes. Process context, kernel
     * lock held, no net_lock; must not sleep. */
    void (*release)(uint32_t portid);
};
struct genl_info {
    struct socket *socket; /* borrowed; use portid/seq for the reply */
    struct genl_family *family;
    uint32_t portid, seq;
    uint16_t flags; /* request's NLM_F_* */
    uint8_t cmd, version;
    const uint8_t *userhdr;      /* hdrsize validated bytes */
    const struct nlattr **attrs; /* maxattr+1 entries, NULL means absent */
    const uint8_t *payload;      /* first attribute, after aligned user header */
    unsigned payload_len;
    /* Optional extended ACK, borrowed static text and offending byte offset
     * from the request's nlmsghdr; zero offset means no attribute offset. */
    const char **extack;
    unsigned *bad_offset;
};
struct genl_dump {
    struct genl_info info; /* stable copy; request storage lives until done */
    unsigned long args[6]; /* handler cursor, initially zero */
    void *priv;            /* handler-owned, released by done */
};
struct genl_msg {
    uint8_t *data;                      /* caller-owned buffer */
    unsigned len, cap;                  /* bytes used and available */
    int error;                          /* sticky negative errno; a failed append writes no bytes */
    unsigned message_start, in_message; /* private to begin/end */
};

/** Register a family and its globally numbered multicast groups.
 * Return 0, -EINVAL, -EEXIST, -ENOSPC or -ENOMEM; failure registers nothing.
 * Process context, kernel lock held, no net_lock; nlctrl is reserved. */
int genl_register_family(struct genl_family *family);

/** Stop dispatch and delivery, finish live dumps and remove family/groups.
 * Process context, kernel lock held, no net_lock; may sleep. Storage can be
 * freed on return. Safe for a family that never completed registration. */
void genl_unregister_family(struct genl_family *family);

/** Initialize a borrowed message buffer, resetting length and sticky error.
 * These buffer/attribute helpers need no locks, allocate nothing, never
 * sleep and retain no pointers beyond their explicit output structures. */
void genl_msg_init(struct genl_msg *msg, uint8_t *buf, unsigned cap);

/** Begin a complete nlmsghdr/genlmsghdr message in msg, using info's seq
 * and family id (or seq zero for a notification). Return 0 or sticky error.
 * hdrsize bytes are zeroed; fill them through returned userhdr when non-NULL.
 * The message is finalized by genl_msg_end before another is begun. */
int genl_msg_begin(struct genl_msg *msg, const struct genl_family *family, uint8_t cmd,
                   uint32_t seq, uint16_t flags, uint8_t **userhdr);

/** Finalize the most recently begun message: update nlmsg_len and pad to 4.
 * Return 0 or sticky error; an unmatched begin is -EINVAL. */
int genl_msg_end(struct genl_msg *msg);

/** Send complete messages as one reply to info's port id. Preserve msg and
 * its storage; return 0 or negative errno. Process context, no net_lock. */
int genl_reply(const struct genl_info *info, const struct genl_msg *msg);

/** Deliver msg to every member of family group at group_index, from zero.
 * Return 0 with no members, or negative errno; an overrun is also recorded
 * as ENOBUFS at each affected socket. Borrow msg only until return.
 * Kernel lock held, no net_lock; safe on the wireless worker. */
int genl_multicast(const struct genl_family *family, unsigned group_index,
                   const struct genl_msg *msg);

/** Send msg to the one generic socket of portid, as an event a listener
 * registered for. Return 0, -ECONNREFUSED when no socket has that port,
 * -ENOBUFS when its queue is full, or a message error. Borrows msg only
 * until return. Kernel lock held, no net_lock; safe on the wireless worker. */
int genl_unicast(const struct genl_family *family, uint32_t portid, const struct genl_msg *msg);

/** Tell every family with a release call that the generic socket of portid
 * has closed. Process context, kernel lock held, no net_lock. */
void genl_release(uint32_t portid);

/**
 * Parse len bounded attribute bytes into maxattr+1 table entries.
 * Clear the table first and again on failure. Reject truncated headers,
 * payloads, missing alignment bytes, duplicate known ids and policy errors.
 * Unknown ids above maxattr are skipped. Values borrow data until it changes.
 * Return 0 or -EINVAL; on failure set *bad_offset when non-NULL to the
 * attribute's byte offset from data. policy may be NULL for bounds only.
 */
int nla_parse(const uint8_t *data, unsigned len, const struct nla_policy *policy, unsigned maxattr,
              const struct nlattr **table, unsigned *bad_offset);

/** Parse an already bounded nested attribute's payload by the same rules.
 * Return 0 or -EINVAL, with offsets relative to its payload. */
int nla_parse_nested(const struct nlattr *attr, const struct nla_policy *policy, unsigned maxattr,
                     const struct nlattr **table, unsigned *bad_offset);

/** Payload address of a validated attribute, borrowed from the datagram. */
const uint8_t *nla_data(const struct nlattr *attr);
/** Payload length of a validated attribute, excluding header and padding. */
unsigned nla_len(const struct nlattr *attr);
/** Attribute id without NLA_F_*; attr must already be bounds-checked. */
unsigned nla_type(const struct nlattr *attr);
/** Read a policy-validated native-endian u8 payload. */
uint8_t nla_get_u8(const struct nlattr *attr);
/** Read a policy-validated native-endian u16 payload without alignment needs. */
uint16_t nla_get_u16(const struct nlattr *attr);
/** Read a policy-validated native-endian u32 payload without alignment needs. */
uint32_t nla_get_u32(const struct nlattr *attr);
/** Read a policy-validated native-endian u64 payload without alignment needs. */
uint64_t nla_get_u64(const struct nlattr *attr);

/** Append a bounded attribute and zero its 4-byte alignment padding.
 * Return 0 or sticky -EINVAL/-EMSGSIZE, leaving msg bytes/len unchanged on
 * error. data may be NULL only at len zero; inputs must not overlap msg. */
int nla_put(struct genl_msg *msg, unsigned type, const void *data, unsigned len);
/** Append a native-endian u8 attribute, returning 0 or sticky error. */
int nla_put_u8(struct genl_msg *msg, unsigned type, uint8_t value);
/** Append a native-endian u16 attribute, returning 0 or sticky error. */
int nla_put_u16(struct genl_msg *msg, unsigned type, uint16_t value);
/** Append a native-endian u32 attribute, returning 0 or sticky error. */
int nla_put_u32(struct genl_msg *msg, unsigned type, uint32_t value);
/** Append a native-endian u64 attribute, returning 0 or sticky error. */
int nla_put_u64(struct genl_msg *msg, unsigned type, uint64_t value);
/** Append a zero-payload flag attribute, returning 0 or sticky error. */
int nla_put_flag(struct genl_msg *msg, unsigned type);
/** Append string including its final NUL, returning 0 or sticky error. */
int nla_put_string(struct genl_msg *msg, unsigned type, const char *string);
/** Begin a nested attribute, returning its offset or GENL_NEST_INVALID.
 * Sets NLA_F_NESTED. Finish or cancel nests in reverse start order. */
unsigned nla_nest_start(struct genl_msg *msg, unsigned type);
/** Finish the nest at offset, returning 0 or sticky error. */
int nla_nest_end(struct genl_msg *msg, unsigned offset);
/** Discard a nest and its children, restoring len to offset, retaining error. */
void nla_nest_cancel(struct genl_msg *msg, unsigned offset);

/** Internal transport entry: dispatch one complete request in process context.
 * Return 0 once its answer is queued, or a transport error. */
int genl_dispatch(struct socket *sock, uint32_t portid, const uint8_t *req, unsigned len);
/** Whether a multicast group is registered; safe under net_lock. */
int genl_group_exists(uint32_t group);

#endif
