/* Check malformed, missing and inconsistent kernel measurement records.
 * host-deps: tests/system/checkers/records.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../system/checkers/records.h"

static int failures;
static const char grow[] =
    "grow-witness: whole before disk-commands 0 disk-flushes 0\n"
    "growroot: measure: table before monotonic-us 0 disk-commands 0 disk-flushes 0\n"
    "growroot: measure: table after monotonic-us 1 disk-commands 1 disk-flushes 0\n"
    "grow-witness: check-readonly before disk-commands 2 disk-flushes 1\n"
    "growroot: measure: check-readonly before monotonic-us 2 disk-commands 2 disk-flushes 1\n"
    "grow-witness: check-readonly after disk-commands 3 disk-flushes 1\n"
    "growroot: measure: check-readonly after monotonic-us 3 disk-commands 3 disk-flushes 1\n"
    "grow-witness: check-repair before disk-commands 4 disk-flushes 1\n"
    "growroot: measure: check-repair before monotonic-us 4 disk-commands 4 disk-flushes 1\n"
    "grow-witness: check-repair after disk-commands 5 disk-flushes 2\n"
    "growroot: measure: check-repair after monotonic-us 5 disk-commands 5 disk-flushes 2\n"
    "grow-witness: resize before disk-commands 6 disk-flushes 2\n"
    "growroot: measure: resize before monotonic-us 6 disk-commands 6 disk-flushes 2\n"
    "grow-witness: resize after disk-commands 7 disk-flushes 3\n"
    "growroot: measure: resize after monotonic-us 7 disk-commands 7 disk-flushes 3\n"
    "grow-witness: whole after disk-commands 7 disk-flushes 3\n";

static void check(int held, const char *name)
{
    if (!held) {
        fprintf(stderr, "records: %s\n", name);
        failures++;
    }
}

static void changed(const char *from, const char *to, const char *name)
{
    const char *at = strstr(grow, from);
    size_t before, size;
    char *text;
    if (!at) {
        check(0, "test input not found");
        return;
    }
    before = (size_t)(at - grow);
    size = sizeof(grow) - strlen(from) + strlen(to);
    text = malloc(size);
    if (!text) {
        check(0, "allocation");
        return;
    }
    memcpy(text, grow, before);
    strcpy(text + before, to);
    strcpy(text + before + strlen(to), at + strlen(from));
    check(!check_grow_records(text, NULL), name);
    free(text);
}

int main(void)
{
    check(check_grow_records(grow, NULL), "complete matching measurements");
    check(!check_grow_records("", NULL), "no measurements");
    changed("grow-witness: whole before disk-commands 0 disk-flushes 0\n", "", "missing witness");
    changed("whole after disk-commands 7", "whole after disk-commands 8", "witness differs");
    changed("resize before", "resize after", "duplicate witness");
    changed("table after monotonic-us 1", "resize after monotonic-us 1", "phase order");
    changed("table after monotonic-us 1 disk-commands 1",
            "table after monotonic-us 1 disk-commands 0", "no completed disk work");
    changed("monotonic-us 7", "monotonic-us 5", "clock moved backward");
    changed("monotonic-us 7", "monotonic-us 18446744073709551616", "counter overflow");
    changed("monotonic-us 7", "monotonic-us -7", "signed counter");
    changed("monotonic-us 7", "monotonic-us 7 extra extra extra extra extra extra extra extra",
            "too many fields");
    if (!failures)
        puts("records_hosttest: pass");
    return failures != 0;
}
