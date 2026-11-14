/*
 * vblk_scenario.c: the scenario of guest.vblk.restart, in which the
 * kernel's debug shell restarts a machine that has a virtio-blk disk. The
 * driver must say that it reset the device with nothing in flight, and say
 * it before the kernel says that it restarts.
 */
#include <stdint.h>

#include "lifecycle.h"

int etude_scenario(struct etude_case *c)
{
    static const char *const ids[] = {"brought-up", "halted",    "then-restart",
                                      "reset",      "none-lost", NULL};
    struct etude_case_machine *m;
    struct etude_termination end;
    uint64_t at = 0;
    struct lc l;

    lc_begin(&l, c, ids);
    if ((m = lc_shell(&l, "restart", "fixture", &at)) == NULL)
        return lc_finish(&l);
    lc_find(&l, m, "brought-up", " sectors a request, cache on", 0, 1);
    if (!lc_type(&l, m, "reboot\r") ||
        !lc_wait(&l, m, ": shutdown: reset with no request in flight", &at, "halted") ||
        !lc_wait(&l, m, "reboot: restarting", &at, "then-restart") || !lc_end(&l, m, &end))
        return lc_finish(&l);
    lc_assert(&l, "reset", end.end == ETUDE_END_RESET, "QEMU's event is not a reset");
    lc_find(&l, m, "none-lost", " requests unanswered", 0, 0);
    return lc_finish(&l);
}
