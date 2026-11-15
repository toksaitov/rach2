/*
 * drm_auth.c: the master role of a card and the authentication of its
 * clients, as Linux 6.12's drm_auth.c: the first opener of a card with no
 * master becomes master, SET_MASTER and DROP_MASTER by a former master's
 * own process or by root, and magic tokens the master spends to
 * authenticate a client.
 */

#include "drm.h"

#include "../lib/kerrno.h"
#include "../proc/percpu.h"
#include "../proc/process.h"

/** Make `file` the device's master, and tell the listeners. */
static void set_master(struct drm_device *dev, struct drm_file *file)
{
    dev->master = file;
    file->was_master = 1;
    drm_master_changed(dev, file);
}

/** Give `file` a master record of its own, authenticated, and make it the
 * master, as Linux's drm_new_set_master. */
static void new_set_master(struct drm_device *dev, struct drm_file *file)
{
    file->master_id = ++dev->next_master_id;
    file->set_busid = 0;
    file->is_master = 1;
    file->authenticated = 1;
    set_master(dev, file);
}

void drm_master_open(struct drm_file *file)
{
    struct drm_device *dev = file->dev;

    if (!dev->master) {
        new_set_master(dev, file);
    } else {
        file->master_id = dev->master->master_id;
        file->set_busid = dev->master->set_busid;
    }
}

void drm_master_release(struct drm_file *file)
{
    struct drm_device *dev = file->dev;

    /* The magic goes with the file, which leaves the device's list. */
    file->magic = 0;
    if (drm_is_current_master(file)) {
        dev->master = NULL;
        drm_master_changed(dev, NULL);
    }
}

/** Whether `magic` is taken in the master record `master_id` of `dev`, by
 * any file, spent or not, as Linux's magic map keeps a spent id. */
static int magic_taken(const struct drm_device *dev, uint32_t master_id, drm_magic_t magic)
{
    const struct drm_file *f;

    for (f = dev->files; f; f = f->dev_next) {
        if (f->master_id == master_id && f->magic == magic) {
            return 1;
        }
    }
    return 0;
}

/** GET_MAGIC: the file's magic, given the first time as the lowest free in
 * its master record's namespace, from 1. */
int32_t drm_getmagic(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_auth *auth = (struct drm_auth *)data;
    drm_magic_t m = 1;

    if (!file->magic) {
        while (magic_taken(dev, file->master_id, m)) {
            m++;
        }
        file->magic = m;
    }
    auth->magic = file->magic;
    return 0;
}

/** AUTH_MAGIC, for the master: authenticate the file whose magic in the
 * master's record is the one given, and spend it. EINVAL for none. */
int32_t drm_authmagic(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_auth *auth = (struct drm_auth *)data;
    struct drm_file *f;

    if (!auth->magic) {
        return -EINVAL;
    }
    for (f = dev->files; f; f = f->dev_next) {
        if (f->master_id == file->master_id && f->magic == auth->magic && !f->magic_spent) {
            f->authenticated = 1;
            f->magic_spent = 1;
            return 0;
        }
    }
    return -EINVAL;
}

/**
 * Whether the caller may set or drop the master role through `file`, as
 * Linux's drm_master_check_perm: a file that was master, used by the
 * process that held it, or root. 0 or -EACCES.
 */
static int32_t master_check_perm(const struct drm_file *file)
{
    if (file->was_master && current_proc && file->pid == current_proc->tgid) {
        return 0;
    }
    return current_proc && proc_privileged(current_proc) ? 0 : -EACCES;
}

/** SET_MASTER: 0 for the master; EBUSY while another file is master; a
 * file that made no record of its own makes one. */
int32_t drm_setmaster_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    int32_t rc = master_check_perm(file);

    (void)data;
    if (rc != 0 || drm_is_current_master(file)) {
        return rc;
    }
    if (dev->master) {
        return -EBUSY;
    }
    if (!file->master_id) {
        return -EINVAL;
    }
    if (!file->is_master) {
        new_set_master(dev, file);
    } else {
        set_master(dev, file);
    }
    return 0;
}

/** DROP_MASTER: the master gives up the role, and the console may take the
 * screen back; EINVAL for any other file. */
int32_t drm_dropmaster_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    int32_t rc = master_check_perm(file);

    (void)data;
    if (rc != 0) {
        return rc;
    }
    if (!drm_is_current_master(file)) {
        return -EINVAL;
    }
    dev->master = NULL;
    drm_master_changed(dev, NULL);
    drm_client_dev_restore(dev);
    return 0;
}
