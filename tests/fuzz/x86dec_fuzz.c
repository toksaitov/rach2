/**
 * Fuzz harness for the decoder of guest instructions that touch MMIO
 * (sys/kvm/decode.c): the input's first 15 bytes are decoded in 16, 32 and
 * 64-bit mode, and each decode that succeeds is checked against the rules
 * of decode.h; then the input's internal error is checked against the
 * layout of Linux's emulation_failure.
 *
 * fuzz-deps:
 */
#include "kvm/decode.c"
#include "../fuzz/fuzz.h"

extern long write(int fd, const void *buf, unsigned long n);
extern void abort(void);

/* The flags decode.h defines. */
#define ALL_FLAGS 0x3FFu

/** Report that the decoder broke one of its rules, and abort. */
static void reject(const char *rule)
{
    static const char head[] = "x86dec-fuzz: decoder fault: ";
    unsigned long n = 0;

    while (rule[n] != '\0') {
        n++;
    }
    (void)write(2, head, sizeof(head) - 1);
    (void)write(2, rule, n);
    (void)write(2, "\n", 1);
    abort();
}

/** Whether `v` is 1, 2, 4 or 8. */
static int pow2_size(uint32_t v)
{
    return v == 1 || v == 2 || v == 4 || v == 8;
}

/** Whether two decodes agree in every member. */
static int same(const struct kvm_insn *a, const struct kvm_insn *b)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    unsigned long i;

    for (i = 0; i < sizeof(*a); i++) {
        if (x[i] != y[i]) {
            return 0;
        }
    }
    return 1;
}

/** Check the length, bytes, operation, flags and sizes of a decode of
 * `n` bytes in `mode`. */
static void check_shape(const uint8_t *in, uint32_t n, int mode, const struct kvm_insn *d)
{
    uint32_t i;

    if (d->len < 1 || d->len > n || d->len > KVM_INSN_MAX) {
        reject("length outside the bytes");
    }
    for (i = 0; i < d->len; i++) {
        if (d->bytes[i] != in[i]) {
            reject("bytes not copied");
        }
    }
    if (d->op < KVM_OP_LOAD || d->op > KVM_OP_POP) {
        reject("unknown op");
    }
    if ((d->op == KVM_OP_ALU || d->op == KVM_OP_UNARY) && d->alu > KVM_ALU_NEG) {
        reject("unknown alu");
    }
    if ((d->flags & ~ALL_FLAGS) || !(d->flags & (KVM_INSN_READ | KVM_INSN_WRITE))) {
        reject("flags");
    }
    if (!pow2_size(d->size) || !pow2_size(d->reg_size) || d->reg_size < d->size) {
        reject("operand size");
    }
    if (d->addr_size != 2 && d->addr_size != 4 && d->addr_size != 8) {
        reject("address size");
    }
    if ((mode != 64 && (d->size == 8 || d->addr_size == 8)) || (mode == 64 && d->addr_size == 2)) {
        reject("size the mode does not have");
    }
}

/** Check the segment and registers of a decode in `mode`. */
static void check_regs(int mode, const struct kvm_insn *d)
{
    if (d->seg > KVM_SEG_GS) {
        reject("segment");
    }
    if (d->reg < KVM_REG_NONE || d->reg > 15 || (mode != 64 && d->reg > 7)) {
        reject("register");
    }
    if ((d->flags & KVM_INSN_HIGH8) && (d->reg < 0 || d->reg > 3 || d->size != 1)) {
        reject("high byte register");
    }
    if (d->base < KVM_REG_NONE || d->base > KVM_REG_RIP || d->index < KVM_REG_NONE ||
        d->index > 15 || d->index == 4) {
        reject("address registers");
    }
    if (!pow2_size(d->scale) || (d->index == KVM_REG_NONE && d->scale != 1)) {
        reject("scale");
    }
    if (d->base == KVM_REG_RIP && (mode != 64 || d->index != KVM_REG_NONE)) {
        reject("RIP-relative outside 64-bit mode or with an index");
    }
}

/** Check one successful decode of `n` bytes in `mode` against decode.h,
 * and that the instruction's bytes alone decode the same and one byte
 * fewer do not. */
static void check(const uint8_t *in, uint32_t n, int mode, const struct kvm_insn *d)
{
    struct kvm_insn again;

    check_shape(in, n, mode, d);
    check_regs(mode, d);
    if ((d->op == KVM_OP_MOVS || d->op == KVM_OP_STOS) &&
        (d->base != KVM_REG_NONE || d->index != KVM_REG_NONE || d->disp != 0)) {
        reject("string operation with an address");
    }
    if (kvm_decode(in, d->len, mode, &again) != 0 || !same(d, &again)) {
        reject("cut to its length, decodes otherwise");
    }
    if (kvm_decode(in, d->len - 1, mode, &again) != -1) {
        reject("decodes one byte short");
    }
}

/** Decode the input in each mode, check what decodes, and check the
 * internal error that reports it. */
int LLVMFuzzerTestOneInput(const unsigned char *data, unsigned long size)
{
    static const int modes[3] = {16, 32, 64};
    struct kvm_insn d;
    struct kvm_run_internal e;
    uint32_t n = size < 32 ? (uint32_t)size : 32;
    uint32_t i;
    const uint8_t *raw;

    for (i = 0; i < 3; i++) {
        if (kvm_decode(data, n, modes[i], &d) == 0) {
            check(data, n, modes[i], &d);
        }
    }
    kvm_decode_fail(&e, data, n, 0);
    raw = (const uint8_t *)&e.data[1];
    if (e.suberror != KVM_INTERNAL_ERROR_EMULATION) {
        reject("suberror");
    }
    if (n == 0 ? e.ndata != 6 || e.data[0] != 0
               : e.ndata != 8 || raw[0] != (n < 15 ? n : 15) || e.data[0] != 1) {
        reject("emulation_failure layout");
    }
    return 0;
}
