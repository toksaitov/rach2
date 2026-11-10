/* Boot the GNU root on the fake i915 chip and check what libdrm, Mesa and
 * the chip's fault file report. */
#include <stdio.h>
#include <string.h>

#include "netcase.h"

/*
 * The guest's script: drmdevice's report, the lines of Mesa's loader that
 * eglinfo logs on the surfaceless platform, modetest's connectors and
 * planes of the card, then first light run from the fake's file, plain and
 * under each fault, one `step` line each. Under
 * `--- eglinfo` come its exit status and the last of its lines that are not
 * the loader's debug log or an indented list, which show how far crocus
 * got. Its exit is not
 * judged while the driver serves no EXECBUFFER2. The hang goes last, since
 * nothing in the file takes a hung ring back.
 */
static const char script[] =
    "drmdevice 2>&1\n"
    "echo '--- loader'\n"
    "EGL_LOG_LEVEL=debug eglinfo -p surfaceless >/tmp/eglinfo.out 2>&1\n"
    "e=$?\n"
    "grep -E 'pci id for fd|using driver|EGL driver name' /tmp/eglinfo.out\n"
    "echo '--- eglinfo'\n"
    "echo \"eglinfo exit $e\"\n"
    "grep -v -e 'libEGL debug' -e '^    ' /tmp/eglinfo.out | tail -n 30\n"
    "rm -f /tmp/eglinfo.out\n"
    "echo '--- kms'\n"
    "modetest -M i915 -c 2>&1 | grep -E 'connected'\n"
    "modetest -M i915 -p 2>&1 | grep -E 'formats:'\n"
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

/** Say whether modetest's connectors and planes in `out` are the display
 * the driver makes on a chip whose firmware lit nothing: eDP-1
 * disconnected, and on each of the three pipes a primary plane, a sprite
 * and a cursor with Linux's Haswell formats. */
static int kms_listed(const char *out)
{
    return net_find(out, "disconnected*eDP-1") != NULL &&
           net_count(out, "formats: C8 * RG16 XR24 XB24 XR30 XB30 XB4H") == 3 &&
           net_count(out, "formats: XR24 XB24 XR30 XB30 XR4H XB4H YUYV YVYU UYVY VYUY") == 3 &&
           net_count(out, "formats: AR24") == 3;
}

/** Boot with rach2.i915fake and judge the identity, the loader, the
 * display and faults. */
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
    net_assert(&u, "loader",
               net_find(r.out, "pci id for fd #: 8086:0a16, driver crocus") != NULL &&
                   net_find(r.out, "EGL driver name: crocus") != NULL,
               "Mesa's loader did not pick crocus for 8086:0a16");
    net_assert(&u, "kms", kms_listed(r.out),
               "modetest did not list eDP-1 disconnected and the planes' Haswell formats");
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
