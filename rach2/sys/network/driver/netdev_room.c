/*
 * netdev_room.c: the arrays of an interface's addresses and IPv6 groups,
 * which grow as addresses are added and groups joined and are freed with the
 * last, as driver/netdev.h declares them.
 */

#include "netdev.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"

int netdev_addr4_room(struct netdev *nd, int n)
{
    struct addr4_routes *block;
    int room;

    if (n == 0 && nd->naddr2 == 0) {
        kfree(nd->addr4_more);
        nd->addr4_more = 0;
        nd->ip2 = nd->mask2 = nd->peer2 = 0;
        nd->room2 = 0;
        return 0;
    }
    if (n <= nd->room2)
        return 0;
    room = nd->room2 ? nd->room2 : 4;
    while (room < n) {
        if (room > 0x7fffffff / 2 / (int)(sizeof(*block) + 3 * sizeof(uint32_t)))
            return -ENOBUFS;
        room *= 2;
    }
    /* One block: the attributes, whose 64-bit members want the alignment
     * kmalloc gives, then the three arrays of words. */
    block = kmalloc((size_t)room * (sizeof(*block) + 3 * sizeof(uint32_t)));
    if (!block)
        return -ENOBUFS;
    if (nd->naddr2) {
        uint32_t *words = (uint32_t *)(void *)(block + room);
        k_memcpy(block, nd->addr4_more, (size_t)nd->naddr2 * sizeof(*block));
        k_memcpy(words, nd->ip2, (size_t)nd->naddr2 * sizeof(uint32_t));
        k_memcpy(words + room, nd->mask2, (size_t)nd->naddr2 * sizeof(uint32_t));
        k_memcpy(words + 2 * room, nd->peer2, (size_t)nd->naddr2 * sizeof(uint32_t));
    }
    kfree(nd->addr4_more);
    nd->addr4_more = block;
    nd->ip2 = (uint32_t *)(void *)(block + room);
    nd->mask2 = nd->ip2 + room;
    nd->peer2 = nd->mask2 + room;
    nd->room2 = room;
    return 0;
}

int netdev_addr6_room(struct netdev *nd, int n)
{
    struct netdev_addr6 *more;
    int room;

    if (n == 0 && nd->naddr6 == 0) {
        kfree(nd->addr6);
        nd->addr6 = 0;
        nd->room6 = 0;
        return 0;
    }
    if (n <= nd->room6)
        return 0;
    room = nd->room6 ? nd->room6 : 4;
    while (room < n) {
        if (room > 0x7fffffff / 2 / (int)sizeof(*more))
            return -ENOBUFS;
        room *= 2;
    }
    more = kmalloc((size_t)room * sizeof(*more));
    if (!more)
        return -ENOBUFS;
    if (nd->naddr6)
        k_memcpy(more, nd->addr6, (size_t)nd->naddr6 * sizeof(*more));
    kfree(nd->addr6);
    nd->addr6 = more;
    nd->room6 = room;
    return 0;
}

/** The slot of `group` among `nd`'s groups, or -1. */
static int group_slot(const struct netdev *nd, const uint8_t *group)
{
    int i, j;

    for (i = 0; i < nd->ngroup6; i++) {
        for (j = 0; j < 16; j++)
            if (nd->group6[i].addr[j] != group[j])
                break;
        if (j == 16)
            return i;
    }
    return -1;
}

int netdev_join6(struct netdev *nd, const uint8_t *group)
{
    int i;

    if (!nd || group[0] != 0xff) {
        return -EINVAL;
    }
    i = group_slot(nd, group);
    if (i >= 0) {
        nd->group6[i].refs++;
        return 0;
    }
    if (nd->ngroup6 == nd->room_group6) {
        struct netdev_group6 *more;
        int room = nd->room_group6 ? nd->room_group6 * 2 : 8;

        if (nd->room_group6 > 0x7fffffff / 2 / (int)sizeof(*more))
            return -ENOBUFS;
        more = kmalloc((size_t)room * sizeof(*more));
        if (!more)
            return -ENOBUFS;
        if (nd->ngroup6)
            k_memcpy(more, nd->group6, (size_t)nd->ngroup6 * sizeof(*more));
        kfree(nd->group6);
        nd->group6 = more;
        nd->room_group6 = room;
    }
    k_memcpy(nd->group6[nd->ngroup6].addr, group, 16);
    nd->group6[nd->ngroup6].refs = 1;
    nd->ngroup6++;
    return 0;
}

/** The last slot moves into the one freed, since the order of the groups
 * means nothing; the array is freed with the last. */
void netdev_leave6(struct netdev *nd, const uint8_t *group)
{
    int i = nd ? group_slot(nd, group) : -1;

    if (i < 0 || --nd->group6[i].refs != 0) {
        return;
    }
    nd->ngroup6--;
    if (i != nd->ngroup6) {
        nd->group6[i] = nd->group6[nd->ngroup6];
    }
    if (nd->ngroup6 == 0) {
        kfree(nd->group6);
        nd->group6 = 0;
        nd->room_group6 = 0;
    }
}

int netdev_member6(const struct netdev *nd, const uint8_t *group)
{
    return nd && group_slot(nd, group) >= 0;
}

void netdev_addr_free(struct netdev *nd)
{
    kfree(nd->addr4_more);
    kfree(nd->addr6);
    kfree(nd->group6);
    nd->addr4_more = 0;
    nd->ip2 = nd->mask2 = nd->peer2 = 0;
    nd->addr6 = 0;
    nd->group6 = 0;
    nd->naddr2 = nd->room2 = nd->naddr6 = nd->room6 = nd->ngroup6 = nd->room_group6 = 0;
}
