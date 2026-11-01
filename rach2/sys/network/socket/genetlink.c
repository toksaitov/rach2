/* Generic-netlink families, nlctrl and bounded attribute encoding.
 * Wire constants follow Linux's uapi/linux/{genetlink,netlink}.h. */
#include "genetlink.h"
#include "netlink.h"
#include "../net.h"
#include "../netif.h"
#include "../driver/netdev.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"
#include "../../proc/wait_queue.h"

#define NLHDR          16u
#define MULTI          2u
#define ACK            4u
#define DUMP           0x300u
#define MSG_CAP        32448u
#define MAX_ATTR       4096u
#define CTRL_GETFAMILY 3
#define CTRL_NEWFAMILY 1

struct family_entry {
    struct family_entry *next;
    struct genl_family *family;
    unsigned users; /* calls retaining the family across a sleeping handler */
    int retiring;
    wait_queue_t waiters;
};
static struct family_entry *families;
static unsigned next_family = 0x13u;
static uint32_t next_group = 1;

/** Load a native-endian word without requiring alignment. */
static unsigned read16(const uint8_t *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

/** Load a native-endian dword without requiring alignment. */
static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)read16(p) | ((uint32_t)read16(p + 2) << 16);
}

/** Store a native-endian word without requiring alignment. */
static void write16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/** Store a native-endian dword without requiring alignment. */
static void write32(uint8_t *p, uint32_t v)
{
    write16(p, (unsigned)v);
    write16(p + 2, (unsigned)(v >> 16));
}

/** Round an already bounded length up to the wire alignment. */
static unsigned aligned(unsigned n)
{
    return (n + 3u) & ~3u;
}

void genl_msg_init(struct genl_msg *msg, uint8_t *buf, unsigned cap)
{
    k_memset(msg, 0, sizeof(*msg));
    msg->data = buf;
    msg->cap = cap;
}

/** Retain the first append error and return it. */
static int fail(struct genl_msg *msg, int error)
{
    if (!msg->error) {
        msg->error = error;
    }
    return msg->error;
}

int genl_msg_begin(struct genl_msg *msg, const struct genl_family *family, uint8_t cmd,
                   uint32_t seq, uint16_t flags, uint8_t **userhdr)
{
    unsigned size;
    uint8_t *p;

    if (userhdr) {
        *userhdr = 0;
    }
    if (msg->error) {
        return msg->error;
    }
    if (!family || msg->in_message || family->hdrsize > MSG_CAP - NLHDR - GENL_HDRLEN) {
        return fail(msg, -EINVAL);
    }
    size = NLHDR + GENL_HDRLEN + aligned(family->hdrsize);
    if (msg->len > msg->cap || size > msg->cap - msg->len) {
        return fail(msg, -EMSGSIZE);
    }
    msg->message_start = msg->len;
    msg->in_message = 1;
    p = msg->data + msg->len;
    k_memset(p, 0, size);
    write32(p, size);
    write16(p + 4, family->id);
    write16(p + 6, flags);
    write32(p + 8, seq);
    p[16] = cmd;
    p[17] = family->version;
    if (userhdr) {
        *userhdr = p + 20;
    }
    msg->len += size;
    return 0;
}

int genl_msg_end(struct genl_msg *msg)
{
    if (msg->error) {
        return msg->error;
    }
    if (!msg->in_message) {
        return fail(msg, -EINVAL);
    }
    write32(msg->data + msg->message_start, msg->len - msg->message_start);
    msg->in_message = 0;
    return 0;
}

int nla_put(struct genl_msg *msg, unsigned type, const void *data, unsigned len)
{
    unsigned size;
    uint8_t *p;

    if (msg->error) {
        return msg->error;
    }
    if (len > 65531u || type > 65535u || (len && !data)) {
        return fail(msg, -EINVAL);
    }
    size = aligned(len + NLA_HDRLEN);
    if (msg->len > msg->cap || size > msg->cap - msg->len) {
        return fail(msg, -EMSGSIZE);
    }
    p = msg->data + msg->len;
    k_memset(p, 0, size);
    write16(p, len + NLA_HDRLEN);
    write16(p + 2, type);
    if (len) {
        k_memcpy(p + NLA_HDRLEN, data, len);
    }
    msg->len += size;
    return 0;
}

int nla_put_u8(struct genl_msg *msg, unsigned type, uint8_t value)
{
    return nla_put(msg, type, &value, 1);
}

int nla_put_u16(struct genl_msg *msg, unsigned type, uint16_t value)
{
    uint8_t p[2];

    write16(p, value);
    return nla_put(msg, type, p, 2);
}

int nla_put_u32(struct genl_msg *msg, unsigned type, uint32_t value)
{
    uint8_t p[4];

    write32(p, value);
    return nla_put(msg, type, p, 4);
}

int nla_put_u64(struct genl_msg *msg, unsigned type, uint64_t value)
{
    uint8_t p[8];

    write32(p, (uint32_t)value);
    write32(p + 4, (uint32_t)(value >> 32));
    return nla_put(msg, type, p, 8);
}

int nla_put_flag(struct genl_msg *msg, unsigned type)
{
    return nla_put(msg, type, 0, 0);
}

int nla_put_string(struct genl_msg *msg, unsigned type, const char *string)
{
    unsigned len = 0;

    if (!string) {
        return fail(msg, -EINVAL);
    }
    while (len < 65531u && string[len]) {
        ++len;
    }
    return len == 65531u ? fail(msg, -EINVAL) : nla_put(msg, type, string, len + 1);
}

unsigned nla_nest_start(struct genl_msg *msg, unsigned type)
{
    unsigned offset = msg->len;

    if (nla_put(msg, type | NLA_F_NESTED, 0, 0)) {
        return GENL_NEST_INVALID;
    }
    return offset;
}

int nla_nest_end(struct genl_msg *msg, unsigned offset)
{
    if (msg->error) {
        return msg->error;
    }
    if (offset > msg->len || msg->len - offset < 4 || msg->len - offset > 65535u) {
        return fail(msg, -EINVAL);
    }
    write16(msg->data + offset, msg->len - offset);
    return 0;
}

void nla_nest_cancel(struct genl_msg *msg, unsigned offset)
{
    if (offset <= msg->len) {
        msg->len = offset;
    }
}

const uint8_t *nla_data(const struct nlattr *attr)
{
    return (const uint8_t *)attr + NLA_HDRLEN;
}

unsigned nla_len(const struct nlattr *attr)
{
    return read16((const uint8_t *)attr) - NLA_HDRLEN;
}

unsigned nla_type(const struct nlattr *attr)
{
    return read16((const uint8_t *)attr + 2) & NLA_TYPE_MASK;
}

uint8_t nla_get_u8(const struct nlattr *attr)
{
    return nla_data(attr)[0];
}

uint16_t nla_get_u16(const struct nlattr *attr)
{
    return (uint16_t)read16(nla_data(attr));
}

uint32_t nla_get_u32(const struct nlattr *attr)
{
    return read32(nla_data(attr));
}

uint64_t nla_get_u64(const struct nlattr *attr)
{
    return (uint64_t)read32(nla_data(attr)) | ((uint64_t)read32(nla_data(attr) + 4) << 32);
}

/** Validate a policy's payload size and string termination. */
static int policy_ok(const struct nlattr *attr, const struct nla_policy *policy)
{
    unsigned len = nla_len(attr), exact = 0;

    if (len < policy->min_len || (policy->max_len && len > policy->max_len)) {
        return 0;
    }
    switch (policy->type) {
    case GENL_NLA_U8:
    case GENL_NLA_S8:
        exact = 1;
        break;
    case GENL_NLA_U16:
    case GENL_NLA_S16:
        exact = 2;
        break;
    case GENL_NLA_U32:
    case GENL_NLA_S32:
        exact = 4;
        break;
    case GENL_NLA_U64:
    case GENL_NLA_S64:
        exact = 8;
        break;
    case GENL_NLA_FLAG:
        return len == 0;
    case GENL_NLA_STRING:
        return len && nla_data(attr)[len - 1] == 0 &&
               k_strlen((const char *)nla_data(attr)) == len - 1;
    case GENL_NLA_NESTED:
    case GENL_NLA_UNSPEC:
    case GENL_NLA_BINARY:
        return 1;
    default:
        return 0;
    }
    return len == exact;
}

int nla_parse(const uint8_t *data, unsigned len, const struct nla_policy *policy, unsigned maxattr,
              const struct nlattr **table, unsigned *bad_offset)
{
    unsigned at = 0;

    if (!table || maxattr > MAX_ATTR) {
        return -EINVAL;
    }
    k_memset(table, 0, (maxattr + 1) * sizeof(*table));
    if (bad_offset) {
        *bad_offset = 0;
    }
    while (at < len) {
        const struct nlattr *attr;
        unsigned size, type;

        if (!data || len - at < NLA_HDRLEN) {
            break;
        }
        attr = (const struct nlattr *)(const void *)(data + at);
        size = read16(data + at);
        type = nla_type(attr);
        if (size < NLA_HDRLEN || size > len - at || aligned(size) > len - at) {
            break;
        }
        if (type <= maxattr) {
            if (table[type] || (policy && !policy_ok(attr, &policy[type]))) {
                break;
            }
            table[type] = attr;
        }
        at += aligned(size);
    }
    if (at == len) {
        return 0;
    }
    if (bad_offset) {
        *bad_offset = at;
    }
    k_memset(table, 0, (maxattr + 1) * sizeof(*table));
    return -EINVAL;
}

int nla_parse_nested(const struct nlattr *attr, const struct nla_policy *policy, unsigned maxattr,
                     const struct nlattr **table, unsigned *bad_offset)
{
    return nla_parse(nla_data(attr), nla_len(attr), policy, maxattr, table, bad_offset);
}

/** Compare terminated family or group names byte by byte. */
static int name_compare(const char *a, const char *b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/** Whether name contains 1 to GENL_NAMSIZ-1 bytes before its terminator. */
static int valid_name(const char *name)
{
    unsigned n;

    if (!name || !name[0]) {
        return 0;
    }
    for (n = 1; n < GENL_NAMSIZ; ++n) {
        if (!name[n]) {
            return 1;
        }
    }
    return 0;
}

int genl_register_family(struct genl_family *family)
{
    struct family_entry *entry, *e;
    unsigned i, j;
    uint32_t lock;

    if (!family || !valid_name(family->name) || family->id ||
        !name_compare(family->name, "nlctrl") || family->maxattr > MAX_ATTR ||
        family->hdrsize > MSG_CAP - 20u || (family->n_ops && !family->ops) ||
        (family->n_groups && !family->groups)) {
        return -EINVAL;
    }
    for (i = 0; i < family->n_ops; ++i) {
        if (!family->ops[i].doit && !family->ops[i].dumpit) {
            return -EINVAL;
        }
        for (j = 0; j < i; ++j) {
            if (family->ops[i].cmd == family->ops[j].cmd) {
                return -EINVAL;
            }
        }
    }
    for (i = 0; i < family->n_groups; ++i) {
        if (!valid_name(family->groups[i].name)) {
            return -EINVAL;
        }
        for (j = 0; j < i; ++j) {
            if (!name_compare(family->groups[i].name, family->groups[j].name)) {
                return -EINVAL;
            }
        }
    }
    entry = kzalloc(sizeof(*entry));
    if (!entry) {
        return -ENOMEM;
    }
    lock = net_lock();
    for (e = families; e; e = e->next) {
        if (e->family == family || !name_compare(e->family->name, family->name)) {
            net_unlock(lock);
            kfree(entry);
            return -EEXIST;
        }
    }
    if (next_family > 1023u || family->n_groups > 0x7fffffffu - next_group) {
        net_unlock(lock);
        kfree(entry);
        return -ENOSPC;
    }
    family->id = (uint16_t)next_family++;
    for (i = 0; i < family->n_groups; ++i) {
        family->groups[i].id = next_group++;
    }
    entry->family = family;
    entry->next = families;
    families = entry;
    net_unlock(lock);
    return 0;
}

/** Whether no handler still borrows a retiring family's storage. */
static int unused(void *arg)
{
    return ((struct family_entry *)arg)->users == 0;
}

void genl_unregister_family(struct genl_family *family)
{
    struct family_entry **link, *entry;
    uint32_t lock = net_lock();
    unsigned i;

    for (link = &families; *link && (*link)->family != family; link = &(*link)->next) {
    }
    entry = *link;
    if (!entry || entry->retiring) {
        net_unlock(lock);
        return;
    }
    entry->retiring = 1;
    net_unlock(lock);
    (void)wq_wait_event_nointr(&entry->waiters, unused, entry);
    lock = net_lock();
    for (link = &families; *link != entry; link = &(*link)->next) {
    }
    *link = entry->next;
    family->id = 0;
    for (i = 0; i < family->n_groups; ++i) {
        family->groups[i].id = 0;
    }
    net_unlock(lock);
    kfree(entry);
}

int genl_group_exists(uint32_t group)
{
    struct family_entry *e;
    uint32_t lock = net_lock();
    int found = 0;
    unsigned i;

    for (e = families; e && !found; e = e->next) {
        for (i = 0; !e->retiring && i < e->family->n_groups; ++i) {
            if (e->family->groups[i].id == group) {
                found = 1;
                break;
            }
        }
    }
    net_unlock(lock);
    return found;
}

/** Validate a completed datagram before a transport helper borrows it. */
static int complete(const struct genl_msg *msg)
{
    unsigned at = 0;

    if (msg->error || msg->in_message || msg->len > msg->cap || !msg->len) {
        return msg->error ? msg->error : -EINVAL;
    }
    while (at < msg->len) {
        unsigned len;

        if (msg->len - at < NLHDR) {
            return -EINVAL;
        }
        len = read32(msg->data + at);
        if (len < NLHDR || len > msg->len - at || aligned(len) > msg->len - at) {
            return -EINVAL;
        }
        at += aligned(len);
    }
    return 0;
}

int genl_reply(const struct genl_info *info, const struct genl_msg *msg)
{
    int rc = complete(msg);

    return rc ? rc : netlink_genl_reply(info->socket, info->portid, msg->data, msg->len);
}

int genl_multicast(const struct genl_family *family, unsigned group_index,
                   const struct genl_msg *msg)
{
    struct family_entry *e;
    uint32_t lock;
    int found = 0, rc = complete(msg);

    if (rc) {
        return rc;
    }
    lock = net_lock();
    for (e = families; e; e = e->next) {
        if (e->family == family && !e->retiring) {
            found = 1;
            break;
        }
    }
    if (!found || group_index >= family->n_groups) {
        net_unlock(lock);
        return -ENOENT;
    }
    rc = netlink_genl_multicast(family->groups[group_index].id, msg->data, msg->len);
    net_unlock(lock);
    return rc;
}

static struct genl_family controller = {"nlctrl", GENL_ID_CTRL, 2, 0, 7, 0, 0, 0, 0, 0, 0};

/** Append a controller description, including operation and group nests. */
static int describe(struct genl_msg *msg, const struct genl_family *f, uint32_t seq, uint16_t flags)
{
    unsigned i, nest, item;

    genl_msg_begin(msg, &controller, CTRL_NEWFAMILY, seq, flags, 0);
    nla_put_u16(msg, 1, f->id);
    nla_put_string(msg, 2, f->name);
    nla_put_u32(msg, 3, f->version);
    nla_put_u32(msg, 4, f->hdrsize);
    nla_put_u32(msg, 5, f->maxattr);
    if (f->n_ops) {
        nest = nla_nest_start(msg, 6);
        for (i = 0; i < f->n_ops; ++i) {
            const struct genl_ops *op = &f->ops[i];
            unsigned caps = op->flags | (op->doit ? 2u : 0) | (op->dumpit ? 4u : 0) |
                            ((op->policy || f->policy) ? 8u : 0);

            item = nla_nest_start(msg, i + 1);
            nla_put_u32(msg, 1, op->cmd);
            nla_put_u32(msg, 2, caps);
            nla_nest_end(msg, item);
        }
        nla_nest_end(msg, nest);
    }
    if (f->n_groups) {
        nest = nla_nest_start(msg, 7);
        for (i = 0; i < f->n_groups; ++i) {
            item = nla_nest_start(msg, i + 1);
            nla_put_string(msg, 1, f->groups[i].name);
            nla_put_u32(msg, 2, f->groups[i].id);
            nla_nest_end(msg, item);
        }
        nla_nest_end(msg, nest);
    }
    return genl_msg_end(msg);
}

/** Emit a terminal dump message, carrying a negative status on failure. */
static int dump_done(const struct genl_info *info, int error)
{
    uint8_t buf[20];

    k_memset(buf, 0, sizeof(buf));
    write32(buf, sizeof(buf));
    write16(buf + 4, 3);
    write16(buf + 6, MULTI);
    write32(buf + 8, info->seq);
    write32(buf + 12, info->portid);
    write32(buf + 16, (uint32_t)error);
    return netlink_genl_reply(info->socket, info->portid, buf, sizeof(buf));
}

/** Resolve one family or enumerate nlctrl and the registered families. */
static int control(const struct genl_info *info, uint8_t *buf)
{
    struct family_entry *e;
    const struct genl_family *f = 0;
    struct genl_msg msg;
    int dump = (info->flags & DUMP) == DUMP, rc;
    unsigned id = info->attrs[1] ? nla_get_u16(info->attrs[1]) : 0;
    const char *name = info->attrs[2] ? (const char *)nla_data(info->attrs[2]) : 0;

    if (info->cmd != CTRL_GETFAMILY) {
        return -EOPNOTSUPP;
    }
    if (dump) {
        f = &controller;
    } else if ((name && !name_compare(name, controller.name)) || (!name && id == GENL_ID_CTRL)) {
        f = &controller;
    }
    genl_msg_init(&msg, buf, MSG_CAP);
    if (f) {
        describe(&msg, f, info->seq, dump ? MULTI : 0);
    }
    for (e = families; e; e = e->next) {
        if (e->retiring ||
            (!dump && (name ? name_compare(name, e->family->name) != 0 : id != e->family->id))) {
            continue;
        }
        f = e->family;
        if (msg.len) {
            rc = genl_reply(info, &msg);
            if (rc) {
                return rc;
            }
        }
        genl_msg_init(&msg, buf, MSG_CAP);
        describe(&msg, f, info->seq, dump ? MULTI : 0);
        if (!dump) {
            break;
        }
    }
    if (!f) {
        return name || id ? -ENOENT : -EINVAL;
    }
    rc = genl_reply(info, &msg);
    return rc || !dump ? rc : dump_done(info, 0);
}

/** Run a dump to completion, with each callback's messages in one datagram.
 * Dispatch retains the request and family through sleeping handlers. */
static int run_dump(struct genl_info *info, const struct genl_ops *op, uint8_t *buf)
{
    struct genl_dump dump;
    struct genl_msg msg;
    int more, rc = 0;

    k_memset(&dump, 0, sizeof(dump));
    dump.info = *info;
    do {
        unsigned at;

        genl_msg_init(&msg, buf, MSG_CAP);
        more = op->dumpit(&dump, &msg);
        if (more < 0) {
            rc = more;
            break;
        }
        rc = msg.len ? complete(&msg) : msg.error;
        if (rc || (more && !msg.len)) {
            rc = rc ? rc : -EMSGSIZE;
            break;
        }
        for (at = 0; at < msg.len; at += aligned(read32(buf + at))) {
            write16(buf + at + 6, read16(buf + at + 6) | MULTI);
            write32(buf + at + 8, info->seq);
            write32(buf + at + 12, info->portid);
        }
        if (msg.len) {
            rc = genl_reply(info, &msg);
        }
    } while (more && !rc);
    if (op->done) {
        op->done(&dump);
    }
    {
        int sent = dump_done(info, rc);

        return sent;
    }
}

int genl_dispatch(struct socket *sock, uint32_t portid, const uint8_t *req, unsigned len)
{
    static const struct nla_policy ctrl_policy[8] = {
        {GENL_NLA_UNSPEC, 0, 0}, {GENL_NLA_U16, 0, 0},   {GENL_NLA_STRING, 1, GENL_NAMSIZ},
        {GENL_NLA_U32, 0, 0},    {GENL_NLA_U32, 0, 0},   {GENL_NLA_U32, 0, 0},
        {GENL_NLA_NESTED, 0, 0}, {GENL_NLA_NESTED, 0, 0}};
    struct family_entry *entry = 0, *e;
    struct genl_info info;
    const struct genl_ops *op = 0;
    const struct nla_policy *policy;
    const struct nlattr **attrs = 0;
    uint8_t *buf = 0;
    const char *detail = 0;
    unsigned bad = 0, offset, i;
    uint32_t lock;
    int rc = -ENOENT, sent;

    k_memset(&info, 0, sizeof(info));
    info.socket = sock;
    info.portid = portid;
    info.seq = read32(req + 8);
    info.flags = (uint16_t)read16(req + 6);
    info.extack = &detail;
    info.bad_offset = &bad;
    lock = net_lock();
    if (read16(req + 4) == GENL_ID_CTRL) {
        info.family = &controller;
    } else {
        for (e = families; e; e = e->next) {
            if (!e->retiring && e->family->id == read16(req + 4)) {
                entry = e;
                ++entry->users;
                info.family = e->family;
                break;
            }
        }
    }
    net_unlock(lock);
    if (!info.family) {
        detail = "unknown generic-netlink family";
        goto out;
    }
    offset = 20u + aligned(info.family->hdrsize);
    if (len < offset) {
        rc = -EINVAL;
        detail = "short generic-netlink header";
        goto out;
    }
    info.cmd = req[16];
    info.version = req[17];
    info.userhdr = req + 20;
    info.payload = req + offset;
    info.payload_len = len - offset;
    policy = info.family == &controller ? ctrl_policy : info.family->policy;
    if (entry) {
        for (i = 0; i < info.family->n_ops; ++i) {
            if (info.family->ops[i].cmd == info.cmd) {
                op = &info.family->ops[i];
                break;
            }
        }
        if (!op || (((info.flags & DUMP) == DUMP) ? !op->dumpit : !op->doit)) {
            rc = -EOPNOTSUPP;
            detail = "unsupported generic-netlink operation";
            goto out;
        }
        if ((op->flags & GENL_ADMIN_PERM) && !net_admin()) {
            rc = -EPERM;
            goto out;
        }
        if (op->policy) {
            policy = op->policy;
        }
    }
    attrs = kmalloc((info.family->maxattr + 1u) * sizeof(*attrs));
    if (!attrs) {
        rc = -ENOMEM;
        goto out;
    }
    info.attrs = attrs;
    rc = nla_parse(info.payload, info.payload_len, policy, info.family->maxattr, attrs, &bad);
    if (rc) {
        bad += offset;
        detail = "invalid generic-netlink attribute";
        goto out;
    }
    if (!entry || (info.flags & DUMP) == DUMP) {
        buf = kmalloc(MSG_CAP);
        if (!buf) {
            rc = -ENOMEM;
            goto out;
        }
        rc = !entry ? control(&info, buf) : run_dump(&info, op, buf);
    } else {
        rc = op->doit(&info);
    }
out:
    if (rc || (info.flags & ACK)) {
        sent = netlink_ack(sock, req, rc, detail, bad);
    } else {
        sent = 0;
    }
    kfree(buf);
    kfree(attrs);
    if (entry) {
        lock = net_lock();
        --entry->users;
        wq_wake_all(&entry->waiters);
        net_unlock(lock);
    }
    return sent;
}
