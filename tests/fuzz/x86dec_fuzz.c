/**
 * Fuzz harness for the decoder of the guest instructions the kernel
 * carries out (sys/kvm/decode.c): the input's first 15 bytes are decoded
 * in 16, 32 and 64-bit mode, and each decode that succeeds is checked
 * against the rules of decode.h; then the input's internal error is
 * checked against the layout of Linux's emulation_failure.
 *
 * fuzz-deps:
 */
#include "kvm/decode.c"
#include "../fuzz/fuzz.h"

extern long write(int fd, const void *buf, unsigned long n);
extern void abort(void);

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

/** Whether the instruction is an encoding the processor refuses that was
 * decoded no further than its opcode. */
static int bare_ud(const struct kvm_insn *d)
{
    return (d->flags & KVM_INSN_UD) && d->size == 0;
}

/** Check the operand and address sizes of a decode in `mode`. */
static void check_sizes(int mode, const struct kvm_insn *d)
{
    int wide =
        d->op == KVM_OP_CMPXCHG8B || d->dst.kind == KVM_OPND_MM || d->src.kind == KVM_OPND_MM;

    if (!bare_ud(d) && !pow2_size(d->size) && d->size != 16) {
        reject("operand size");
    }
    if (d->addr_size != 2 && d->addr_size != 4 && d->addr_size != 8) {
        reject("address size");
    }
    if ((mode != 64 && ((d->size == 8 && !wide) || d->addr_size == 8)) ||
        (mode == 64 && d->addr_size == 2)) {
        reject("size the mode does not have");
    }
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
    if (d->op < KVM_OP_MOV || d->op > KVM_OP_LAST) {
        reject("unknown op");
    }
    if ((d->op == KVM_OP_ALU || d->op == KVM_OP_UNARY) && d->sub > KVM_ALU_CMPR) {
        reject("unknown alu");
    }
    if ((d->flags & ~KVM_INSN_ALL) ||
        ((d->flags & KVM_INSN_MOV) && (d->flags & KVM_INSN_NOWRITE)) ||
        ((d->flags & KVM_INSN_REP) && (d->flags & KVM_INSN_REPNE))) {
        reject("flags");
    }
    check_sizes(mode, d);
}

/** Check one operand of a decode in `mode`: its kind, register and size. */
static void check_operand(int mode, const struct kvm_insn *d, const struct kvm_operand *o)
{
    uint32_t top = mode == 64 ? 15 : 7;

    switch (o->kind) {
    case KVM_OPND_NONE:
    case KVM_OPND_IMM:
        break;
    case KVM_OPND_REG:
        if (o->reg > top || !pow2_size(o->size) || (o->high8 && (o->reg > 3 || o->size != 1))) {
            reject("register");
        }
        break;
    case KVM_OPND_MEM:
        if (o->mem > KVM_MEM_XLAT ||
            (!pow2_size(o->size) && o->size != 16 && o->size != 6 && o->size != 10)) {
            reject("memory operand");
        }
        break;
    case KVM_OPND_SREG:
        if (o->reg > KVM_SEG_GS && !(d->flags & KVM_INSN_UD)) {
            reject("segment register");
        }
        break;
    case KVM_OPND_XMM:
        if (o->reg > top || o->size != 16) {
            reject("SSE register");
        }
        break;
    case KVM_OPND_MM:
        if (o->reg > 7 || o->size != 8) {
            reject("MMX register");
        }
        break;
    default:
        reject("operand kind");
    }
    if (o->kind != KVM_OPND_REG && o->high8) {
        reject("high byte of no register");
    }
}

/** Check the segment and address registers of a decode in `mode`. */
static void check_regs(int mode, const struct kvm_insn *d)
{
    if (d->seg > KVM_SEG_GS) {
        reject("segment");
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

/** Whether `o` is memory at the ModRM byte's or a moffs's address. */
static int modrm_mem(const struct kvm_operand *o)
{
    return o->kind == KVM_OPND_MEM && o->mem == KVM_MEM_MODRM;
}

/** Check one successful decode of `n` bytes in `mode` against decode.h,
 * and that the instruction's bytes alone decode the same and one byte
 * fewer do not. */
static void check(const uint8_t *in, uint32_t n, int mode, const struct kvm_insn *d)
{
    struct kvm_insn again;

    check_shape(in, n, mode, d);
    check_regs(mode, d);
    check_operand(mode, d, &d->dst);
    check_operand(mode, d, &d->src);
    check_operand(mode, d, &d->src2);
    if (modrm_mem(&d->dst) + modrm_mem(&d->src) + modrm_mem(&d->src2) > 1) {
        reject("two memory operands at one address");
    }
    if ((d->flags & KVM_INSN_STRING) &&
        (d->base != KVM_REG_NONE || d->index != KVM_REG_NONE || d->disp != 0)) {
        reject("string operation with an address");
    }
    if ((d->flags & KVM_INSN_LOCK) && !(d->flags & KVM_INSN_UD) && !modrm_mem(&d->dst)) {
        reject("LOCK taken with no memory destination");
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
