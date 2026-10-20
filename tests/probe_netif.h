/*
 * probe_netif.h: the network interface a freestanding probe works on, eth0
 * under QEMU or the machine's own in a native run (tests/native/Readme.md).
 *
 * It needs probe.h and probe_args.h, included before it, and no C library.
 */
#ifndef PROBE_NETIF_H
#define PROBE_NETIF_H

/**
 * The interface's name: the argument `ifname=NAME`, which the `network` peer
 * gives; else, given `machine=native`, the interface of the machine's
 * default route, the line of /proc/net/route whose destination is 00000000;
 * else eth0, as on the test image or on a machine with no default route.
 * The name is in a buffer of the header's own.
 */
static __inline__ const char *probe_ifname(void)
{
    static char text[1024];
    static char name[16];
    const char *arg = probe_arg("ifname", 0);
    int fd, n, i, k, j;

    if (arg != 0 || !probe_native()) {
        return arg != 0 ? arg : "eth0";
    }
    fd = syscall3(SYS_OPEN, (long)"/proc/net/route", 0, 0);
    n = fd < 0 ? -1 : syscall3(SYS_READ, fd, (long)text, (int)sizeof(text) - 1);
    if (fd >= 0) {
        syscall1(SYS_CLOSE, fd);
    }
    for (i = 0; i < n; i = k + 1) {
        for (k = i; k < n && text[k] != '\t' && text[k] != '\n'; k++) {
        }
        for (j = 0; j < 9 && k + 1 + j < n && text[k + 1 + j] == "00000000\t"[j]; j++) {
        }
        if (j == 9 && text[k] == '\t' && k - i < (int)sizeof(name)) {
            for (j = 0; j < k - i; j++) {
                name[j] = text[i + j];
            }
            name[j] = '\0';
            return name;
        }
        while (k < n && text[k] != '\n') {
            k++;
        }
    }
    return "eth0";
}

#endif
