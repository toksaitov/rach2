/* native_prepare.h: the immutable payload of the local Rach2 collection. */
#ifndef ETUDE_NATIVE_PREPARE_H
#define ETUDE_NATIVE_PREPARE_H
#include <signal.h>
#include <stdint.h>
#include "json.h"

/* The programs preparation runs, by their index in `np_context.tools`. */
enum np_tool {
    NP_MKE2FS,
    NP_DEBUGFS,
    NP_E2FSCK,
    NP_MFORMAT,
    NP_MKFS,  /* build/krffs/mkfs.krffs */
    NP_STAGE, /* build/krffs/stageimage.krffs */
    NP_EXFAT, /* build/tests/host/mkexfatfixture */
    NP_GIT,
    NP_GZIP,
    NP_TOOLS
};

/* The leading byte count of a KRFFS node, and the volume's block size. */
#define NP_KRFFS_HEADER 357
#define NP_KRFFS_BLOCK  4096

/* The root file that brings a private root's free extent to a block. */
#define NP_ROOT_PADDING "/.etude-native-root-padding"

struct np_context {
    char tree[4096]; /* the absolute root of the source tree */
    char work[4096]; /* an absolute private directory, removed at the end */
    const char *arch;
    const char *build; /* "build" for i686, "build/x86_64" for x86-64 */
    char *tools[NP_TOOLS];
    char **environment; /* the tools' environment */
    volatile sig_atomic_t *stop;
};

/* One case of the inventory: its name, fixtures and command. */
struct np_case {
    char *name;
    char **fixtures, **argv;
    size_t nfixtures, nargv;
};

int etude_native_prepare_main(int argc, char **argv, volatile sig_atomic_t *stop);
int np_run(struct np_context *c, char *const argv[], struct jbuf *output);
int np_blank(const char *path, long mib);
int np_krffs_free_tail(const char *path, int reject_padding, uint64_t *used, uint64_t *free_bytes);
int np_root_image(struct np_context *c, const char *out, const char *kind, long mib,
                  const char *text);
int np_images(struct np_context *c, const char *output, const struct np_case *cases, size_t n);
int np_archive(struct np_context *c, const char *payload, const char *archive, char hex[65]);
int np_remove(const char *path);
int np_file_hash(const char *path, char hex[65]);
int np_path(char *path, size_t size, const char *format, ...);
char *np_strdup(const char *text);
#endif
