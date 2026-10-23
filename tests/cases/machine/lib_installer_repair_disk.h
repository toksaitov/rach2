/* lib_installer_repair_disk.h: one helper boot for a stopped-state edit. */
#ifndef LIB_INSTALLER_REPAIR_DISK_H
#define LIB_INSTALLER_REPAIR_DISK_H
#include <stdint.h>
#include "etude/scenario.h"
struct repair_disk_request {
    const char *mode, *disk, *name, *pubkey;
    uint64_t recovery, boot;
};
/* Bind the private disk to the image helper, edit it, sync and confirm quit.
 * OUTPUT records stdout and OUTPUT.err records stderr. OUT is their joined
 * text, owned by the caller. Return zero only after a successful edit. */
int repair_disk(struct etude_case *c, const struct repair_disk_request *request, const char *output,
                char **out);
#endif
