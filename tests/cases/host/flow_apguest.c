/* flow_apguest.c: apguest.sh -m check judged by the guest's last record. */
#define _POSIX_C_SOURCE 200809L
#include "flow_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Stand for QEMU: print FLOW_APGUEST_SAY as the guest's console and end with
 * FLOW_APGUEST_STATUS, after checking that the console is QEMU's stdio.
 *
 * Return: the status asked for, or 90 when the arguments are not a check
 * boot's.
 */
int flow_apguest_mock(int argc, char **argv)
{
    const char *say = getenv("FLOW_APGUEST_SAY"), *status = getenv("FLOW_APGUEST_STATUS");
    int i, stdio = 0, check = 0;
    for (i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "-serial") && !strcmp(argv[i + 1], "stdio"))
            stdio = 1;
        if (!strcmp(argv[i], "-append") && strstr(argv[i + 1], "wifiap.run=check"))
            check = 1;
    }
    if (!stdio || !check)
        return 90;
    fputs(say ? say : "", stdout);
    return status ? atoi(status) : 0;
}

/**
 * Run apguest.sh -m check against one console and QEMU status, and check
 * the script's status and its result line.
 */
static int row(struct fixture *f, const char *label, const char *say, const char *status, int want,
               const char *line)
{
    char guest[4200], qemu[4200], env_say[4200], env_status[64];
    const char *env[] = {guest, qemu, env_say, env_status, "FLOW_APGUEST=1", NULL};
    const char *a[] = {
        "/bin/sh", flow_source(f, "tests/system/wifi/apguest.sh"), "-a", "tcg", "-m", "check",
        NULL};
    int ok;
    snprintf(guest, sizeof(guest), "RACH2_WIFI_GUEST=%s", flow_path(f, "guest"));
    snprintf(qemu, sizeof(qemu), "RACH2_QEMU=%s/qemu-system-x86_64", f->bin);
    snprintf(env_say, sizeof(env_say), "FLOW_APGUEST_SAY=%s", say);
    snprintf(env_status, sizeof(env_status), "FLOW_APGUEST_STATUS=%s", status);
    fx_run(f, a, f->work, env, NULL);
    ok = fx_check(f, f->status == want, label);
    ok = fx_check(f, (f->out && strstr(f->out, line)) || (f->err && strstr(f->err, line)), label) &&
         ok;
    return ok;
}

/**
 * Check that the script passes a ready guest and fails a failed check, a
 * console with no result and a QEMU that failed, and that it leaves no
 * scratch behind.
 */
int flow_apguest(struct fixture *f)
{
    int ok;
    if (!fx_mkdir(flow_path(f, "guest")) || !flow_put(f, "guest/vmlinuz", "") ||
        !flow_put(f, "guest/initrd", "") || !fx_mock(f, "qemu-system-x86_64"))
        return 0;
    ok = row(f, "ready",
             "wifiap: beacon: 02:00:00:00:00:00 rach2-net\r\nwifiap: check: ok\r\n"
             "wifiap: ready: wpa2 check, at 3.2 s\r\n",
             "0", 0, "apguest: result: ok: wpa2 check, at 3.2 s\n");
    ok = row(f, "failed check",
             "wifiap: check: failed: not heard: rach2-other\r\n"
             "wifiap: failed: check\r\n",
             "0", 1, "apguest: result: failed: the guest's check failed\n") &&
         ok;
    ok = row(f, "no result", "Kernel panic - not syncing: Attempted to kill init!\r\n", "0", 1,
             "apguest: result: failed: the guest gave no result\n") &&
         ok;
    ok = row(f, "qemu failed", "wifiap: ready: wpa2 check, at 3.2 s\r\n", "1", 1,
             "apguest: the guest's QEMU failed: status 1\n") &&
         ok;
    ok = fx_check(f, fx_empty(flow_path(f, "tmp")), "scratch removed") && ok;
    return ok;
}
