/*
 * devmodel.c: a host device with pending and durable writes; see devmodel.h.
 *
 * It also answers the two calls the FAT drivers make into the block layer,
 * which `blockmax_host.c` answers for the tests that need no model: the
 * most sectors a request moves, and the device's flush, which is the
 * model's barrier here.
 */

#include "devmodel.h"
#include "block/block.h"

extern void *malloc(unsigned long n);
extern void free(void *p);
extern void *memcpy(void *d, const void *s, unsigned long n);

struct devmodel *devmodel_current;

void devmodel_init(struct devmodel *m, unsigned char *durable, unsigned char *visible,
                   uint64_t bytes)
{
    int i;

    for (i = 0; i < m->npending && m->durable != 0; i++) {
        free(m->pending[i].data);
    }
    m->durable = durable;
    m->visible = visible;
    m->bytes = bytes;
    m->npending = 0;
    m->overflow = 0;
    m->barriers = 0;
    m->fail_barrier = 0;
    m->failed = 0;
    m->writes = 0;
    m->cut_at = 0;
    m->stopped = 0;
    m->failed_at = 0;
    devmodel_current = m;
}

void devmodel_load(struct devmodel *m)
{
    int i;

    for (i = 0; i < m->npending; i++) {
        free(m->pending[i].data);
    }
    m->npending = 0;
    m->overflow = 0;
    m->barriers = 0;
    m->failed = 0;
    m->writes = 0;
    m->stopped = 0;
    memcpy(m->durable, m->visible, m->bytes);
}

int devmodel_read(struct devmodel *m, uint64_t lba, uint32_t count, void *buf)
{
    if (lba >= m->bytes / 512 || count > m->bytes / 512 - lba) {
        return -5;
    }
    memcpy(buf, m->visible + lba * 512, count * 512);
    return 0;
}

int devmodel_write(struct devmodel *m, uint64_t lba, uint32_t count, const void *buf)
{
    struct devmodel_write *w;

    if (lba >= m->bytes / 512 || count > m->bytes / 512 - lba) {
        return -5;
    }
    m->writes++;
    if (m->stopped || (m->cut_at > 0 && m->writes >= m->cut_at)) {
        m->stopped = 1;
        return -5; /* the machine stopped: this write and the rest never land */
    }
    if (m->npending == DEVMODEL_PENDING_MAX) {
        m->overflow = 1;
        return -5;
    }
    w = &m->pending[m->npending];
    w->lba = lba;
    w->count = count;
    w->data = malloc(count * 512);
    if (!w->data) {
        return -5;
    }
    memcpy(w->data, buf, count * 512);
    m->npending++;
    memcpy(m->visible + lba * 512, buf, count * 512);
    return 0;
}

/* Put pending writes 0 to `n` on the medium, in the order they were made,
   which keeps a sector's versions in order, and drop them from the list. */
static void settle(struct devmodel *m, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        memcpy(m->durable + m->pending[i].lba * 512, m->pending[i].data, m->pending[i].count * 512);
        free(m->pending[i].data);
    }
    for (i = n; i < m->npending; i++) {
        m->pending[i - n] = m->pending[i];
    }
    m->npending -= n;
}

int devmodel_barrier(struct devmodel *m)
{
    m->barriers++;
    if (m->stopped || (m->cut_at > 0 && m->writes >= m->cut_at)) {
        m->stopped = 1;
        return -5; /* stopped: no barrier lands after the cut */
    }
    if (m->fail_barrier == m->barriers) {
        /* A flush the drive reports failed part way, with the older half
           of the pending writes on the medium and the rest not; the driver
           is told, and the drive goes on answering. */
        settle(m, m->npending / 2);
        m->failed = 1;
        m->failed_at = m->writes;
        return -5;
    }
    settle(m, m->npending);
    return 0;
}

void devmodel_snapshot(const struct devmodel *m, int keep, unsigned char *out)
{
    int i, from = keep >= m->npending ? 0 : m->npending - keep;

    memcpy(out, m->durable, m->bytes);
    for (i = from; i < m->npending; i++) {
        memcpy(out + m->pending[i].lba * 512, m->pending[i].data, m->pending[i].count * 512);
    }
}

uint32_t block_max_sectors(block_device_t *device)
{
    return device->max_sectors ? device->max_sectors : BLOCK_REQ_SECTORS;
}

/** The device's flush on the host: the model's barrier. */
int block_flush(block_device_t *device)
{
    (void)device;
    return devmodel_current ? devmodel_barrier(devmodel_current) : 0;
}

/** The device's flags: a host device has no partition view over it. */
uint32_t block_flags(block_device_t *device)
{
    return device->flags;
}

/** A request with flags on the host: a fake device keeps no cache of its
 * own, so a write without the flush is the write. */
int block_request_flags(block_device_t *device, int write, uint64_t lba, uint32_t count,
                        void *buffer, uint32_t flags)
{
    (void)flags;
    return block_request(device, write, lba, count, buffer);
}
