#ifndef KRFFS_NODE_H
#define KRFFS_NODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

struct krffs_file_system;

#define KRFFS_FILE_NAME_BUFFER_SIZE 256

enum krffs_node_type {
    KRFFS_Root_Node,
    KRFFS_Free_Node,
    KRFFS_Reserved_Node,
};

#pragma pack(push,1)
struct krffs_node_time {
    int64_t seconds;
    int64_t nanoseconds;
};
#pragma pack(pop)

struct krffs_node_time krffs_get_current_node_time();
time_t krffs_get_time_from_node_time(const struct krffs_node_time *node_time);
struct timespec krffs_get_timespec_from_node_time(const struct krffs_node_time *node_time);

#pragma pack(push,1)
struct krffs_node {
    uint16_t magic;
    uint8_t type;
    uint64_t id;
    uint8_t name[KRFFS_FILE_NAME_BUFFER_SIZE];
    uint16_t mode;
    uint32_t uid;
    uint32_t gid;
    struct krffs_node_time access_time;
    struct krffs_node_time modification_time;
    struct krffs_node_time attribute_change_time;
    uint64_t size;
    uint64_t previous_node_size;
    uint64_t data_size;
};
#pragma pack(pop)

struct krffs_node *krffs_initialize_free_node(struct krffs_node *node,
                                              uint64_t size,
                                              uint64_t previous_node_size);

struct krffs_node *krffs_initialize_root_node(struct krffs_node *node,
                                              uint64_t size,
                                              uint64_t previous_node_size);

void krffs_set_reserved_node_name(struct krffs_node *node, const char *name);

struct krffs_node *krffs_wrap_node_position(struct krffs_file_system *file_system,
                                            struct krffs_node *node);

struct krffs_node *krffs_get_previous_node(struct krffs_file_system *file_system,
                                           struct krffs_node *node);

struct krffs_node *krffs_get_next_node(struct krffs_file_system *file_system,
                                       struct krffs_node *node);

uint64_t krffs_get_node_relative_position(struct krffs_file_system *file_system,
                                          struct krffs_node *node);

bool krffs_is_node_in_file_system(struct krffs_file_system *file_system,
                                  struct krffs_node *node);

struct krffs_node *krffs_find_reserved_node_by_name(struct krffs_file_system *file_system,
                                                    const char *name);

struct krffs_node *krffs_find_reserved_or_root_node_by_name(struct krffs_file_system *file_system,
                                                            const char *name);

size_t krffs_count_free_nodes(struct krffs_file_system *file_system);
uint64_t krffs_calculate_total_free_space(struct krffs_file_system *file_system);
size_t krffs_count_reserved_nodes(struct krffs_file_system *file_system);
uint64_t krffs_calculate_total_reserved_space(struct krffs_file_system *file_system);

#endif
