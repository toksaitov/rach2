/**
 * Check the chown and write set-id rules extracted from syscall_fs.c.
 * The setattr mask also distinguishes a change of mode or IDs, which
 * inotify reports, from a change of ctime alone.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/** A user namespace other than the first, which the macro tests only for
 * presence. */
struct userns {
    uint32_t level;
};

struct fixture_proc {
    uint32_t euid;
    uint32_t egid;
    uint32_t fsuid; /* read only while `fs_own` says so; 0 here */
    uint32_t fsgid;
    uint32_t fs_own;
    uint32_t nocaps;
    uint32_t cap_lost;
    struct userns *userns; /* NULL in the first user namespace */
    uint32_t ngroups;
    uint32_t groups[8];
};

static struct fixture_proc caller;
static struct fixture_proc *current_proc = &caller;
static struct userns child_ns = {1};

#define S_IFMT  0170000u
#define S_IFDIR 0040000u
#define S_IFREG 0100000u

#define CAP_CHOWN  0
#define CAP_FOWNER 3
#define CAP_FSETID 4

/** The effective capabilities of this fixture's root. */
static int cap_holds(const struct fixture_proc *p, uint32_t cap)
{
    return p->euid == 0 && !(p->cap_lost & (1u << cap));
}

/** The child namespace in this fixture has empty ID maps. */
static int inode_capable(uint32_t uid, uint32_t gid, uint32_t cap)
{
    (void)uid;
    (void)gid;
    return !caller.userns && cap_holds(&caller, cap);
}

/** The caller owns the inode or holds FOWNER over its mapped owner. */
static int inode_owner_or_capable(uint32_t uid)
{
    return caller.euid == uid || inode_capable(uid, 0, CAP_FOWNER);
}

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
        printf("chown-plan: fail: %s\n", what);
    }
}

/** Set the caller's effective ids and one supplementary group, or none when
 * `group` is 0, in the first user namespace. */
static void as_caller(uint32_t euid, uint32_t egid, uint32_t group)
{
    caller.userns = NULL;
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

/** Root of a user namespace other than the first has no privilege over a
 * file, and is held to an owner's rules. */
static void namespace_rows(void)
{
    struct vfs_stat st;
    int what;

    as_caller(0, 0, 0);
    caller.userns = &child_ns;
    st = attrs(FILE_MODE | 04755u, 1000, 1000);
    what = vfs_chown_plan(&st, 0, 0);
    check(what == -EPERM && st.uid == 1000 && st.gid == 1000 && st.mode == (FILE_MODE | 04755u),
          "root of another user namespace may not take a file it does not own");
    st = attrs(FILE_MODE | 0644u, 0, 0);
    what = vfs_chown_plan(&st, 1000, UNCHANGED);
    check(what == -EPERM && st.uid == 0,
          "root of another user namespace may not give its file away");
    what = vfs_chown_plan(&st, UNCHANGED, 555);
    check(what == -EPERM && st.gid == 0,
          "root of another user namespace may not give its file to a group it is not in");
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

/** A nonowner can request ctime alone but cannot change mode or IDs. */
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
    check(what == -EPERM && st.mode == (FILE_MODE | 04755u),
          "a nonowner cannot clear a set-id file with chown(-1, -1)");
    what = vfs_chown_plan(&st, 1000, 1000);
    check(what == -EPERM && st.uid == 1000 && st.mode == (FILE_MODE | 04755u),
          "a nonowner cannot supply unchanged IDs");
    st = attrs(FILE_MODE | 0644u, 1000, 1000);
    check(vfs_chown_plan(&st, UNCHANGED, UNCHANGED) == VFS_SET_CTIME,
          "a ctime-only chown of a plain file needs no ownership");
}

/** Exercise owner chown without supplied IDs and both write clear rules. */
static void clear_rows(void)
{
    struct vfs_stat st;

    as_caller(1000, 1000, 100);
    st = attrs(FILE_MODE | 06700u, 1000, 2000);
    check(vfs_chown_plan(&st, UNCHANGED, UNCHANGED) == VFS_SET_MODE &&
              st.mode == (FILE_MODE | 0700u),
          "an owner outside the group clears both bits even without supplied IDs");
    st = attrs(FILE_MODE | 02700u, 1000, 100);
    check(vfs_chown_plan(&st, UNCHANGED, UNCHANGED) == VFS_SET_CTIME &&
              st.mode == (FILE_MODE | 02700u),
          "a member of the file's group keeps non-executable setgid");
    st = attrs(FILE_MODE | 0644u, 1000, 1000);
    check(vfs_chown_plan(&st, 1000, 1000) == (VFS_SET_UID | VFS_SET_GID),
          "permitted unchanged IDs remain in the mask for inotify");
    check(write_setid_bits(FILE_MODE | 06700u, 1000, 2000) == 06000u,
          "a writer outside the group clears setgid without group execute");
    check(write_setid_bits(FILE_MODE | 06700u, 1000, 100) == 04000u,
          "a supplementary group preserves setgid without group execute");
    check(write_setid_bits(FILE_MODE | 02710u, 1000, 100) == 02000u,
          "a writer with group execute clears setgid even in the group");
    as_caller(0, 0, 0);
    check(write_setid_bits(FILE_MODE | 06710u, 1000, 2000) == 0,
          "a privileged writer preserves both bits");
    caller.cap_lost = 1u << CAP_FSETID;
    check(write_setid_bits(FILE_MODE | 06710u, 1000, 2000) == 06000u,
          "a writer without FSETID loses both bits even as root");
    st = attrs(FILE_MODE | 06700u, 0, 0);
    check(vfs_chown_plan(&st, UNCHANGED, 2000) == (VFS_SET_MODE | VFS_SET_GID) &&
              st.mode == (FILE_MODE | 0700u),
          "a new outside group clears setgid with setuid");
    caller.cap_lost = 1u << CAP_CHOWN;
    st = attrs(FILE_MODE | 0644u, 1000, 2000);
    check(vfs_chown_plan(&st, 0, UNCHANGED) == -EPERM, "FOWNER alone cannot change the owner");
    caller.cap_lost = 0;
}

/** Run the rows: 0 when every row passes, 1 otherwise. */
int main(void)
{
    privileged_rows();
    namespace_rows();
    owner_rows();
    stranger_rows();
    clear_rows();
    if (fails) {
        printf("chown-plan: result: fail: %d rows\n", fails);
        return 1;
    }
    printf("chown-plan: result: pass: the set-id rules of a mounted chown\n");
    return 0;
}
