#ifndef _ANON_H_
#define _ANON_H_

/*
 * The descriptors that name no file: eventfd, epoll and timerfd. Each is a
 * descriptor of type FD_TYPE_ANON whose `priv` is an object that begins with
 * this header, whose name /proc/PID/fd reports as "anon_inode:NAME", as
 * Linux does.
 */
struct anon_obj {
    const char *name;
};

#endif /* _ANON_H_ */
