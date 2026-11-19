/*
 * pelt.c: per-entity load tracking for the fair class, Linux 6.12's PELT, per
 * entity, per queue and per group, with blocked averages decaying.
 */

#include "fair.h"
