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

struct userns *proc_userns(const process_t *p)
{
    (void)p;
    return &init_userns;
}

struct pidns *proc_pidns(const process_t *p)
{
    (void)p;
    return &init_pidns;
}

struct mntns *proc_mntns(const process_t *p)
{
    (void)p;
    return &init_mntns;
}

struct netns *proc_netns(const process_t *p)
{
    (void)p;
    return &init_netns;
}

struct utsns *proc_utsns(const process_t *p)
{
    (void)p;
    return &init_utsns;
}

struct ipcns *proc_ipcns(const process_t *p)
{
    (void)p;
    return &init_ipcns;
}

int ns_owner_privileged(const process_t *p, const struct ns_common *c)
{
    (void)c;
    return proc_privileged(p);
}

uint32_t ns_uid_from_kernel(const struct userns *ns, uint32_t kid)
{
    (void)ns;
    return kid;
}

uint32_t uid_seen(uint32_t kid)
{
    return kid;
}

uint32_t gid_seen(uint32_t kid)
{
    return kid;
}

int64_t uid_named(uint32_t id)
{
    return id;
}

int64_t gid_named(uint32_t id)
{
    return id;
}

uint32_t pid_seen(uint32_t kid)
{
    return kid;
}

uint32_t pid_named(uint32_t pid)
{
    return pid;
}

uint32_t pid_seen_by(const process_t *viewer, uint32_t kid)
{
    (void)viewer;
    return kid;
}

void ns_release(process_t *p)
{
    (void)p;
}

uint32_t ns_reaper(const process_t *p)
{
    (void)p;
    return 0;
}

void ns_reaper_gone(const process_t *p)
{
    (void)p;
}

struct ns_common *ns_of(const process_t *p, int kind)
{
    (void)p;
    (void)kind;
    return NULL;
}

void ns_put(struct ns_common *c)
{
    (void)c;
}

void ns_link_name(const struct ns_common *c, char *out)
{
    (void)c;
    out[0] = '\0';
}

int32_t nsfs_open(struct ns_common *c, uint32_t oflag)
{
    (void)c;
    (void)oflag;
    return -ENOENT;
}

struct ns_common *nsfs_ns(const struct fd_entry *e)
{
    (void)e;
    return NULL;
}

#endif /* NS_FIRST_H */
