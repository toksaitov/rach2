/* Native file identity, access and gate operations for routine authority. */
#ifndef ETUDE_AUTHORITY_PLATFORM_H
#define ETUDE_AUTHORITY_PLATFORM_H
#include <stdint.h>
#include <stddef.h>
#include "include/etude/exec.h"
struct authority_file_id {
    uint64_t device, inode;
};
struct authority_gate {
    intptr_t value;
};
int authority_file_id(const char *path, struct authority_file_id *identity);
int authority_protected(const char *path, const char *operator_id, int private_file);
int authority_operator(const char *operator_id);
int authority_gate_open(const char *path, int exclusive, struct authority_gate *gate);
void authority_gate_close(struct authority_gate *gate);
int authority_publish(const char *path, const void *bytes, size_t size, int private_file);
const char *authority_installed_binding(void);
int authority_nonce(char out[65]);
#endif
