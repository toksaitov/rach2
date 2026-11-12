/* Idle deadlines of services otherwise driven by the boot processor's tick. */
#ifndef _IDLE_TIMER_H_
#define _IDLE_TIMER_H_
#include "timer.h"

/* Lower a wait in ticks to a 32-bit deadline, with wrap handled as its owner
 * handles it. An event already due is served on the next count. */
#define IDLE_TIMER_MIN(wait, due, now)                                    \
    do {                                                                  \
        int32_t idle_left = (int32_t)((uint32_t)(due) - (uint32_t)(now)); \
        uint32_t idle_wait = idle_left <= 0 ? 1u : (uint32_t)idle_left;   \
        if (idle_wait < (wait)) {                                         \
            (wait) = idle_wait;                                           \
        }                                                                 \
    } while (0)

/* Each query returns the ticks until its next service, from 1 to TICK_HZ.
 * TICK_HZ is also the clock maintenance bound when the service has no work.
 * The caller holds the big lock with interrupts off; a service whose state
 * changes outside it takes its own lock. Queries leave that state alone. */
uint32_t netdev_idle_ticks(void);
uint32_t ath9k_idle_ticks(void);
uint32_t netconsole_idle_ticks(void);
uint32_t ip_frag_idle_ticks(void);
uint32_t frag6_idle_ticks(void);
uint32_t nd6_idle_ticks(void);
uint32_t netlink_idle_ticks(void);
uint32_t usb_idle_ticks(void);
uint32_t input_idle_ticks(void);
uint32_t ps2kbd_idle_ticks(void);
uint32_t alps_idle_ticks(void);
uint32_t itco_idle_ticks(void);
uint32_t iommu_idle_ticks(void);
uint32_t ata_idle_ticks(void);
uint32_t block_request_idle_ticks(void);
uint32_t block_lag_idle_ticks(void);
uint32_t speaker_idle_ticks(void);
uint32_t hda_idle_ticks(void);
uint32_t blank_idle_ticks(void);
uint32_t vkms_idle_ticks(void);
uint32_t tcp_idle_ticks(void);
uint32_t tty_idle_ticks(void);
uint32_t sched_load_idle_ticks(void);
uint32_t timerfd_idle_ticks(void);
#endif
