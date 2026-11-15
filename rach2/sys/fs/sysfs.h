/*
 * sysfs.h: the entries of /sys that drivers add at run time, directories,
 * attribute files and links, below the fixed directories synthfs.c knows or
 * below each other, and the PCI functions a driver reports with no device
 * behind them.
 */
#ifndef _SYSFS_H_
#define _SYSFS_H_

#include "../lib/kint.h"

/*
 * An entry is known by its handle, a positive number that names it until it
 * is removed and never again after: the slot in bits 0 to 9 and the slot's
 * generation above. A fixed directory of /sys, a node of synthfs.h's
 * SYSFS_* with its key, takes a handle of its own, an anchor, by which
 * entries are added below it (`sysfs_anchor`); an anchor stays until the
 * kernel stops. A name below one directory is taken once, and a name the
 * fixed directory has itself is never reached, since the fixed names are
 * classified first.
 *
 * Every call runs under the big kernel lock, from a probe, a system call or
 * a worker, never from an interrupt, since adding takes heap. A `show` runs
 * at each open of its file and at each read from offset 0, a `store` at each
 * write. Either may sleep, as a driver's that asks its device does, holding
 * a reference to whatever it uses after the sleep, since the entry may be
 * removed meanwhile; a uevent's `show`, which `sysfs_uevent` calls, must
 * not.
 */

#define SYSFS_NODES_MAX 65536 /* entries and anchors at once; the table grows */
#define SYSFS_NAME_MAX  32    /* bytes of a name, with its NUL */
/* Bytes; the largest, i915's `error`, is under 160 KiB. */
#define SYSFS_SHOW_MAX  1048576u

/* The kinds of entry, as `sysfs_node_kind` answers. */
#define SYSFS_NODE_ANCHOR 1 /* a fixed directory of synthfs.h */
#define SYSFS_NODE_DIR    2
#define SYSFS_NODE_ATTR   3
#define SYSFS_NODE_LINK   4

/**
 * Write the value of an attribute into `buf`, `cap` bytes, as text or as
 * bytes: return how many it has, which may exceed `cap`, in which case the
 * caller calls again with room for them, up to SYSFS_SHOW_MAX; or -errno.
 * `obj` and `arg` are what the attribute was added with.
 */
typedef int32_t (*sysfs_show_t)(void *obj, int arg, char *buf, uint32_t cap);

/**
 * Take a write of `len` bytes at `buf`, a kernel copy that ends in no NUL:
 * return `len`, or -errno. The open has checked the writer against the
 * file's mode, as Linux's generic_permission does.
 */
typedef int32_t (*sysfs_store_t)(void *obj, int arg, const char *buf, uint32_t len);

/** The anchor of the fixed directory `node`, a SYSFS_* of synthfs.h, of the
 * device `key`, made on its first use; -ENOSPC when the table is full, or
 * -ENOMEM. */
int sysfs_anchor(int node, int key);

/** Add the directory `name` below the directory or anchor `parent`; its
 * handle, or -errno: -ENOENT for a parent that is gone or no directory,
 * -EEXIST for a name taken, -ENAMETOOLONG, -ENOSPC or -ENOMEM. */
int sysfs_add_dir(int parent, const char *name);

/* An attribute file: its name, its permission bits (0444 to read, 0644 or
 * 0200 when `store` takes writes), and how it is read and written, either
 * of which may be NULL. Drivers keep these in tables. */
struct sysfs_attr {
    const char *name;
    uint32_t mode;
    sysfs_show_t show;
    sysfs_store_t store;
};

/** Add the attribute file `attr` below `parent`, whose `show` and `store`
 * are given `obj` and `arg`; its handle, or -errno as `sysfs_add_dir`. */
int sysfs_add_attr(int parent, const struct sysfs_attr *attr, void *obj, int arg);

/**
 * The directory /sys/module/MODULE of the module `module`, made with
 * /sys/module on its first use, where a built-in driver that Linux builds
 * as a module shows it loaded; its handle, or -errno as `sysfs_add_dir`,
 * with `*made` as `sysfs_module_params` sets it.
 */
int sysfs_module_dir(const char *module, int *made);

/**
 * The directory /sys/module/MODULE/parameters of the module `module`, made
 * with the directories above it on its first use, where a driver adds its
 * module's parameters as Linux's module_param shows them; its handle, or
 * -errno as `sysfs_add_dir`, which leaves none of the directories the call
 * made. `*made` is the highest directory the call made, 0 when all were
 * there, for `sysfs_module_params_undo`.
 */
int sysfs_module_params(const char *module, int *made);

/**
 * Take away a module's parameters that failed part way, as Linux's
 * module_param_sysfs_setup leaves none of its group: the directory `made`
 * of `sysfs_module_params`, with the files added below it, or, when it is
 * 0, the `n` files at `added`, so that the directories and files that
 * other modules share or own stay.
 */
void sysfs_module_params_undo(int made, const int *added, int n);

/** Add the link `name` below `parent` to the directory or anchor `target`,
 * which a path through the link reaches and `readlink` gives relative to
 * the link; its handle, or -errno as `sysfs_add_dir`, and -ENOENT for a
 * target that is gone or no directory. */
int sysfs_add_link(int parent, const char *name, int target);

/** Add the file `uevent` below the directory `dir`, mode 0644, read through
 * `show`, whose text is the device's variables, a `KEY=value` on each line,
 * which every uevent of the directory carries; a privileged write of an
 * action sends that uevent (`sysfs_uevent_write`). Its handle, or -errno as
 * `sysfs_add_attr`. */
int sysfs_add_uevent(int dir, sysfs_show_t show, void *obj, int arg);

/**
 * Send the uevent `action` ("add", "remove", "change", ...) of the
 * directory `dir` to the listeners of NETLINK_KOBJECT_UEVENT, as Linux's
 * kobject_uevent_env does: `action@DEVPATH`, then ACTION, DEVPATH (the
 * directory's path less "/sys"), SUBSYSTEM (the last name of the target of
 * the directory's `subsystem` link), the lines of `env`, `KEY=value` each
 * ended by a newline, or NULL, the variables of the directory's `uevent`
 * file, and SEQNUM, each ended by a NUL. 0, -ENOENT for a directory that is
 * gone, -ENOMEM, or -E2BIG when the variables pass 2048 bytes, Linux's
 * limit, which leaves out the first line. A "remove" is
 * sent before the directory is removed, while its files are there.
 */
int32_t sysfs_uevent(int dir, const char *action, const char *env);

/**
 * Take a write of a `uevent` file of `dir`, as Linux's kobject_synth_uevent:
 * an action of add, remove, change, move, online, offline, bind or unbind,
 * then optionally a UUID and `KEY=value` words, sent as that uevent with
 * SYNTH_UUID (0 without one) and SYNTH_ARG_KEY=value. `len`, or -EINVAL.
 */
int32_t sysfs_uevent_write(int dir, const char *buf, uint32_t len);

/** Send the uevent `action` of the device whose directory is `path`,
 * "/sys/...", as `sysfs_uevent` does, with SUBSYSTEM `subsystem` (or none
 * for NULL), the lines of `env`, and the `vars_len` bytes of variables at
 * `vars`, lines of `KEY=value` (or none for NULL): for a fixed directory's
 * device, which has no entry here. 0, -ENOMEM or -E2BIG. */
int32_t sysfs_uevent_path(const char *path, const char *subsystem, const char *action,
                          const char *env, const char *vars, uint32_t vars_len);

#define SYSFS_SYNTH_ENV_MAX 256

/** Parse a write of a `uevent` file, as `sysfs_uevent_write` takes it, into
 * the action, `action` of 8 bytes, and the lines of its variables, `env` of
 * SYSFS_SYNTH_ENV_MAX bytes. 0, or -EINVAL for a word that is no action, a
 * UUID not of 36 characters, an argument without `=` or too many. */
int32_t sysfs_uevent_parse(const char *buf, uint32_t len, char *action, char *env);

/** The number of the last uevent sent, /sys/kernel/uevent_seqnum. */
uint32_t sysfs_uevent_seqnum(void);

/** Remove the entry `handle` and every entry below it. A link to a removed
 * directory stays and leads nowhere (ENOENT) until its owner removes it. An
 * anchor, or a handle that is gone, is left alone. */
void sysfs_remove(int handle);

/* What synthfs.c, procfs.c and the system calls read. */

/** The anchor of `node` and `key`, or 0 when none was made. */
int sysfs_anchor_find(int node, int key);

/** The kind of entry `handle`, SYSFS_NODE_*, or 0 for one that is gone. */
int sysfs_node_kind(int handle);

/** The entry named by the `len` bytes at `name` below the directory or
 * anchor `dir`, or 0. */
int sysfs_node_child(int dir, const char *name, int len);

/** The number of entries below `dir`, and the handle of the `n`th of them,
 * in the order they were added, or 0 past the last. */
int sysfs_node_count(int dir);
int sysfs_node_nth(int dir, int n);

/** The name of `handle`, or "" for an anchor or one that is gone. */
const char *sysfs_node_name(int handle);

/** The directory or anchor `handle` is below, or 0. */
int sysfs_node_parent(int handle);

/** The fixed node and key of the anchor `handle`, or -1 and 0. */
void sysfs_node_anchor(int handle, int *node, int *key);

/** The target of the link `handle`, or 0. */
int sysfs_node_target(int handle);

/** The permission bits of the attribute `handle`, or 0. */
uint32_t sysfs_node_mode(int handle);

/** Whether the attribute `handle` can be read: it has a show and a read
 * bit in its mode. */
int sysfs_node_readable(int handle);

/** Whether the attribute `handle` takes writes. */
int sysfs_node_writable(int handle);

/** Call the attribute's `show`, as sysfs_show_t says; -ENOENT for one that
 * is gone, -EACCES for one with none. */
int32_t sysfs_node_show(int handle, char *buf, uint32_t cap);

/** Call the attribute's `store`; -ENOENT for one that is gone, -EACCES for
 * one with none. */
int32_t sysfs_node_store(int handle, const char *buf, uint32_t len);

/** The path of the entry or anchor `handle`, "/sys/...", into `out` of
 * `cap` bytes, NUL-terminated; "/sys" for one that is gone. */
void sysfs_node_path(int handle, char *out, int cap);

/** The target of the link `handle` relative to the directory it is in, as
 * Linux writes a link of sysfs ("../../devices/..."), into `out` of `cap`
 * bytes, unterminated; its length, or -ENOENT. */
int sysfs_link_text(int handle, char *out, int cap);

/*
 * Reported PCI identities: a driver that stands in for a device reports the
 * identity Linux would read from the function's configuration space. A
 * function with no device behind it then has its directory, its attributes
 * and its links under /sys/devices/pci0000:00 and /sys/bus/pci/devices as a
 * real one has, made from that identity; a real function at the same
 * address shows the reported identity in place of its own, in its files,
 * its uevent and the header of its `config`.
 */
#define SYSFS_PCI_VIRTUAL_MAX 8

struct sysfs_pci_ident {
    uint16_t vendor, device;
    uint16_t subsystem_vendor, subsystem_device;
    uint32_t class; /* class, subclass and programming interface, 24 bits */
    uint8_t revision;
    uint8_t boot_vga; /* 1: the function the firmware's display used */
};

/** Report the function `key`, bus << 8 | device << 3 | function, with the
 * identity `id`, which is copied. 0, -EEXIST for a key reported already, or
 * -ENOSPC past SYSFS_PCI_VIRTUAL_MAX. */
int sysfs_pci_virtual_add(int key, const struct sysfs_pci_ident *id);

/** Withdraw the function `key`. */
void sysfs_pci_virtual_remove(int key);

/** The identity reported for `key` into `*id`: 1, or 0 for none. */
int sysfs_pci_virtual(int key, struct sysfs_pci_ident *id);

/** The key of the `n`th function reported, or -1 past the last. */
int sysfs_pci_virtual_nth(int n);

/*
 * PCI functions bound to a driver of the kernel, shown as Linux's driver
 * core shows them: /sys/bus/pci/drivers/DRIVER, made on the first binding
 * to it, with a link named by the function's address to its directory, the
 * function's `driver` link to the driver's directory, and DRIVER= in its
 * uevent. libudev reads a device's driver from that link, as IGT's device
 * scan and Mesa's loader do. vfio-pci makes its own links (vfio/group.c).
 */
#define SYSFS_PCI_BOUND_MAX 32

/** Show the function `key`, bus << 8 | device << 3 | function, bound to
 * `driver`, a string that outlives the binding: 0, also when it is shown
 * bound to `driver` already; -EBUSY when it is shown bound to another;
 * -ENOSPC past SYSFS_PCI_BOUND_MAX; or the errors of making the entries,
 * with none of them left. */
int sysfs_pci_bind(int key, const char *driver);

/** Take away what `sysfs_pci_bind` showed for `key`; nothing when none. */
void sysfs_pci_unbind(int key);

/** The driver the function `key` is shown bound to, or NULL. */
const char *sysfs_pci_driver(int key);

/**
 * Send the uevent `action` of the function `key` with its variables, its
 * DRIVER among them, and SUBSYSTEM=pci, as a write of its `uevent` sends
 * it. A driver sends "bind" once its probe has succeeded and its device is
 * registered, as Linux's driver_bound does after really_probe. 0, -ENOENT
 * for a function that is gone, -ENOMEM or -E2BIG. procfs.c, which writes
 * the variables, defines it.
 */
int32_t sysfs_pci_uevent(int key, const char *action);

#endif /* _SYSFS_H_ */
