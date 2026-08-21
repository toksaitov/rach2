/**
 * Dual-target host test for the KRFFS path walk the extractor runs.
 *
 * host-deps: krffs/krffs_reader.c
 *
 * `krffs_read_regular` is what `readfile.krffs` uses to pull a guest-built
 * artifact out of a disk image, so it must refuse a malformed or truncated
 * image with the reason named rather than read out of bounds. The regions
 * here are crafted by hand in memory, byte for byte in the on-disk node
 * layout, which is what lets each case hold exactly one defect: a chain the
 * real stager would never write cannot be produced by calling the real
 * stager. Returns the failed-check count (0 == all passed).
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

/** Build the well-formed region: root, a directory, a file in it, free. */
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

int main(void)
{
    struct krffs_file_system file_system;
    uint8_t *data = (uint8_t *)0;
    uint64_t size = 0;
    char reason[256];

    /* The well-formed region: the file reads back byte-exact through its
     * two-component path. */
    file_system = build_region();
    CHECK(krffs_read_regular(&file_system, "/d/f", &data, &size, reason, sizeof(reason)) == 0);
    CHECK(size == 5 && data != 0 && memcmp(data, "hello", 5) == 0);

    /* A missing path is a refusal that names the missing component. */
    file_system = build_region();
    CHECK(krffs_read_regular(&file_system, "/d/absent", &data, &size, reason, sizeof(reason)) != 0);
    CHECK(strstr(reason, "absent") != 0);

    /* A directory is refused; this reads files. */
    file_system = build_region();
    CHECK(krffs_read_regular(&file_system, "/d", &data, &size, reason, sizeof(reason)) != 0);
    CHECK(strstr(reason, "directory") != 0);

    /* A truncated region: the walk must refuse the node that runs past the
     * end, not read through it. */
    file_system = build_region();
    file_system.size = (off_t)(sizeof(struct krffs_node) + 8);
    CHECK(krffs_read_regular(&file_system, "/d/f", &data, &size, reason, sizeof(reason)) != 0);
    CHECK(strstr(reason, "region") != 0);

    /* A node whose size field escapes the region while its header fits:
     * the walk must name that node, not report the path missing after
     * skipping over the region's end. */
    file_system = build_region();
    ((struct krffs_node *)(region + sizeof(struct krffs_node)))->size = REGION_BYTES * 2;
    CHECK(krffs_read_regular(&file_system, "/d/f", &data, &size, reason, sizeof(reason)) != 0);
    CHECK(strstr(reason, "leaves the region") != 0);

    /* A size of UINT64_MAX: added to any offset it would wrap and compare
     * as inside the region, so the check must subtract instead of add. */
    file_system = build_region();
    ((struct krffs_node *)(region + sizeof(struct krffs_node)))->size = (uint64_t)-1;
    CHECK(krffs_read_regular(&file_system, "/d/f", &data, &size, reason, sizeof(reason)) != 0);
    CHECK(strstr(reason, "leaves the region") != 0);

    /* A data size of UINT64_MAX: added to the header size it would wrap the
     * same way. */
    file_system = build_region();
    ((struct krffs_node *)(region + 2 * sizeof(struct krffs_node)))->data_size = (uint64_t)-1;
    CHECK(krffs_read_regular(&file_system, "/d/f", &data, &size, reason, sizeof(reason)) != 0);
    CHECK(strstr(reason, "claims") != 0);

    /* A malformed chain: a zero node size cannot advance the walk. */
    file_system = build_region();
    ((struct krffs_node *)(region + sizeof(struct krffs_node)))->size = 0;
    CHECK(krffs_read_regular(&file_system, "/d/f", &data, &size, reason, sizeof(reason)) != 0);
    CHECK(strstr(reason, "leaves the region") != 0);

    /* A malformed chain: a wrong magic is a refusal, not a guess. */
    file_system = build_region();
    ((struct krffs_node *)(region + sizeof(struct krffs_node)))->magic = 1;
    CHECK(krffs_read_regular(&file_system, "/d/f", &data, &size, reason, sizeof(reason)) != 0);
    CHECK(strstr(reason, "malformed") != 0);

    /* A file whose claimed data does not fit its node: refused before the
     * caller can copy bytes that belong to the next node. */
    file_system = build_region();
    ((struct krffs_node *)(region + 2 * sizeof(struct krffs_node)))->data_size = 4096;
    CHECK(krffs_read_regular(&file_system, "/d/f", &data, &size, reason, sizeof(reason)) != 0);
    CHECK(strstr(reason, "claims") != 0);

    /* A relative or empty path never walks at all. */
    file_system = build_region();
    CHECK(krffs_read_regular(&file_system, "d/f", &data, &size, reason, sizeof(reason)) != 0);
    CHECK(krffs_read_regular(&file_system, "/", &data, &size, reason, sizeof(reason)) != 0);

    /* Bounded so the count never wraps to 0 (a false pass) as an exit
     * status. */
    return (fails > 100) ? 100 : fails;
}
