/*
 * krffs_hosttest.c: the file system's core over a backend of its own.
 *
 * host-deps: rach2/sys/fs/krffs.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 *
 * The core addresses nodes by offset and reaches the medium through a
 * backend, so a test can put its own backend under it. Two are used here: a
 * plain memory image, and the same image behind a recorder that keeps every
 * write in order. The rows are the format's contract: what the mount check
 * accepts and what it refuses, the allocator's split, merge and relocation,
 * the ends of the region, a hole read as zeroes, the directory operations,
 * the orphan reaping, and the order of writes.
 *
 * The order rows replay every prefix of the writes an operation makes, which
 * is what a machine stopping in the middle of one leaves on the medium. Each
 * prefix must leave a forward chain that closes with every node sound; the
 * backward link may lag, since it is derived and the check at the next mount
 * rebuilds it. That is the rule krffs.Readme.md states, and these rows are
 * what say the code keeps it.
 *
 * No hosted headers: the kernel headers define the fixed-width types, and the
 * libc calls used here are declared by hand the way the other host tests
 * declare them.
 */

#include "../../rach2/sys/fs/krffs.h"

extern int printf(const char *fmt, ...);
extern void *malloc(unsigned long size);
extern void free(void *p);

/* ---- what krffs.c reaches outside itself ---- */

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

void console_printf(const char *format, ...)
{
    (void)format;
}

static int64_t fake_seconds = 1000;

void rtc_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    if (seconds) {
        *seconds = fake_seconds;
    }
    if (nanoseconds) {
        *nanoseconds = 0;
    }
}

static int fails;

static void check(int ok, const char *what)
{
    if (!ok) {
        fails++;
        printf("krffs_hosttest: FAIL: %s\n", what);
    }
}

/* ---- the image and its backends ---- */

#define IMAGE_SIZE 65536u

/* What free space is filled with in the order rows: no file writes it, so a
   byte of it inside a file's length is a length published too early. */
#define POISON 0xA5

static uint8_t image[IMAGE_SIZE];

static int memory_read(void *ctx, uint64_t offset, void *buffer, uint32_t size)
{
    k_memcpy(buffer, (uint8_t *)ctx + offset, size);
    return 0;
}

static int memory_write(void *ctx, uint64_t offset, const void *buffer, uint32_t size)
{
    k_memcpy((uint8_t *)ctx + offset, buffer, size);
    return 0;
}

static int memory_sync(void *ctx)
{
    (void)ctx;
    return 0;
}

static const krffs_backend_t memory_backend = {memory_read, memory_write, memory_sync};

/* The recorder: the same image, with every write kept in the order it was
   made, so a row can replay any prefix of an operation. */

#define WRITES_MAX 4096

static struct {
    uint64_t offset;
    uint32_t size;
    uint8_t bytes[512];
} recorded[WRITES_MAX];

static int recorded_count;
static int recording;
/* Which write the backend refuses, counted from the start of an operation, or
   -1 for a backend that refuses none. One write and not every write after it:
   a device that refuses a block and takes the next is what leaves a change
   published over a write that never landed. */
static int refused_write = -1;
static int writes_seen;

static int record_write(void *ctx, uint64_t offset, const void *buffer, uint32_t size)
{
    if (writes_seen++ == refused_write) {
        return -1;
    }
    if (recording && size <= sizeof(recorded[0].bytes) && recorded_count < WRITES_MAX) {
        recorded[recorded_count].offset = offset;
        recorded[recorded_count].size = size;
        k_memcpy(recorded[recorded_count].bytes, buffer, size);
        recorded_count++;
    } else if (recording) {
        printf("krffs_hosttest: FAIL: a write the recorder cannot hold\n");
        fails++;
    }
    return memory_write(ctx, offset, buffer, size);
}

static const krffs_backend_t recording_backend = {memory_read, record_write, memory_sync};

/* ---- images ---- */

/**
 * Lay a fresh file system into `buffer`: a root node and one free node.
 */
static void make_image(uint8_t *buffer, uint32_t size)
{
    krffs_node_t *root = (krffs_node_t *)buffer;
    krffs_node_t *free_node = (krffs_node_t *)(buffer + sizeof(krffs_node_t));
    uint32_t i;

    for (i = 0; i < size; i++) {
        buffer[i] = 0;
    }
    root->magic = KRFFS_FILE_SYSTEM_MAGIC;
    root->type = KRFFS_ROOT_NODE;
    root->id = KRFFS_ROOT_ID;
    root->mode = (uint16_t)(KRFFS_S_IFDIR | 0755);
    root->size = sizeof(krffs_node_t);
    root->previous_node_size = size - sizeof(krffs_node_t);
    root->data_size = 0;

    free_node->magic = KRFFS_FILE_SYSTEM_MAGIC;
    free_node->type = KRFFS_FREE_NODE;
    free_node->size = size - sizeof(krffs_node_t);
    free_node->previous_node_size = sizeof(krffs_node_t);
    free_node->data_size = 0;
}

static krffs_mount_t mount_over(const krffs_backend_t *backend, void *ctx, uint32_t size)
{
    krffs_mount_t mount;

    mount.backend = backend;
    mount.ctx = ctx;
    mount.size = size;
    return mount;
}

/**
 * Walk the forward chain, asking of every node what the check asks except the
 * backward link, which an interrupted operation is allowed to leave behind.
 *
 * Returns 1 if the chain closes with every node sound, 0 if not.
 */
static int forward_chain_closes(krffs_mount_t *mount)
{
    uint64_t position = 0;
    uint64_t total = 0;
    uint64_t steps = 0;
    krffs_node_t head;

    for (;;) {
        if (++steps > krffs_max_steps(mount)) {
            return 0;
        }
        if (mount->size - position < sizeof(krffs_node_t)) {
            return 0;
        }
        if (krffs_head(mount, position, &head) != 0) {
            return 0;
        }
        if (head.magic != KRFFS_FILE_SYSTEM_MAGIC) {
            return 0;
        }
        if (head.type != KRFFS_ROOT_NODE && head.type != KRFFS_FREE_NODE &&
            head.type != KRFFS_RESERVED_NODE) {
            return 0;
        }
        if (head.size < sizeof(krffs_node_t) || head.size > mount->size - position) {
            return 0;
        }
        if (head.data_size > head.size - sizeof(krffs_node_t)) {
            return 0;
        }
        total += head.size;
        position += head.size;
        if (position == mount->size) {
            break;
        }
    }
    return total == mount->size;
}

/**
 * Rebuild every backward link from the forward chain, which is what the check
 * at the next mount does with the one fault an interrupted write can leave.
 */
static void rebuild_backward_links(krffs_mount_t *mount)
{
    uint64_t position = 0;
    uint64_t previous_size = 0;
    krffs_node_t head;
    uint64_t steps = 0;

    while (steps++ <= krffs_max_steps(mount)) {
        if (krffs_head(mount, position, &head) != 0) {
            return;
        }
        if (position != 0) {
            head.previous_node_size = previous_size;
            mount->backend->write(mount->ctx, position + KRFFS_FIELD_PREVIOUS, &previous_size,
                                  sizeof(previous_size));
        }
        previous_size = head.size;
        position += head.size;
        if (position >= mount->size) {
            break;
        }
    }
    mount->backend->write(mount->ctx, KRFFS_FIELD_PREVIOUS, &previous_size, sizeof(previous_size));
}

/**
 * Replay the first `count` recorded writes onto a fresh copy of `before` and
 * judge what a machine stopping there would have left.
 */
static int prefix_is_sound(const uint8_t *before, int count, uint32_t size)
{
    static uint8_t replay[IMAGE_SIZE];
    krffs_mount_t mount = mount_over(&memory_backend, replay, size);
    uint64_t offset = 0;
    uint64_t steps = 0;
    krffs_node_t head;
    int i;

    k_memcpy(replay, before, size);
    for (i = 0; i < count; i++) {
        k_memcpy(replay + recorded[i].offset, recorded[i].bytes, recorded[i].size);
    }
    if (!forward_chain_closes(&mount)) {
        return 0;
    }
    /* The free space is poisoned, so a length published before the bytes it
       counts shows up here as a file claiming bytes nobody wrote. */
    do {
        if (krffs_head(&mount, offset, &head) != 0) {
            return 0;
        }
        if (head.type == KRFFS_RESERVED_NODE) {
            uint64_t at;

            for (at = 0; at < head.data_size; at++) {
                if (replay[offset + sizeof(krffs_node_t) + at] == POISON) {
                    return 0;
                }
            }
        }
        offset = krffs_next_offset(&mount, offset, &head);
    } while (offset != 0 && ++steps < krffs_max_steps(&mount));

    rebuild_backward_links(&mount);
    return krffs_chain_is_valid(&mount);
}

/**
 * Every prefix of the writes just recorded, asked whether a file ever claims a
 * byte past `old_length` that is not a zero.
 *
 * That is what a length published before the zeroes it counts looks like: the
 * file grew into slack it already had, so the bytes under the new length are
 * whatever it held before rather than the zeroes a growth owes.
 */
static int every_prefix_is_zero_filled(const uint8_t *before, uint32_t size, uint64_t old_length)
{
    static uint8_t replay[IMAGE_SIZE];
    krffs_mount_t mount = mount_over(&memory_backend, replay, size);
    int i;

    for (i = 0; i <= recorded_count; i++) {
        uint64_t offset = 0;
        uint64_t steps = 0;
        krffs_node_t head;
        int k;

        k_memcpy(replay, before, size);
        for (k = 0; k < i; k++) {
            k_memcpy(replay + recorded[k].offset, recorded[k].bytes, recorded[k].size);
        }
        do {
            if (krffs_head(&mount, offset, &head) != 0) {
                return 0;
            }
            if (head.type == KRFFS_RESERVED_NODE) {
                uint64_t at;

                for (at = old_length; at < head.data_size; at++) {
                    if (replay[offset + sizeof(krffs_node_t) + at] != 0) {
                        printf("krffs_hosttest: after %d of %d writes a file claims "
                               "a byte it did not zero\n",
                               i, recorded_count);
                        return 0;
                    }
                }
            }
            offset = krffs_next_offset(&mount, offset, &head);
        } while (offset != 0 && ++steps < krffs_max_steps(&mount));
    }
    return 1;
}

/**
 * Every prefix of the writes just recorded, judged. Returns 1 if all are
 * sound.
 */
static int every_prefix_is_sound(const uint8_t *before, uint32_t size)
{
    int i;

    for (i = 0; i <= recorded_count; i++) {
        if (!prefix_is_sound(before, i, size)) {
            printf("krffs_hosttest: the image after %d of %d writes is not "
                   "one a mount could repair\n",
                   i, recorded_count);
            return 0;
        }
    }
    return 1;
}

/**
 * Run `what` with the backend refusing write number `which`, and judge what
 * the file system was left with.
 *
 * A backend on a device can refuse a write, and the core must not publish a
 * change over a write that was refused. Returns 1 if the image is still one a
 * mount can walk, whatever the operation answered.
 */
static int survives_refusal(uint8_t *img, const uint8_t *before, uint32_t size,
                            void (*what)(krffs_mount_t *), int which)
{
    krffs_mount_t mount = mount_over(&recording_backend, img, size);
    uint64_t offset = 0;
    uint64_t steps = 0;
    krffs_node_t head;

    k_memcpy(img, before, size);
    recording = 0;
    writes_seen = 0;
    refused_write = which;
    what(&mount);
    refused_write = -1;

    if (!forward_chain_closes(&mount)) {
        return 0;
    }
    do {
        if (krffs_head(&mount, offset, &head) != 0) {
            return 0;
        }
        if (head.type == KRFFS_RESERVED_NODE) {
            uint64_t at;

            for (at = 0; at < head.data_size; at++) {
                if (img[offset + sizeof(krffs_node_t) + at] == POISON) {
                    return 0;
                }
            }
        }
        offset = krffs_next_offset(&mount, offset, &head);
    } while (offset != 0 && ++steps < krffs_max_steps(&mount));
    return 1;
}

static void make_a_file(krffs_mount_t *mount)
{
    /* A name the image does not have, so the allocator really runs: creating
       one that exists answers success without taking a node. */
    krffs_create_at(mount, "/second");
}

static void grow_a_file(krffs_mount_t *mount)
{
    krffs_truncate_at(mount, "/refused", 3000);
}

static void remove_a_file(krffs_mount_t *mount)
{
    krffs_unlink_at(mount, "/refused");
}

int main(void)
{
    static uint8_t before[IMAGE_SIZE];
    krffs_mount_t mount;
    krffs_node_t head;
    uint64_t node;
    uint64_t other;
    char buffer[1024];
    char name[64];
    int is_dir;
    int i;

    /* ---- the mount check ---- */

    make_image(image, IMAGE_SIZE);
    mount = mount_over(&memory_backend, image, IMAGE_SIZE);
    check(krffs_chain_is_valid(&mount) == 1, "a fresh image is sound");

    image[0] = 0;
    check(krffs_chain_is_valid(&mount) == 0, "a bad magic is refused");
    make_image(image, IMAGE_SIZE);

    image[sizeof(krffs_node_t) + KRFFS_FIELD_TYPE] = 42;
    check(krffs_chain_is_valid(&mount) == 0, "an unknown node type is refused");
    make_image(image, IMAGE_SIZE);

    {
        uint64_t huge = IMAGE_SIZE;

        k_memcpy(image + sizeof(krffs_node_t) + KRFFS_FIELD_SIZE, &huge, sizeof(huge));
        check(krffs_chain_is_valid(&mount) == 0, "a size that leaves the region is refused");
        make_image(image, IMAGE_SIZE);
    }
    {
        uint64_t small = sizeof(krffs_node_t) - 1;

        k_memcpy(image + sizeof(krffs_node_t) + KRFFS_FIELD_SIZE, &small, sizeof(small));
        check(krffs_chain_is_valid(&mount) == 0, "a size below a header is refused");
        make_image(image, IMAGE_SIZE);
    }
    {
        uint64_t data = IMAGE_SIZE;

        k_memcpy(image + sizeof(krffs_node_t) + KRFFS_FIELD_DATA_SIZE, &data, sizeof(data));
        check(krffs_chain_is_valid(&mount) == 0, "data larger than its node is refused");
        make_image(image, IMAGE_SIZE);
    }
    {
        uint64_t wrong = 12345;

        k_memcpy(image + sizeof(krffs_node_t) + KRFFS_FIELD_PREVIOUS, &wrong, sizeof(wrong));
        check(krffs_chain_is_valid(&mount) == 0,
              "a backward link the forward chain contradicts is refused");
        make_image(image, IMAGE_SIZE);
    }
    {
        uint64_t wrong = 12345;

        k_memcpy(image + KRFFS_FIELD_PREVIOUS, &wrong, sizeof(wrong));
        check(krffs_chain_is_valid(&mount) == 0,
              "a root whose backward link is not the last node is refused");
        make_image(image, IMAGE_SIZE);
    }
    {
        /* A name that fills its field has no end, and everything that reads
           a name would read past it. */
        uint64_t offset = sizeof(krffs_node_t);

        image[offset + KRFFS_FIELD_TYPE] = KRFFS_RESERVED_NODE;
        for (i = 0; i < KRFFS_FILE_NAME_BUFFER_SIZE; i++) {
            image[offset + KRFFS_FIELD_NAME + i] = 'x';
        }
        check(krffs_chain_is_valid(&mount) == 0, "a name that never ends is refused");
        make_image(image, IMAGE_SIZE);
    }

    /* A size no region could hold is refused before the arithmetic that adds
       a header to it can wrap. */
    check(krffs_allocate_at(&mount, (uint64_t)-1, KRFFS_NO_NODE) == KRFFS_NO_NODE,
          "a size larger than the region allocates nothing");
    check(krffs_allocate_at(&mount, IMAGE_SIZE, KRFFS_NO_NODE) == KRFFS_NO_NODE,
          "and neither does one exactly the region's size");
    check(krffs_chain_is_valid(&mount) == 1, "and the chain is what it was before both");

    /* ---- the operations ---- */

    check(krffs_create_at(&mount, "/alpha") == 0, "a file is created");
    node = krffs_walk(&mount, "/alpha");
    check(krffs_resize_at(&mount, node, (uint64_t)-1) == KRFFS_NO_NODE,
          "a resize to more than the region answers nothing");
    check(krffs_chain_is_valid(&mount) == 1, "and changes nothing");
    check(krffs_chain_is_valid(&mount) == 1, "the chain is sound after it");
    node = krffs_walk(&mount, "/alpha");
    check(node != KRFFS_NO_NODE && node != 0, "and the walk finds it");
    check(krffs_head(&mount, node, &head) == 0 && head.data_size == 0, "a new file is empty");

    for (i = 0; i < (int)sizeof(buffer); i++) {
        buffer[i] = (char)((i % 127) + 1);
    }
    check(krffs_write_at(&mount, &node, buffer, 16, 0) == 16, "sixteen bytes are written");
    check(krffs_head(&mount, node, &head) == 0 && head.data_size == 16,
          "and the file is sixteen bytes long");
    k_memset(buffer, 0, sizeof(buffer));
    check(krffs_read_at(&mount, node, buffer, 16, 0) == 16 && buffer[0] == 1 && buffer[15] == 16,
          "and they read back");
    check(krffs_read_at(&mount, node, buffer, 16, 16) == 0, "a read past the end answers nothing");
    check(krffs_chain_is_valid(&mount) == 1, "the chain is sound after a write");

    /* A write past the end leaves a hole, and a hole reads as zeroes. */
    buffer[0] = 'z';
    check(krffs_write_at(&mount, &node, buffer, 1, 100) == 1,
          "a write past the end grows the file");
    k_memset(buffer, 0x5A, sizeof(buffer));
    check(krffs_read_at(&mount, node, buffer, 32, 20) == 32, "the hole reads");
    for (i = 0; i < 32; i++) {
        if (buffer[i] != 0) {
            break;
        }
    }
    check(i == 32, "and every byte of it is zero");

    check(krffs_truncate_at(&mount, "/alpha", 4) == 0, "the file is truncated");
    node = krffs_walk(&mount, "/alpha");
    check(krffs_head(&mount, node, &head) == 0 && head.data_size == 4, "and is four bytes long");
    check(krffs_chain_is_valid(&mount) == 1, "the chain is sound after a truncate");

    check(krffs_truncate_at(&mount, "/alpha", 4096) == 0, "the file grows");
    node = krffs_walk(&mount, "/alpha");
    check(krffs_head(&mount, node, &head) == 0 && head.data_size == 4096, "to what was asked");
    check(krffs_read_at(&mount, node, buffer, 8, 64) == 8 && buffer[0] == 0,
          "and the growth is zeroes");
    check(krffs_chain_is_valid(&mount) == 1, "the chain is sound after a grow");

    /* A grow with a node in the way relocates, and the contents move. */
    check(krffs_create_at(&mount, "/beta") == 0, "a second file is created");
    other = krffs_walk(&mount, "/beta");
    check(krffs_write_at(&mount, &other, buffer, 8, 0) == 8, "and written");
    node = krffs_walk(&mount, "/alpha");
    check(krffs_write_at(&mount, &node, buffer, 8, 8192) == 8,
          "the first file grows past its neighbour");
    check(krffs_walk(&mount, "/alpha") != KRFFS_NO_NODE, "and the name still finds it");
    check(krffs_walk(&mount, "/beta") != KRFFS_NO_NODE, "and the neighbour is where it was");
    check(krffs_chain_is_valid(&mount) == 1, "the chain is sound after a relocation");
    {
        uint64_t alpha = krffs_walk(&mount, "/alpha");
        uint64_t beta = krffs_walk(&mount, "/beta");

        check(alpha != beta, "the two files are two nodes");
        check(krffs_head(&mount, alpha, &head) == 0 && head.data_size == 8200,
              "the moved file has the length the write made");
    }

    /* Directories. */
    check(krffs_mkdir_at(&mount, "/dir") == 0, "a directory is made");
    check(krffs_mkdir_at(&mount, "/dir") == -1, "and not twice");
    check(krffs_create_at(&mount, "/dir/inner") == 0, "a file inside it");
    check(krffs_walk(&mount, "/dir/inner") != KRFFS_NO_NODE, "resolves");
    check(krffs_rmdir_at(&mount, "/dir") == -1, "a directory with a child is not removed");
    check(krffs_readdir_at(&mount, "/dir", 0, name, sizeof(name), &is_dir) == 1 && name[0] == 'i' &&
              is_dir == 0,
          "its one child is listed");
    check(krffs_readdir_at(&mount, "/dir", 1, name, sizeof(name), &is_dir) == 0,
          "and there is no second");
    check(krffs_node_path_at(&mount, krffs_walk(&mount, "/dir/inner"), name, sizeof(name)) == 0 &&
              name[0] == '/' && name[1] == 'd',
          "the child's path is rebuilt from its parents");
    check(krffs_unlink_at(&mount, "/dir/inner") == 0, "the child is removed");
    check(krffs_rmdir_at(&mount, "/dir") == 0, "and then the directory is");
    check(krffs_chain_is_valid(&mount) == 1, "the chain is sound after the directory rows");

    /* A rename moves the name, and an orphan is a node no path reaches. */
    check(krffs_rename_at(&mount, "/beta", "/gamma") == 0, "a file is renamed");
    check(krffs_walk(&mount, "/beta") == KRFFS_NO_NODE, "the old name is gone");
    check(krffs_walk(&mount, "/gamma") != KRFFS_NO_NODE, "the new one is not");
    krffs_orphan_at(&mount, krffs_walk(&mount, "/gamma"));
    check(krffs_walk(&mount, "/gamma") == KRFFS_NO_NODE, "an orphan is reached by no path");
    check(krffs_reap_orphans_at(&mount) == 1, "and the reaper frees it");
    check(krffs_reap_orphans_at(&mount) == 0, "and finds nothing the next time");
    check(krffs_chain_is_valid(&mount) == 1, "the chain is sound after reaping");

    /* ---- the ends of the region ---- */

    make_image(image, IMAGE_SIZE);
    {
        uint32_t before_free = krffs_free_at(&mount);
        uint64_t taken;

        check(before_free == IMAGE_SIZE - 2 * sizeof(krffs_node_t),
              "a fresh image is free but for its two headers");
        taken = krffs_allocate_at(&mount, before_free, KRFFS_NO_NODE);
        check(taken != KRFFS_NO_NODE, "the whole free node can be taken");
        check(krffs_free_at(&mount) == 0, "and nothing is free after it");
        check(krffs_chain_is_valid(&mount) == 1, "the region's last node ends at its edge");
        check(krffs_allocate_at(&mount, 1, KRFFS_NO_NODE) == KRFFS_NO_NODE,
              "a full region allocates nothing");
        krffs_remove_at(&mount, taken);
        check(krffs_free_at(&mount) == before_free, "and the space comes back");
        check(krffs_chain_is_valid(&mount) == 1, "sound again");
    }
    {
        /* A remainder too small for a header of its own stays in the node. */
        uint32_t free_bytes = krffs_free_at(&mount);
        uint64_t taken = krffs_allocate_at(&mount, free_bytes - 8, KRFFS_NO_NODE);

        check(taken != KRFFS_NO_NODE, "a node that leaves eight bytes is taken");
        check(krffs_head(&mount, taken, &head) == 0 &&
                  head.size == IMAGE_SIZE - sizeof(krffs_node_t),
              "the remainder stays inside it rather than becoming a fragment");
        check(krffs_chain_is_valid(&mount) == 1, "and the chain is sound");
        krffs_remove_at(&mount, taken);
    }

    /* ---- the order of writes ---- */

    make_image(image, IMAGE_SIZE);
    for (i = 2 * (int)sizeof(krffs_node_t); i < (int)IMAGE_SIZE; i++) {
        image[i] = POISON;
    }
    mount = mount_over(&recording_backend, image, IMAGE_SIZE);

    k_memcpy(before, image, IMAGE_SIZE);
    recorded_count = 0;
    recording = 1;
    krffs_create_at(&mount, "/one");
    recording = 0;
    check(every_prefix_is_sound(before, IMAGE_SIZE),
          "every stop inside a create leaves a chain a mount can walk");

    k_memcpy(before, image, IMAGE_SIZE);
    recorded_count = 0;
    recording = 1;
    node = krffs_walk(&mount, "/one");
    krffs_write_at(&mount, &node, buffer, 512, 0);
    recording = 0;
    check(every_prefix_is_sound(before, IMAGE_SIZE),
          "every stop inside a write leaves a chain a mount can walk");

    k_memcpy(before, image, IMAGE_SIZE);
    recorded_count = 0;
    recording = 1;
    krffs_truncate_at(&mount, "/one", 8192);
    recording = 0;
    check(every_prefix_is_sound(before, IMAGE_SIZE),
          "every stop inside a grow leaves a chain a mount can walk");

    k_memcpy(before, image, IMAGE_SIZE);
    recorded_count = 0;
    recording = 1;
    krffs_truncate_at(&mount, "/one", 16);
    recording = 0;
    check(every_prefix_is_sound(before, IMAGE_SIZE),
          "every stop inside a truncate leaves a chain a mount can walk");

    /* A move: a second file boxes the first in, so the grow relocates it. */
    krffs_create_at(&mount, "/two");
    node = krffs_walk(&mount, "/two");
    krffs_write_at(&mount, &node, buffer, 16, 0);
    k_memcpy(before, image, IMAGE_SIZE);
    recorded_count = 0;
    recording = 1;
    node = krffs_walk(&mount, "/one");
    krffs_write_at(&mount, &node, buffer, 16, 4096);
    recording = 0;
    check(every_prefix_is_sound(before, IMAGE_SIZE),
          "every stop inside a move leaves a chain a mount can walk");

    k_memcpy(before, image, IMAGE_SIZE);
    recorded_count = 0;
    recording = 1;
    krffs_unlink_at(&mount, "/two");
    recording = 0;
    check(every_prefix_is_sound(before, IMAGE_SIZE),
          "every stop inside a removal leaves a chain a mount can walk");

    /* ---- a length raised inside the slack a node already has ---- */

    make_image(image, IMAGE_SIZE);
    for (i = 2 * (int)sizeof(krffs_node_t); i < (int)IMAGE_SIZE; i++) {
        image[i] = POISON;
    }
    mount = mount_over(&recording_backend, image, IMAGE_SIZE);
    krffs_create_at(&mount, "/slack");
    node = krffs_walk(&mount, "/slack");
    krffs_write_at(&mount, &node, buffer, 108, 0);
    krffs_truncate_at(&mount, "/slack", 100);
    node = krffs_walk(&mount, "/slack");
    check(krffs_head(&mount, node, &head) == 0 && head.data_size == 100 &&
              head.size == sizeof(krffs_node_t) + 108,
          "a node keeps the slack a truncate of a few bytes leaves");

    k_memcpy(before, image, IMAGE_SIZE);
    recorded_count = 0;
    recording = 1;
    krffs_truncate_at(&mount, "/slack", 104);
    recording = 0;
    check(every_prefix_is_sound(before, IMAGE_SIZE),
          "every stop inside a growth into that slack leaves a chain a mount "
          "can walk");
    check(every_prefix_is_zero_filled(before, IMAGE_SIZE, 100),
          "and none of them shows a length over bytes the file used to hold");
    node = krffs_walk(&mount, "/slack");
    check(krffs_read_at(&mount, node, buffer, 4, 100) == 4 && buffer[0] == 0 && buffer[3] == 0,
          "and the bytes it grew by are zeroes, not what the file held before");

    /* ---- a backend that refuses a write ---- */

    make_image(image, IMAGE_SIZE);
    for (i = 2 * (int)sizeof(krffs_node_t); i < (int)IMAGE_SIZE; i++) {
        image[i] = POISON;
    }
    mount = mount_over(&recording_backend, image, IMAGE_SIZE);
    krffs_create_at(&mount, "/refused");
    node = krffs_walk(&mount, "/refused");
    krffs_write_at(&mount, &node, buffer, 64, 0);
    k_memcpy(before, image, IMAGE_SIZE);

    {
        int which;
        int sound = 1;

        for (which = 0; which < 24; which++) {
            if (!survives_refusal(image, before, IMAGE_SIZE, grow_a_file, which) ||
                !survives_refusal(image, before, IMAGE_SIZE, remove_a_file, which) ||
                !survives_refusal(image, before, IMAGE_SIZE, make_a_file, which)) {
                printf("krffs_hosttest: a backend that refused write %d left "
                       "an image no mount could walk\n",
                       which);
                sound = 0;
                break;
            }
        }
        check(sound, "a refused write never leaves a chain a mount cannot walk");
        k_memcpy(image, before, IMAGE_SIZE);
    }

    if (fails == 0) {
        printf("krffs_hosttest: OK\n");
    }
    return fails;
}
