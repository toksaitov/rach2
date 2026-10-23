/* native.h: portable files, processes and reports for host cases. */
#ifndef HOST_NATIVE_H
#define HOST_NATIVE_H
#include <stddef.h>
struct native_case {
    char tree[4096], work[4096];
    unsigned records, failures, commands;
    int uncertain;
};
int native_begin(struct native_case *c, const char *id);
int native_end(struct native_case *c);
void native_assert(struct native_case *c, const char *id, int pass, const char *why);
int native_path(char out[4096], const char *base, const char *name);
int native_tool(struct native_case *c, char out[4096], const char *name);
int native_run(struct native_case *c, const char *const argv[]);
unsigned char *native_read(const char *path, size_t *size);
int native_write(const char *path, const void *data, size_t size);
#endif
