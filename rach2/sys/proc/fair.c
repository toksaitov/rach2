/*
 * fair.c: Linux 6.12's fair scheduling class: virtual runtime, the EEVDF
 * pick of the earliest eligible virtual deadline, lag kept across sleeps,
 * nice weights and the group hierarchy.
 */

#include "fair.h"
