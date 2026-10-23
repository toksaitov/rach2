/* collection.h: the prepared native inventory and local framed attempts. */
#ifndef NATIVE_COLLECTION_H
#define NATIVE_COLLECTION_H
#include <stddef.h>
#include "etude/native.h"
#define NC_CASE_MAX 512
#define NC_WORD_MAX 96
struct nc_case {
    char *line, *name;
    char *fixtures[NC_WORD_MAX], *argv[NC_WORD_MAX];
    size_t nfixtures, argc;
    int proof;
};
struct nc_map {
    char kind[16];
    unsigned old, native;
};
struct nc_image {
    char name[128], type[16];
    unsigned mib;
};
struct nc_root {
    char name[128], image[128];
};
struct nc_inventory {
    struct nc_case cases[NC_CASE_MAX];
    size_t ncases;
    struct nc_map maps[256];
    size_t nmaps;
    struct nc_root roots[NC_CASE_MAX];
    size_t nroots;
    struct nc_image images[128];
    size_t nimages;
    char account[80];
    unsigned uid;
};
struct etude_reading;
struct nc_config {
    const struct etude_reading *granted;
    const char *tree, *inventory, *images, *guest, *state, *stage, *results, *run, *self;
    const char *purpose, *machine, *kernel_sha256;
    const char *arch, *peer, *outside, *interface, *address, *gateway, *far, *empty, *silent;
};
struct nc_attempt {
    struct etude_native_spec spec;
    char *files[16], *fixtures[16], *environment[32], *unset[32], *argv[NC_WORD_MAX];
    char *transport[32];
    char *allocated[256];
    size_t nallocated, argc;
    char why[256];
};
int etude_native_collection_main(int argc, char **argv);
int nc_assets(const struct nc_config *conf);
int nc_inventory_read(struct nc_inventory *out, const struct nc_config *conf, int with_roots);
void nc_inventory_free(struct nc_inventory *out);
int nc_image_has(const struct nc_inventory *inventory, const char *name, const char *type);
int nc_map_value(const struct nc_inventory *i, const char *kind, unsigned old, unsigned *value);
int nc_attempt_make(struct nc_attempt *a, const struct nc_config *conf,
                    const struct nc_inventory *inventory, const struct nc_case *c);
void nc_attempt_free(struct nc_attempt *a);
char *nc_text(struct nc_attempt *a, const char *format, ...);
int nc_mkdir(const char *path);
struct nc_history;
int nc_run(const struct nc_config *conf, const struct nc_inventory *inventory,
           const struct nc_case *c, struct nc_history *history, size_t index);
int nc_collection(struct nc_config *conf, const struct nc_inventory *inventory, int argc,
                  char **argv, int first);
int nc_proof(struct nc_config *conf);
int nc_proof_prepare(struct nc_attempt *a, const struct nc_config *conf, const char *directory,
                     int mode);
int nc_proof_judge(struct nc_attempt *a, struct etude_native_result *result, int mode);
int nc_cut_transport(int argc, char **argv);
#endif
