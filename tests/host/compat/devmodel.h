/*
 * devmodel.h: a host device with pending and durable writes.
 *
 * A drive with a cache answers a write before the bytes are on the medium
 * and keeps them pending until a flush; a crash between two flushes keeps
 * some of what was pending and loses the rest, in an order the drive chose.
 * The FAT host tests write through this model so that a barrier the driver
 * leaves out changes what a crash leaves behind: reads see every write, as
 * a drive's cache answers them; `block_flush` is the barrier that makes the
 * pending writes durable, or fails at the Nth call and keeps half of them;
 * and a snapshot at a cut is the durable image with none, only the
 * newest, or all of the writes pending since the last barrier, each a state
 * the medium can be in after a crash there.
 */
#ifndef DEVMODEL_H
#define DEVMODEL_H

#include "lib/kint.h"

/* The most writes pending between two barriers; one more sets `overflow`. */
#define DEVMODEL_PENDING_MAX 4096

/* What a snapshot keeps of the writes pending at the cut: the newest N of
   them, from none to all. A drive that keeps writes pending may put the
   later ones on the medium before the earlier, so each suffix of the
   pending list is a state a crash can leave; the most a stop judges is
   `DEVMODEL_KEEP_MOST` suffixes beside none and all. */
#define DEVMODEL_KEEP_NONE 0
#define DEVMODEL_KEEP_MOST 6

struct devmodel_write {
    uint64_t lba;
    uint32_t count;
    unsigned char *data;
};

struct devmodel {
    unsigned char *durable; /* the medium */
    unsigned char *visible; /* the medium with the pending writes over it */
    uint64_t bytes;
    struct devmodel_write pending[DEVMODEL_PENDING_MAX];
    int npending;
    int overflow;     /* more pending writes than the list holds */
    int barriers;     /* flushes made since the load */
    int fail_barrier; /* the barrier that fails, or 0 */
    int failed;       /* whether one did */
    int writes;       /* writes since the load, refused ones included */
    int cut_at;       /* the write that is refused, and every one after; 0 for none */
    int stopped;      /* the machine stopped: no write or barrier lands after */
    int failed_at;    /* the count of writes when a barrier failed */
};

/* The model `block_flush` reaches. */
extern struct devmodel *devmodel_current;

/** Make `m` the model over `bytes` of medium, both images the test's own,
 * and the one `block_flush` reaches. `m` is zeroed or was made before; the
 * writes still pending in it are freed. */
void devmodel_init(struct devmodel *m, unsigned char *durable, unsigned char *visible,
                   uint64_t bytes);

/** Start again: the visible image copied to the medium, nothing pending, the
 * counts at 0 and the model running; `fail_barrier` and `cut_at` stay. */
void devmodel_load(struct devmodel *m);

/** Read `count` sectors at `lba` from the visible image: 0, or -5 (EIO)
 * past the end. */
int devmodel_read(struct devmodel *m, uint64_t lba, uint32_t count, void *buf);

/** Write `count` sectors at `lba` into the visible image and the pending
 * list: 0, or -5 (EIO) past the end, at or after the cut, when the list is
 * full, or when the copy cannot be allocated. */
int devmodel_write(struct devmodel *m, uint64_t lba, uint32_t count, const void *buf);

/** The barrier: 0 when the pending writes reach the medium; -5 (EIO) at or
 * after the cut, with none landing, or at the failing barrier, with the
 * older half landing. */
int devmodel_barrier(struct devmodel *m);

/** The medium as a crash at the cut leaves it with the newest `keep`
 * pending writes on it, into `out`; `keep` past the count keeps all. */
void devmodel_snapshot(const struct devmodel *m, int keep, unsigned char *out);

#endif
