/*
 * fixture.h: the fixtures etude-guest makes for a native case, beside its
 * workspace, and the arguments that give their values to the program.
 * tools/etude/Readme.md, "Native runs", gives the contract.
 */
#ifndef ETUDE_GUEST_FIXTURE_H
#define ETUDE_GUEST_FIXTURE_H

#include "../history_json.h"
#include <stddef.h>

#define FIX_VOLUMES     4
#define FIX_PAIRS       32
#define FIX_PATH        1024
#define FIX_ARG         1024
#define FIX_PEER_VALUES 8

/* A volume: an image of the invocation attached to a loop device. */
struct fix_volume {
    char type[16];    /* krffs, ext2, vfat, exfat, or ext2-dir for a root */
    char image[160];  /* the file of the invocation */
    int mounted;      /* on volN of the workspace, or attached alone */
    int loop;         /* the device's number, or -1 */
    int global_mount; /* mounted in the machine's namespace by etude-guest */
};

/* The fixtures of one invocation and what was made of them. */
struct fixtures {
    long id_old[FIX_PAIRS], id_new[FIX_PAIRS];
    int nids;
    long port_old[FIX_PAIRS], port_new[FIX_PAIRS];
    int nports;
    struct fix_volume vol[FIX_VOLUMES];
    int nvol;
    int has_root;
    struct fix_volume root; /* its type may also be ext2-dir */
    char root_path[256];    /* the program's path inside the root */
    char root_at[FIX_PATH]; /* the root: STATE/r, or STATE/a/TOKEN/r for ext2-dir */
    long tmpfs_bytes;       /* 0 for none */
    int tmpfs_global;
    int mountns, ipcns, utsns, pty, log, alone, console;
    int private_dir;                      /* WORK/private, removed once the case's processes end */
    char peer[96];                        /* ADDRESS:PORT, or empty */
    char peer_value[FIX_PEER_VALUES][96]; /* NAME=VALUE arguments of the peer */
    int npeer_values;
    int pty_master;
    char pty_slave[64];
    char *log_before;
    long log_before_len;
};

/** Whether the invocation has a file called `name`; given by the caller. */
typedef int (*fix_has_file)(const void *ctx, const char *name);

const char *fixture_parse(struct fixtures *fx, const struct eh_json *list, fix_has_file has,
                          const void *ctx, const char **why);
int fixture_simple(const struct fixtures *fx);
const char *fixture_prepare(struct fixtures *fx, const char *dir, const char **why);
int fixture_args(const struct fixtures *fx, char **argv, int n, int cap);
int fixture_enter(const struct fixtures *fx, const char *work, char *prog, size_t cap,
                  const char **dir);
int fixture_cleanup(struct fixtures *fx, const char *dir);
int fixture_settle(const char *dir);

#endif
