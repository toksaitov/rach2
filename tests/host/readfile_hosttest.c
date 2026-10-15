/**
 * Host test of the KRFFS path walk of the extractor, krffs/krffs_reader.c.
 *
 * host-deps: krffs/krffs_reader.c
 *
 * `readfile.krffs` takes a file a guest built out of a disk image with
 * `krffs_read_regular`, which must fail on a malformed or truncated image
 * with the reason in its message, and never read outside the image. The
 * test writes each region in memory by hand, byte for byte in the node
 * layout on the disk, so that each case has one defect; the stager writes
 * no such chain.
 */

#include <stdint.h>
#include <string.h>

#include "krffs_file_system.h"
#include "krffs_node.h"
#include "krffs_reader.h"

static int fails;

#define CHECK(cond)    \
    do {               \
        if (!(cond)) { \
            fails++;   \
        }              \
    } while (0)

#define REGION_BYTES 8192
#define DIR_ID       7u
#define FILE_MODE    0644u
#define DIR_MODE     (0040000u | 0755u)

static unsigned char region[REGION_BYTES];

/** Write one node header at `offset` and return the offset past its size. */
static size_t put_node(size_t offset, uint8_t type, uint64_t id, const char *name, uint16_t mode,
                       uint64_t size, uint64_t data_size, uint64_t parent_id)
{
    struct krffs_node node;

    memset(&node, 0, sizeof(node));
    node.magic = 0x4B52;
    node.type = type;
    node.id = id;
    strncpy((char *)node.name, name, sizeof(node.name) - 1);
    node.mode = mode;
    node.size = size;
    node.data_size = data_size;
    node.parent_id = parent_id;
    memcpy(region + offset, &node, sizeof(node));
    return offset + (size_t)size;
}

/** Write the well-formed region: the root, the directory "d", the file "f"
 * in it with "hello", and a free node to the end; return its file system. */
static struct krffs_file_system build_region(void)
{
    struct krffs_file_system file_system;
    size_t offset = 0;
    size_t file_at;

    memset(region, 0xEE, sizeof(region));
    offset = put_node(offset, KRFFS_Root_Node, 0, "", 0755, sizeof(struct krffs_node), 0, 0);
    offset = put_node(offset, KRFFS_Reserved_Node, DIR_ID, "d", DIR_MODE, sizeof(struct krffs_node),
                      0, 0);
    file_at = offset;
    offset = put_node(offset, KRFFS_Reserved_Node, 99, "f", FILE_MODE,
                      sizeof(struct krffs_node) + 16, 5, DIR_ID);
    memcpy(region + file_at + sizeof(struct krffs_node), "hello", 5);
    put_node(offset, KRFFS_Free_Node, 0, "", 0, REGION_BYTES - offset, 0, 0);

    file_system.node = (struct krffs_node *)region;
    file_system.size = REGION_BYTES;
    return file_system;
}

/** Run every check; the exit status is the count of failed checks, at most
 * 100, so that it never wraps to 0. */
int main(void)
{
    struct krffs_file_system file_system;
    uint8_t *data = (uint8_t *)0;
    uint64_t size = 0;
    char reason[256];

    /* The well-formed region: /d/f reads back as it was written. */
    file_system = build_region();
    CHECK(krffs_read_regular(&file_system, "/d/f", &data, &size, reason, sizeof(reason)) == 0);
    CHECK(size == 5 && data != 0 && memcmp(data, "hello", 5) == 0);

    /* A missing path fails with the missing component named. */
    file_system = build_region();
    CHECK(krffs_read_regular(&file_system, "/d/absent", &data, &size, reason, sizeof(reason)) != 0);
    CHECK(strstr(reason, "absent") != 0);

    /* A path to a directory fails. */
    file_system = build_region();
    CHECK(krffs_read_regular(&file_system, "/d", &data, &size, reason, sizeof(reason)) != 0);
    CHECK(strstr(reason, "directory") != 0);

    /* A truncated region fails at the node that runs past its end. */
    file_system = build_region();
    file_system.size = (off_t)(sizeof(struct krffs_node) + 8);
    CHECK(krffs_read_regular(&file_system, "/d/f", &data, &size, reason, sizeof(reason)) != 0);
    CHECK(strstr(reason, "region") != 0);

    /* A node whose header fits and whose size runs past the region fails. */
    file_system = build_region();
    ((struct krffs_node *)(region + sizeof(struct krffs_node)))->size = REGION_BYTES * 2;
    CHECK(krffs_read_regular(&file_system, "/d/f", &data, &size, reason, sizeof(reason)) != 0);
    CHECK(strstr(reason, "leaves the region") != 0);

    /* A size of UINT64_MAX, which added to an offset would wrap. */
    file_system = build_region();
    ((struct krffs_node *)(region + sizeof(struct krffs_node)))->size = (uint64_t)-1;
    CHECK(krffs_read_regular(&file_system, "/d/f", &data, &size, reason, sizeof(reason)) != 0);
    CHECK(strstr(reason, "leaves the region") != 0);

    /* A data size of UINT64_MAX, which added to the header would wrap. */
    file_system = build_region();
    ((struct krffs_node *)(region + 2 * sizeof(struct krffs_node)))->data_size = (uint64_t)-1;
    CHECK(krffs_read_regular(&file_system, "/d/f", &data, &size, reason, sizeof(reason)) != 0);
    CHECK(strstr(reason, "claims") != 0);

    /* A node size of 0, which would not advance the walk. */
    file_system = build_region();
    ((struct krffs_node *)(region + sizeof(struct krffs_node)))->size = 0;
    CHECK(krffs_read_regular(&file_system, "/d/f", &data, &size, reason, sizeof(reason)) != 0);
    CHECK(strstr(reason, "leaves the region") != 0);

    /* A wrong magic number. */
    file_system = build_region();
    ((struct krffs_node *)(region + sizeof(struct krffs_node)))->magic = 1;
    CHECK(krffs_read_regular(&file_system, "/d/f", &data, &size, reason, sizeof(reason)) != 0);
    CHECK(strstr(reason, "malformed") != 0);

    /* A file whose data size is larger than its node. */
    file_system = build_region();
    ((struct krffs_node *)(region + 2 * sizeof(struct krffs_node)))->data_size = 4096;
    CHECK(krffs_read_regular(&file_system, "/d/f", &data, &size, reason, sizeof(reason)) != 0);
    CHECK(strstr(reason, "claims") != 0);

    /* A relative path and the root alone fail. */
    file_system = build_region();
    CHECK(krffs_read_regular(&file_system, "d/f", &data, &size, reason, sizeof(reason)) != 0);
    CHECK(krffs_read_regular(&file_system, "/", &data, &size, reason, sizeof(reason)) != 0);

    return (fails > 100) ? 100 : fails;
}
