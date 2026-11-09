/* Boot the GNU root on the fake i915 chip and check what libdrm, Mesa and
 * the chip's fault file report. */
#include <stdio.h>
#include <string.h>

#include "netcase.h"

/*
 * The guest's script: drmdevice's report, the lines of Mesa's loader that
 * eglinfo logs, then first light run from the fake's file, plain and under
 * each fault, one `step` line each. The hang goes last, since nothing in the
 * file takes a hung ring back.
 */
static const char script[] = "drmdevice 2>&1\n"
                             "echo '--- loader'\n"
                             "EGL_LOG_LEVEL=debug LIBGL_DEBUG=verbose eglinfo 2>&1 >/dev/null |"
                             " grep -E 'pci id for fd|MESA-LOADER'\n"
                             "echo '--- fake'\n"
                             "f=$(ls -d /sys/kernel/debug/dri/*/i915_fake | head -n 1)\n"
                             "echo \"file $f\"\n"
                             "step() { for w in \"$@\"; do echo \"$w\" > \"$f\"; done;"
                             " echo \"step $*: $(grep '^light ' \"$f\")\"; }\n"
                             "step light; step submit light; step bind light; step irq light\n"
                             "step light; step hang light\n"
                             "cat \"$f\"\n";

/** Say whether `out` has drmdevice's PCI line `key` with value `value`. */
static int drm_field(const char *out, const char *key, const char *value)
{
    const char *p = out, *v;
    size_t n = strlen(key);

    while ((p = strstr(p, key)) != NULL) {
        p += n;
        for (v = p; *v == ' ' || *v == '\t'; v++)
            ;
        if (v != p && strncmp(v, value, strlen(value)) == 0)
            return 1;
    }
    return 0;
}

/** Boot with rach2.i915fake and judge the identity, the loader and faults. */
int etude_scenario(struct etude_case *c)
{
    struct net_case u;
    struct net_boot b;
    struct net_run r;
    char password[128];
    const char *argv[] = {"/bin/bash", "-c", script, NULL};
    const char *light;

    net_case(&u, c);
    memset(&b, 0, sizeof(b));
    b.u = &u;
    net_secret(&u, "demo-password", password, sizeof(password));
    if (net_step(&u, "boot") == ETUDE_OK)
        net_boot(&b, &u, "gnu", 1, NULL);
    net_seen(&b, "edu stands in for 8086:0a16");
    net_seen(&b, "ext2: root: /dev/disk0p1");
    net_seen(&b, "rach2 login");
    if (net_login(&b, "root", password, "# ") == ETUDE_OK && etude_guest_open(b.m) != ETUDE_OK)
        net_fail(&u, "the GNU guest command channel did not open");
    if (net_step(&u, "probe") != ETUDE_OK ||
        net_guest(&u, b.m, argv, NULL, NULL, "i915fake", &r) != ETUDE_OK) {
        net_assert(&u, "identity", 0, "the guest script could not run");
        return net_end(&u);
    }
    fputs(r.out, stdout);
    fputs(r.err, stderr);
    net_assert(&u, "identity",
               drm_field(r.out, "vendor_id", "8086") && drm_field(r.out, "device_id", "0a16") &&
                   drm_field(r.out, "subvendor_id", "17aa") &&
                   drm_field(r.out, "subdevice_id", "380c"),
               "drmdevice did not report 8086:0a16 on board 17aa:380c");
    net_assert(&u, "loader", net_find(r.out, ": 8086:0a16, driver crocus") != NULL,
               "Mesa's loader did not pick crocus for 8086:0a16");
    light = net_find(r.out, "step light: light ok");
    net_assert(&u, "faults",
               net_exited0(&r) && light != NULL &&
                   net_find(r.out, "step submit light: light error -5") != NULL &&
                   net_find(r.out, "step bind light: light error -12") != NULL &&
                   net_find(r.out, "step irq light: light error -62") != NULL &&
                   net_find(light + 1, "step light: light ok") != NULL &&
                   net_find(r.out, "step hang light: light error -62") != NULL &&
                   net_find(r.out, "ring rcs0 commands # batches # hung 1") != NULL,
               "first light from the fake's file did not fail by each fault and recover");
    net_run_free(&r);
    return net_end(&u);
}
