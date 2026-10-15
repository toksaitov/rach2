/*
 * The namespace calls of proc/ns.c for a host test, as they answer on a
 * kernel where every process is in the first namespaces: ids and pids are
 * the kernel's, there is no reaper but init, and there are no namespace
 * files. A test includes it after the kernel sources it compiles.
 */
#ifndef NS_FIRST_H
#define NS_FIRST_H

#include "lib/kerrno.h"
#include "proc/ns.h"

struct userns init_userns;
struct pidns init_pidns;
struct mntns init_mntns;
struct netns init_netns;
struct utsns init_utsns;
struct ipcns init_ipcns;

/** The first user namespace, whatever `p`. */
struct userns *proc_userns(const process_t *p)
{
    (void)p;
    return &init_userns;
}

/** The first PID namespace, whatever `p`. */
struct pidns *proc_pidns(const process_t *p)
{
    (void)p;
    return &init_pidns;
}

/** The first mount namespace, whatever `p`. */
struct mntns *proc_mntns(const process_t *p)
{
    (void)p;
    return &init_mntns;
}

/** The first network namespace, whatever `p`. */
struct netns *proc_netns(const process_t *p)
{
    (void)p;
    return &init_netns;
}

/** The first UTS namespace, whatever `p`. */
struct utsns *proc_utsns(const process_t *p)
{
    (void)p;
    return &init_utsns;
}

/** The first IPC namespace, whatever `p`. */
struct ipcns *proc_ipcns(const process_t *p)
{
    (void)p;
    return &init_ipcns;
}

/** Whether `p` is privileged; the first user namespace owns every `c`. */
int ns_owner_privileged(const process_t *p, const struct ns_common *c)
{
    (void)c;
    return proc_privileged(p);
}

/** The kernel id `kid` itself, which the first user namespace maps. */
uint32_t ns_uid_from_kernel(const struct userns *ns, uint32_t kid)
{
    (void)ns;
    return kid;
}

/** The kernel user id `kid` itself. */
uint32_t uid_seen(uint32_t kid)
{
    return kid;
}

/** The kernel group id `kid` itself. */
uint32_t gid_seen(uint32_t kid)
{
    return kid;
}

/** The user id `id` itself, never -1. */
int64_t uid_named(uint32_t id)
{
    return id;
}

/** The group id `id` itself, never -1. */
int64_t gid_named(uint32_t id)
{
    return id;
}

/** The kernel id `kid` itself, its PID in the first PID namespace. */
uint32_t pid_seen(uint32_t kid)
{
    return kid;
}

/** The PID `pid` itself, its kernel id in the first PID namespace. */
uint32_t pid_named(uint32_t pid)
{
    return pid;
}

/** The kernel id `kid` itself, whatever `viewer`. */
uint32_t pid_seen_by(const process_t *viewer, uint32_t kid)
{
    (void)viewer;
    return kid;
}

/** Nothing: no process has a reference to a namespace. */
void ns_release(process_t *p)
{
    (void)p;
}

/** 0, the system's init, which is the reaper of every process. */
uint32_t ns_reaper(const process_t *p)
{
    (void)p;
    return 0;
}

/** Nothing: no process is the init of a PID namespace of its own. */
void ns_reaper_gone(const process_t *p)
{
    (void)p;
}

/** NULL for every kind, as there are no namespace files. */
struct ns_common *ns_of(const process_t *p, int kind)
{
    (void)p;
    (void)kind;
    return NULL;
}

/** Nothing: the first namespaces are never freed. */
void ns_put(struct ns_common *c)
{
    (void)c;
}

/** The empty string in `out`. */
void ns_link_name(const struct ns_common *c, char *out)
{
    (void)c;
    out[0] = '\0';
}

/** -ENOENT, as there are no namespace files. */
int32_t nsfs_open(struct ns_common *c, uint32_t oflag)
{
    (void)c;
    (void)oflag;
    return -ENOENT;
}

/** NULL: no descriptor is open on a namespace. */
struct ns_common *nsfs_ns(const struct fd_entry *e)
{
    (void)e;
    return NULL;
}

#endif /* NS_FIRST_H */
