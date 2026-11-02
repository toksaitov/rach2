/* Validate the full script inventory before selecting a partition. */
#ifndef HOST_SCRIPT_INVENTORY_H
#define HOST_SCRIPT_INVENTORY_H
#include <stddef.h>
#define HOST_SCRIPT_ROWS  46
#define HOST_SCRIPT_PARTS 5
enum host_script_kind { SCRIPT_KERNEL, SCRIPT_PROTOCOL, SCRIPT_IMAGE, SCRIPT_BUILD, SCRIPT_FLOW };
enum host_script_part {
    SCRIPT_NATIVE_PART,
    SCRIPT_IMAGE_PART,
    SCRIPT_BUILD_PART,
    SCRIPT_FLOW_PART,
    SCRIPT_TOOLS_PART
};
struct host_script_row {
    char id[80];
    enum host_script_kind kind;
    enum host_script_part part;
};
const char *host_script_case(unsigned part);
int host_script_case_part(const char *id);
int host_script_inventory(const char *list, const char *manifest,
                          struct host_script_row rows[HOST_SCRIPT_ROWS], size_t *count);
#endif
