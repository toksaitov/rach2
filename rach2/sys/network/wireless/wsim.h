/* The software radio, wsim, and its device /dev/wsim, through which a
 * program carries the radio's frames to a medium and back. */
#ifndef _WLAN_WSIM_H_
#define _WLAN_WSIM_H_

#include "wireless.h"
#include "../../fs/fd.h"
#include "../../lib/kuser.h"

struct wait_queue;

#define WSIM_MAX_RADIOS 4  /* the registry's four interfaces */
#define WSIM_QUEUE      64 /* frames sent and not yet read from /dev/wsim */
#define WSIM_PENDING    64 /* frames sent and awaiting their status */

/* WSIM_IOC_INFO: _IOR('w', 1, struct wsim_info), the radio a descriptor of
 * /dev/wsim carries. */
#define WSIM_IOC_INFO 0x80107701u

/* The answer to WSIM_IOC_INFO, sixteen bytes in the host's byte order. */
struct wsim_info {
    uint16_t radio;     /* N of wsimN, the number in the device's records */
    uint16_t frequency; /* MHz the radio is tuned to; 0 before its first */
    uint8_t mac[6];
    uint8_t started;     /* 1 while the station layer runs the radio */
    uint8_t filters;     /* WLAN_FILTER_* of the last configuration */
    uint8_t reserved[4]; /* zero */
};

/* The station layer's calls wsim makes, which `wsim_attach` takes so that
 * wsim.c links before the registry and the data path do. Each has the
 * contract of the function of wireless.h or radio.h it names. */
struct wsim_station {
    int (*register_radio)(struct wlan_radio *radio, struct wlan **out); /* wlan_register_radio */
    void (*unregister_radio)(struct wlan *wlan);                        /* wlan_unregister_radio */
    int (*rx)(struct wlan_radio *radio, const uint8_t *frame, unsigned len,
              const struct wlan_rx_status *status); /* wlan_radio_rx */
    void (*tx_status)(struct wlan_radio *radio,
                      const struct wlan_tx_status *status); /* wlan_radio_tx_status */
};

/**
 * Take the station layer's calls and make the radios `rach2.wsim=N` asks
 * for, N from 1 to WSIM_MAX_RADIOS, as wsim0 to wsimN-1.
 *
 * Process context under the kernel lock, without net_lock; may sleep, since
 * registration may. Call once, after the registry can take radios; until
 * then an open of /dev/wsim fails with ENODEV. Returns the number of radios
 * made; a radio that fails to register is logged and left out.
 */
int wsim_attach(const struct wsim_station *station);

/**
 * Give the descriptor `e` a radio: the lowest-numbered one `rach2.wsim`
 * made that no descriptor carries, or else a new one, registered here.
 * Returns 0, -ENODEV before `wsim_attach`, -ENOSPC when WSIM_MAX_RADIOS
 * radios are carried, -ENOMEM, or the registry's error.
 */
int32_t wsim_open(fd_entry_t *e);

/** Read the oldest frame the radio sent as one record of the relay format,
 * the whole record or -EINVAL for a buffer too small for it. Blocks until
 * there is one unless O_NONBLOCK, which gives -EAGAIN. */
int32_t wsim_read(fd_entry_t *e, char __user *buf, uint32_t count);

/**
 * Take one record of the relay format: a frame the radio receives, or the
 * status of a frame it sent. Returns `count` for a frame the radio received
 * or a status taken, 0 for a frame it did not receive (stopped, tuned to
 * another channel, filtered, or its receive queue full), -EINVAL for a
 * record the format refuses, -ENOENT for a status of no frame awaiting
 * one, or -EFAULT.
 */
int32_t wsim_write(fd_entry_t *e, const char __user *buf, uint32_t count);

/** POLLIN while a frame waits to be read; POLLOUT always. */
short wsim_poll(fd_entry_t *e, short events, struct wait_queue **wq);

/** WSIM_IOC_INFO, or -ENOTTY for another request. */
int32_t wsim_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg);

/**
 * Close the radio's medium: frames unread and frames awaiting status
 * complete as failed, and a radio the open made is unregistered and freed.
 * A radio `rach2.wsim` made stays, refusing frames until a descriptor
 * carries it again. Process context; may sleep.
 */
void wsim_close(fd_entry_t *e);

#endif
