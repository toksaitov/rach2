#include "krffs_reader.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "krffs_file_system.h"
#include "krffs_node.h"

/** A directory's type bit in a node's mode (matches the kernel's S_IFDIR). */
#define READER_S_IFDIR 0040000u

/**
 * Validate that `node` lies whole inside the region and describes itself
 * sanely: the magic, a size that covers at least its own header, and an end
 * that does not run past the region. Writes the reason and returns false
 * when it does not, which is how a truncated or corrupted image surfaces as
 * a refusal instead of a read out of bounds.
 */
static bool node_is_sane(struct krffs_file_system *file_system, struct krffs_node *node,
                         char *errbuf, size_t errlen)
{
    uint8_t *base = (uint8_t *)file_system->node;
    uint8_t *position = (uint8_t *)node;
    uint64_t offset = (uint64_t)(position - base);
    uint64_t region = (uint64_t)file_system->size;

    /* Subtraction only, never addition: a crafted 64-bit size added to an
     * offset wraps, and a wrapped sum compares as inside the region. */
    if (offset >= region || region - offset < sizeof(*node)) {
        snprintf(errbuf, errlen, "node at offset 0x%llx runs past the region",
                 (unsigned long long)offset);
        return false;
    }
    if (node->magic != KRFFS_File_System_Magic) {
        snprintf(errbuf, errlen,
                 "node at offset 0x%llx has magic 0x%x, not 0x%x: the chain "
                 "is malformed",
                 (unsigned long long)offset, node->magic, KRFFS_File_System_Magic);
        return false;
    }
    if (node->size < sizeof(*node) || node->size > region - offset) {
        snprintf(errbuf, errlen,
                 "node at offset 0x%llx has size 0x%llx, which leaves the "
                 "region",
                 (unsigned long long)offset, (unsigned long long)node->size);
        return false;
    }
    return true;
}

/**
 * Find the reserved child named `name` inside directory `parent_id`, walking
 * every node once with the bounds checks above. Returns NULL with the reason
 * set when the chain is malformed, and NULL with `errbuf` empty when the
 * chain is whole and the child simply is not there.
 */
static struct krffs_node *find_child(struct krffs_file_system *file_system, uint64_t parent_id,
                                     const char *name, char *errbuf, size_t errlen)
{
    struct krffs_node *node = file_system->node;
    uint8_t *base = (uint8_t *)file_system->node;
    uint64_t region = (uint64_t)file_system->size;
    uint64_t walked = 0;

    errbuf[0] = '\0';
    while (true) {
        if (!node_is_sane(file_system, node, errbuf, errlen)) {
            return NULL;
        }
        if (node->type == KRFFS_Reserved_Node && node->parent_id == parent_id &&
            strncmp((const char *)node->name, name, KRFFS_FILE_NAME_BUFFER_SIZE) == 0) {
            return node;
        }
        walked += node->size;
        if (walked >= region) {
            return NULL;
        }
        node = (struct krffs_node *)(((uint8_t *)node) + node->size);
        if ((uint8_t *)node >= base + region) {
            return NULL;
        }
    }
}

int krffs_read_regular(struct krffs_file_system *file_system, const char *path, uint8_t **data,
                       uint64_t *size, char *errbuf, size_t errlen)
{
    char component[KRFFS_FILE_NAME_BUFFER_SIZE];
    struct krffs_node *node = NULL;
    uint64_t parent_id = 0;
    const char *cursor = path;

    if (path == NULL || path[0] != '/' || path[1] == '\0') {
        snprintf(errbuf, errlen, "the path must be absolute and name a file");
        return -1;
    }
    if (!node_is_sane(file_system, file_system->node, errbuf, errlen)) {
        return -1;
    }

    while (*cursor == '/') {
        size_t length = 0;

        cursor++;
        while (cursor[length] != '\0' && cursor[length] != '/') {
            length++;
        }
        if (length == 0) {
            continue;
        }
        if (length >= sizeof(component)) {
            snprintf(errbuf, errlen,
                     "a path component is longer than a "
                     "node name can be");
            return -1;
        }
        memcpy(component, cursor, length);
        component[length] = '\0';
        cursor += length;

        node = find_child(file_system, parent_id, component, errbuf, errlen);
        if (node == NULL) {
            if (errbuf[0] == '\0') {
                snprintf(errbuf, errlen,
                         "no '%s' under the directory at id "
                         "0x%llx",
                         component, (unsigned long long)parent_id);
            }
            return -1;
        }
        parent_id = node->id;
    }

    if (node == NULL) {
        snprintf(errbuf, errlen, "the path must be absolute and name a file");
        return -1;
    }
    if (node->mode & READER_S_IFDIR) {
        snprintf(errbuf, errlen, "'%s' is a directory", path);
        return -1;
    }
    if (node->data_size > node->size - sizeof(*node)) {
        snprintf(errbuf, errlen, "'%s' claims 0x%llx data bytes in a 0x%llx-byte node", path,
                 (unsigned long long)node->data_size, (unsigned long long)node->size);
        return -1;
    }

    *data = ((uint8_t *)node) + sizeof(*node);
    *size = node->data_size;
    return 0;
}
