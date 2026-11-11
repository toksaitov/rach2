/*
 * xsaves.c: whether the kernel and KVM's guests may use XSAVES, by `cpuid`
 * and AMD's Erratum 1386, as kcpu.h describes it.
 */

#include "kcpu.h"

/* Erratum 1386's fixed parts, Linux 6.12's erratum_1386_microcode: family
 * 0x17 model, stepping and the least microcode revision with the fix. */
static const struct {
    uint8_t model;
    uint8_t stepping;
    uint32_t revision;
} erratum_1386_fixed[] = {{0x01, 0x2, 0x0800126Eu}, {0x31, 0x0, 0x08301052u}};

int kcpu_xsaves_usable(void)
{
    uint32_t max = 0, b = 0, c = 0, d = 0, sig = 0, xs = 0, revision = 0;
    uint32_t family, model, stepping;
    unsigned int i;

    kcpu_id(0, &max, &b, &c, &d);
    if (max < 0xD) {
        return 0;
    }
    kcpu_id_sub(0xD, 1, &xs, 0, 0, 0);
    if (!(xs & (1u << 3))) {
        return 0;
    }
    /* "AuthenticAMD" in EBX, EDX and ECX */
    if (b != 0x68747541u || d != 0x69746E65u || c != 0x444D4163u) {
        return 1;
    }
    kcpu_id(1, &sig, 0, 0, 0);
    family = (sig >> 8) & 0xFu;
    model = (sig >> 4) & 0xFu;
    stepping = sig & 0xFu;
    if (family == 0xFu) {
        family += (sig >> 20) & 0xFFu;
        model |= ((sig >> 16) & 0xFu) << 4;
    }
    /* Linux's Zen 1 and Zen 2 models; another family 0x17 model is neither */
    if (family != 0x17u || !(model <= 0x7Fu || model == 0x90u || model == 0x91u ||
                             (model >= 0xA0u && model <= 0xAFu))) {
        return 1;
    }
    kcpu_rdmsr(0x8Bu, &revision, 0); /* MSR_AMD64_PATCH_LEVEL */
    for (i = 0; i < sizeof(erratum_1386_fixed) / sizeof(erratum_1386_fixed[0]); i++) {
        if (model == erratum_1386_fixed[i].model && stepping == erratum_1386_fixed[i].stepping &&
            revision >= erratum_1386_fixed[i].revision) {
            return 1;
        }
    }
    return 0;
}
