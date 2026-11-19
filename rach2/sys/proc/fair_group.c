/*
 * fair_group.c: group scheduling for the fair class, a group's queue and
 * entity on each processor and the group entity's weight from its share
 * (Linux 6.12's calc_group_shares and update_cfs_group).
 */

#include "fair.h"
