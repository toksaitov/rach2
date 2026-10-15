#ifndef _ANON_H_
#define _ANON_H_

/*
 * The header of the objects behind descriptors that name no file: an
 * eventfd, an epoll set, a timerfd, an inotify instance, a signalfd, a
 * message queue and a namespace file. Such a descriptor has type
 * FD_TYPE_ANON and a `priv` that points at an object beginning with this
 * header. /proc/PID/fd shows its link as "anon_inode:NAME", as Linux does;
 * a message queue's as "/NAME", the path of its file in the mqueue file
 * system, with " (deleted)" once mq_unlink has taken the name; and a
 * namespace file's as its NAME alone, "KIND:[INUM]", as nsfs's.
 */
struct anon_obj {
    const char *name;
};

#endif /* _ANON_H_ */
