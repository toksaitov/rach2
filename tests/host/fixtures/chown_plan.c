/**
 * The set-id rules of a chown on a mounted file system.
 *
 * tests/host/chown_mutate.sh extracts `vfs_chown_plan` and the three
 * credential helpers it calls unchanged from a copy of
 * rach2/sys/syscall_fs.c into chown_plan.inc, with `struct vfs_stat` and
 * the `VFS_SET_` masks from rach2/sys/fs/vfs.h and `EPERM` from
 * rach2/sys/lib/kerrno.h. The chown by path and the chown by descriptor
 * both decide through that plan for every volume but the boot root, so
 * these rows are their rule: a directory keeps its set-id bits, setuid goes
 * with any change of owner or group, and setgid goes only when
 * group-execute is set beside it, which is the rule `cred_kill_setid`
 * applies on the boot root. That the two calls reach the plan is shown on
 * an ext2 root by `modeprobe`, not here. The caller's record has only the
 * fields the helpers read.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct fixture_proc {
    uint32_t euid;
    uint32_t egid;
    uint32_t nocaps;
    uint32_t ngroups;
    uint32_t groups[8];
};

static struct fixture_proc caller;
static struct fixture_proc *current_proc = &caller;

/* `cred_privileged` calls this. The macro in rach2/sys/proc/process.h also
 * requires that the process have no user namespace, which this record lacks. */
#define proc_privileged(p) ((p)->euid == 0 && !(p)->nocaps)

#include "chown_plan.inc"

#define DIR_MODE  0040000u
#define FILE_MODE 0100000u
#define UNCHANGED ((uint32_t)-1)

static int fails;

/** Count and print a failed row named `what`. */
static void check(int ok, const char *what)
{
    if (!ok) {
        fails++;
        printf("chown-proof: fail: %s\n", what);
    }
}

/** Set the caller's effective ids and one supplementary group, or none when
 * `group` is 0. */
static void as_caller(uint32_t euid, uint32_t egid, uint32_t group)
{
    caller.euid = euid;
    caller.egid = egid;
    caller.ngroups = group ? 1u : 0u;
    caller.groups[0] = group;
}

/** A node's attributes as `vfs_getattr` reports them. */
static struct vfs_stat attrs(uint32_t mode, uint32_t uid, uint32_t gid)
{
    struct vfs_stat st;

    memset(&st, 0, sizeof(st));
    st.mode = mode;
    st.uid = uid;
    st.gid = gid;
    return st;
}

/** Root changes owners: the set-id bits are what the rule decides. */
static void privileged_rows(void)
{
    struct vfs_stat st;
    int what;

    as_caller(0, 0, 0);
    st = attrs(DIR_MODE | 06755u, 0, 0);
    what = vfs_chown_plan(&st, 1000, 1000);
    check(what == (VFS_SET_UID | VFS_SET_GID) && st.mode == (DIR_MODE | 06755u) && st.uid == 1000 &&
              st.gid == 1000,
          "a directory keeps its set-id bits across a change of owner");
    st = attrs(FILE_MODE | 04755u, 0, 0);
    what = vfs_chown_plan(&st, 1000, 1000);
    check(what == (VFS_SET_UID | VFS_SET_GID | VFS_SET_MODE) && st.mode == (FILE_MODE | 0755u),
          "a file loses setuid when its owner changes");
    st = attrs(FILE_MODE | 02755u, 0, 0);
    what = vfs_chown_plan(&st, 1000, UNCHANGED);
    check(what == (VFS_SET_UID | VFS_SET_MODE) && st.mode == (FILE_MODE | 0755u) && st.gid == 0,
          "setgid goes with a change of owner when group-execute is set");
    st = attrs(FILE_MODE | 02700u, 0, 0);
    what = vfs_chown_plan(&st, 1000, 0);
    check((what & VFS_SET_UID) && st.mode == (FILE_MODE | 02700u) && st.uid == 1000,
          "setgid stays through a change of owner when group-execute is clear");
    st = attrs(FILE_MODE | 0644u, 1000, 1000);
    what = vfs_chown_plan(&st, 2000, 2000);
    check(what == (VFS_SET_UID | VFS_SET_GID) && st.uid == 2000 && st.gid == 2000 &&
              st.mode == (FILE_MODE | 0644u),
          "root gives a file to any owner and group, and a mode without set-id bits stays");
}

/** An owner without privilege changes the group within its own groups, and
 * nothing else. */
static void owner_rows(void)
{
    struct vfs_stat st;
    int what;

    as_caller(1000, 1000, 100);
    st = attrs(FILE_MODE | 04755u, 1000, 1000);
    what = vfs_chown_plan(&st, UNCHANGED, 100);
    check(what == (VFS_SET_GID | VFS_SET_MODE) && st.gid == 100 && st.mode == (FILE_MODE | 0755u),
          "an owner gives a file to a supplementary group of its own, and setuid goes");
    st = attrs(FILE_MODE | 0644u, 1000, 100);
    what = vfs_chown_plan(&st, UNCHANGED, 1000);
    check(what == VFS_SET_GID && st.gid == 1000, "an owner gives a file to its effective group");
    st = attrs(FILE_MODE | 04755u, 1000, 1000);
    what = vfs_chown_plan(&st, 2000, UNCHANGED);
    check(what == -EPERM && st.uid == 1000 && st.mode == (FILE_MODE | 04755u),
          "an owner may not give a file away, and the refusal changes nothing");
    what = vfs_chown_plan(&st, UNCHANGED, 555);
    check(what == -EPERM && st.gid == 1000 && st.mode == (FILE_MODE | 04755u),
          "an owner may not give a file to a group it is not in");
}

/** A caller that does not own the file changes nothing, and needs no right
 * for a request that changes nothing. */
static void stranger_rows(void)
{
    struct vfs_stat st;
    int what;

    as_caller(1001, 1000, 100);
    st = attrs(FILE_MODE | 04755u, 1000, 1000);
    what = vfs_chown_plan(&st, UNCHANGED, 100);
    check(what == -EPERM && st.gid == 1000,
          "a caller that does not own a file may not change its group");
    what = vfs_chown_plan(&st, UNCHANGED, UNCHANGED);
    check(what == 0 && st.mode == (FILE_MODE | 04755u),
          "a chown that changes nothing is permitted to any caller and keeps the set-id bits");
    what = vfs_chown_plan(&st, 1000, 1000);
    check(what == 0 && st.uid == 1000 && st.mode == (FILE_MODE | 04755u),
          "a chown to the owner and group the file has changes nothing and is permitted");
}

/** Run the rows: 0 when every row passes, 1 otherwise. */
int main(void)
{
    privileged_rows();
    owner_rows();
    stranger_rows();
    if (fails) {
        printf("chown-proof: result: fail: %d rows\n", fails);
        return 1;
    }
    printf("chown-proof: result: pass: the set-id rules of a mounted chown\n");
    return 0;
}
