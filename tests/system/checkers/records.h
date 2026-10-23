/* C judges for the disk-grow and USB records described in records.c. */
#ifndef TEST_RECORDS_H
#define TEST_RECORDS_H
#include <stdio.h>
int check_grow_records(const char *text, FILE *evidence);
int check_usb_records(const char *text, int random, unsigned keys);
#endif
