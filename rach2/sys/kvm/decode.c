/*
 * decode.c: the decoder of the guest instructions that touch MMIO, from
 * their bytes to what they do with memory, and the internal error that
 * reports one it does not know. decode.h has the contract.
 */

#include "decode.h"

/* The REX bits. */
#define REX_W 0x8
#define REX_R 0x4
#define REX_X 0x2
#define REX_B 0x1

/*
 * The decoder's state while it reads one instruction: the bytes it may
 * read, how many it has, and what the prefixes said.
 */
struct dec {
    const uint8_t *p;
    uint32_t n;  /* the bytes that may be read, at most KVM_INSN_MAX */
    uint32_t at; /* the bytes read */
    int mode;    /* 16, 32 or 64 */
    int rex;     /* the REX byte before the opcode, or 0 */
    int seg;     /* a segment override, or -1 */
    int rep;     /* 0, 0xF2 or 0xF3, the last of them */
    int lock;
    int opsize;   /* a 0x66 prefix */
    int lockable; /* the instruction takes a LOCK prefix */
    uint32_t osize;
    uint32_t asize;
};

/**
 * Read the next `k` bytes, 1 to 8, as a little-endian number into `v`:
 * 0, or -1 when the instruction would pass its bytes.
 */
static int fetch(struct dec *d, uint32_t k, uint64_t *v)
{
    uint32_t i;

    if (k > d->n - d->at) {
        return -1;
    }
    *v = 0;
    for (i = 0; i < k; i++) {
        *v |= (uint64_t)d->p[d->at + i] << (8 * i);
    }
    d->at += k;
    return 0;
}

/** Sign-extend the low `k` bytes of `v` to 64 bits. */
static uint64_t sext(uint64_t v, uint32_t k)
{
    uint32_t bits = 8 * k;

    if (bits >= 64) {
        return v;
    }
    v &= ((uint64_t)1 << bits) - 1;
    if (v & ((uint64_t)1 << (bits - 1))) {
        v |= ~(((uint64_t)1 << bits) - 1);
    }
    return v;
}

/**
 * Read the immediate of an operand of `size` bytes, which is 4 bytes for
 * an 8-byte operand, into `out->imm`, sign-extended: 0 or -1.
 */
static int imm(struct dec *d, uint32_t size, struct kvm_insn *out)
{
    uint32_t k = size == 8 ? 4 : size;
    uint64_t v;

    if (fetch(d, k, &v) != 0) {
        return -1;
    }
    out->imm = sext(v, k);
    out->flags |= KVM_INSN_IMM;
    return 0;
}

/** Name the register `r`, 0 to 15, an operand of `size` bytes, in `out`,
 * as AH, CH, DH or BH for a byte register 4 to 7 with no REX. */
static void set_reg(const struct dec *d, struct kvm_insn *out, int r, uint32_t size)
{
    if (size == 1 && d->rex == 0 && r >= 4 && r < 8) {
        out->reg = r - 4;
        out->flags |= KVM_INSN_HIGH8;
    } else {
        out->reg = r;
    }
}

/** The register of a ModRM's reg field `regf` with REX.R. */
static int reg_of(const struct dec *d, int regf)
{
    return regf | (d->rex & REX_R ? 8 : 0);
}

/**
 * Set the base and index of a 16-bit ModRM's address in `out`, from its
 * `mod` and `rm`: the displacement's bytes that follow.
 */
static uint32_t addr16(struct kvm_insn *out, int mod, int rm)
{
    /* The base and index registers of the 16-bit forms, by rm. */
    static const signed char base16[8] = {3, 3, 5, 5, 6, 7, 5, 3};
    static const signed char index16[8] = {6, 7, 6, 7, -1, -1, -1, -1};

    out->base = base16[rm];
    out->index = index16[rm];
    if (mod == 0 && rm == 6) {
        out->base = KVM_REG_NONE;
        return 2;
    }
    return mod == 1 ? 1u : mod == 2 ? 2u : 0u;
}

/**
 * Set the base and index of a 32 or 64-bit ModRM's address in `out`, from
 * its `mod` and `rm` and the SIB byte that follows when rm is 4: 0 with
 * the displacement's bytes that follow in `*dsize`, or -1.
 */
static int addr32(struct dec *d, struct kvm_insn *out, int mod, int rm, uint32_t *dsize)
{
    uint64_t s;

    *dsize = mod == 1 ? 1u : mod == 2 ? 4u : 0u;
    if (rm == 5 && mod == 0) {
        out->base = d->mode == 64 ? KVM_REG_RIP : KVM_REG_NONE;
        *dsize = 4;
        return 0;
    }
    if (rm != 4) {
        out->base = rm | (d->rex & REX_B ? 8 : 0);
        return 0;
    }
    if (fetch(d, 1, &s) != 0) {
        return -1;
    }
    out->index = (int)((s >> 3) & 7) | (d->rex & REX_X ? 8 : 0);
    if (out->index == 4) {
        out->index = KVM_REG_NONE;
    } else {
        out->scale = 1u << (s >> 6);
    }
    out->base = (int)(s & 7) | (d->rex & REX_B ? 8 : 0);
    if ((s & 7) == 5 && mod == 0) {
        out->base = KVM_REG_NONE;
        *dsize = 4;
    }
    return 0;
}

/**
 * Read a ModRM byte and the SIB byte and displacement after it, and set
 * the memory operand's address in `out`.
 *
 * Returns 0 with the reg field, 0 to 7 without REX.R, in `*regf`; -1 when
 * the ModRM names a register rather than memory (mod 3) or the bytes end.
 */
static int modrm(struct dec *d, struct kvm_insn *out, int *regf)
{
    uint64_t b, disp;
    int mod, rm;
    uint32_t dsize;

    if (fetch(d, 1, &b) != 0) {
        return -1;
    }
    mod = (int)(b >> 6);
    *regf = (int)(b >> 3) & 7;
    rm = (int)b & 7;
    if (mod == 3) {
        return -1;
    }
    if (d->asize == 2) {
        dsize = addr16(out, mod, rm);
    } else if (addr32(d, out, mod, rm, &dsize) != 0) {
        return -1;
    }
    if (dsize != 0) {
        if (fetch(d, dsize, &disp) != 0) {
            return -1;
        }
        out->disp = sext(disp, dsize);
    }
    /* rBP and rSP as the base address the stack segment; r12, r13 do not. */
    out->seg = out->base == 4 || out->base == 5 ? KVM_SEG_SS : KVM_SEG_DS;
    if (d->seg >= 0) {
        out->seg = (uint32_t)d->seg;
    }
    return 0;
}

/**
 * Read the prefixes before the opcode and set the operand and address
 * sizes they give: 0 with the opcode's first byte in `*op`, or -1.
 * A legacy prefix after a REX voids the REX, as on the processor.
 */
static int prefixes(struct dec *d, uint64_t *op)
{
    int adsize = 0;

    for (;;) {
        if (fetch(d, 1, op) != 0) {
            return -1;
        }
        switch (*op) {
        case 0x66:
            d->opsize = 1;
            break;
        case 0x67:
            adsize = 1;
            break;
        case 0x26:
            d->seg = KVM_SEG_ES;
            break;
        case 0x2E:
            d->seg = KVM_SEG_CS;
            break;
        case 0x36:
            d->seg = KVM_SEG_SS;
            break;
        case 0x3E:
            d->seg = KVM_SEG_DS;
            break;
        case 0x64:
            d->seg = KVM_SEG_FS;
            break;
        case 0x65:
            d->seg = KVM_SEG_GS;
            break;
        case 0xF0:
            d->lock = 1;
            break;
        case 0xF2:
        case 0xF3:
            d->rep = (int)*op;
            break;
        default:
            if (d->mode == 64 && (*op & 0xF0) == 0x40) {
                d->rex = (int)*op;
                continue;
            }
            goto done;
        }
        d->rex = 0;
    }
done:
    if (d->rex & REX_W) {
        d->osize = 8;
    } else if (d->opsize) {
        d->osize = d->mode == 16 ? 4 : 2;
    } else {
        d->osize = d->mode == 16 ? 2 : 4;
    }
    if (d->mode == 16) {
        d->asize = adsize ? 4 : 2;
    } else if (d->mode == 32) {
        d->asize = adsize ? 2 : 4;
    } else {
        d->asize = adsize ? 4 : 8;
    }
    return 0;
}

/** The operand size of PUSH and POP: 8 in 64-bit mode, or 2 with 0x66,
 * which REX.W does not change; else the operand size. */
static uint32_t stack_size(const struct dec *d)
{
    if (d->mode == 64) {
        return d->opsize && !(d->rex & REX_W) ? 2 : 8;
    }
    return d->osize;
}

/**
 * Decode the opcodes that begin with 0x0F: MOVZX, MOVSX and MOVNTI.
 * Returns 0 or -1; `out` takes the operand.
 */
static int decode_0f(struct dec *d, struct kvm_insn *out)
{
    uint64_t op;
    int regf;

    if (fetch(d, 1, &op) != 0) {
        return -1;
    }
    switch (op) {
    case 0xB6:
    case 0xB7:
    case 0xBE:
    case 0xBF:
        if (modrm(d, out, &regf) != 0) {
            return -1;
        }
        out->op = KVM_OP_LOAD;
        out->flags |= KVM_INSN_READ | (op & 0x08 ? KVM_INSN_SX : KVM_INSN_ZX);
        out->size = op & 1 ? 2 : 1;
        out->reg_size = d->osize;
        out->reg = reg_of(d, regf);
        return 0;
    case 0xC3:
        /* MOVNTI takes no 0x66, 0xF2 or 0xF3. */
        if (d->opsize || d->rep) {
            return -1;
        }
        if (modrm(d, out, &regf) != 0) {
            return -1;
        }
        out->op = KVM_OP_STORE;
        out->flags |= KVM_INSN_WRITE;
        out->size = d->rex & REX_W ? 8 : 4;
        out->reg = reg_of(d, regf);
        return 0;
    default:
        return -1;
    }
}

/** ADD, OR, ADC, SBB, AND, SUB, XOR and CMP of memory and a register,
 * opcodes 0x00 to 0x3B: 0 or -1. */
static int op_alu(struct dec *d, uint32_t op, struct kvm_insn *out)
{
    int regf;

    if (modrm(d, out, &regf) != 0) {
        return -1;
    }
    out->op = KVM_OP_ALU;
    out->alu = op >> 3;
    out->size = op & 1 ? d->osize : 1;
    set_reg(d, out, reg_of(d, regf), out->size);
    out->flags |= KVM_INSN_READ;
    if (op & 2) {
        out->flags |= KVM_INSN_TO_REG;
    } else if (out->alu != KVM_ALU_CMP) {
        out->flags |= KVM_INSN_WRITE;
        d->lockable = 1;
    }
    return 0;
}

/** The same with an immediate, opcodes 0x80 to 0x83: 0 or -1. */
static int op_group1(struct dec *d, uint32_t op, struct kvm_insn *out)
{
    int regf;

    /* 0x82 is 0x80 outside 64-bit mode, and invalid in it. */
    if ((op == 0x82 && d->mode == 64) || modrm(d, out, &regf) != 0) {
        return -1;
    }
    out->op = KVM_OP_ALU;
    out->alu = (uint32_t)regf;
    out->size = op == 0x81 || op == 0x83 ? d->osize : 1;
    if (imm(d, op == 0x81 ? out->size : 1, out) != 0) {
        return -1;
    }
    out->flags |= KVM_INSN_READ;
    if (out->alu != KVM_ALU_CMP) {
        out->flags |= KVM_INSN_WRITE;
        d->lockable = 1;
    }
    return 0;
}

/** TEST, XCHG and MOV between memory and a register, opcodes 0x84 to
 * 0x8B: 0 or -1. */
static int op_reg(struct dec *d, uint32_t op, struct kvm_insn *out)
{
    int regf;

    if (modrm(d, out, &regf) != 0) {
        return -1;
    }
    out->size = op & 1 ? d->osize : 1;
    set_reg(d, out, reg_of(d, regf), out->size);
    if (op < 0x86) {
        out->op = KVM_OP_ALU;
        out->alu = KVM_ALU_TEST;
        out->flags |= KVM_INSN_READ;
    } else if (op < 0x88) {
        out->op = KVM_OP_XCHG;
        out->flags |= KVM_INSN_READ | KVM_INSN_WRITE;
        d->lockable = 1;
    } else if (op & 2) {
        out->op = KVM_OP_LOAD;
        out->flags |= KVM_INSN_READ;
    } else {
        out->op = KVM_OP_STORE;
        out->flags |= KVM_INSN_WRITE;
    }
    return 0;
}

/** MOV between rAX and an absolute address, opcodes 0xA0 to 0xA3, whose
 * offset is as wide as the address: 0 or -1. */
static int op_moffs(struct dec *d, uint32_t op, struct kvm_insn *out)
{
    if (fetch(d, d->asize, &out->disp) != 0) {
        return -1;
    }
    if (d->seg >= 0) {
        out->seg = (uint32_t)d->seg;
    }
    out->size = op & 1 ? d->osize : 1;
    out->reg = 0;
    if (op & 2) {
        out->op = KVM_OP_STORE;
        out->flags |= KVM_INSN_WRITE;
    } else {
        out->op = KVM_OP_LOAD;
        out->flags |= KVM_INSN_READ;
    }
    return 0;
}

/** MOVSXD, opcode 0x63 in 64-bit mode; ARPL outside it: 0 or -1. */
static int op_movsxd(struct dec *d, struct kvm_insn *out)
{
    int regf;

    if (d->mode != 64 || modrm(d, out, &regf) != 0) {
        return -1;
    }
    out->op = KVM_OP_LOAD;
    out->flags |= KVM_INSN_READ;
    out->size = d->osize == 8 ? 4 : d->osize;
    out->reg_size = d->osize;
    if (d->osize == 8) {
        out->flags |= KVM_INSN_SX;
    }
    out->reg = reg_of(d, regf);
    return 0;
}

/** MOVS and STOS, opcodes 0xA4, 0xA5, 0xAA and 0xAB: 0. */
static int op_string(struct dec *d, uint32_t op, struct kvm_insn *out)
{
    out->size = op & 1 ? d->osize : 1;
    if (op < 0xA6) {
        out->op = KVM_OP_MOVS;
        out->flags |= KVM_INSN_READ | KVM_INSN_WRITE;
        if (d->seg >= 0) {
            out->seg = (uint32_t)d->seg;
        }
    } else {
        /* STOS writes ES:rDI; no override applies to it. */
        out->op = KVM_OP_STOS;
        out->flags |= KVM_INSN_WRITE;
        out->seg = KVM_SEG_ES;
        out->reg = 0;
    }
    if (d->rep == 0xF3) {
        out->flags |= KVM_INSN_REP;
    } else if (d->rep == 0xF2) {
        out->flags |= KVM_INSN_REPNE;
    }
    return 0;
}

/** POP of memory, 0x8F /0, and MOV of an immediate, 0xC6 /0 and
 * 0xC7 /0: 0 or -1. */
static int op_pop_movi(struct dec *d, uint32_t op, struct kvm_insn *out)
{
    int regf;

    if (modrm(d, out, &regf) != 0 || regf != 0) {
        return -1;
    }
    out->flags |= KVM_INSN_WRITE;
    if (op == 0x8F) {
        out->op = KVM_OP_POP;
        out->size = stack_size(d);
        return 0;
    }
    out->op = KVM_OP_STORE;
    out->size = op & 1 ? d->osize : 1;
    return imm(d, out->size, out);
}

/** TEST of an immediate, NOT and NEG, 0xF6 and 0xF7 /0 to /3: 0 or -1. */
static int op_group3(struct dec *d, uint32_t op, struct kvm_insn *out)
{
    int regf;

    if (modrm(d, out, &regf) != 0 || regf > 3) {
        return -1;
    }
    out->size = op & 1 ? d->osize : 1;
    if (regf < 2) {
        out->op = KVM_OP_ALU;
        out->alu = KVM_ALU_TEST;
        out->flags |= KVM_INSN_READ;
        return imm(d, out->size, out);
    }
    out->op = KVM_OP_UNARY;
    out->alu = regf == 2 ? KVM_ALU_NOT : KVM_ALU_NEG;
    out->flags |= KVM_INSN_READ | KVM_INSN_WRITE;
    d->lockable = 1;
    return 0;
}

/** INC and DEC, 0xFE and 0xFF /0 and /1, and PUSH of memory, 0xFF /6:
 * 0 or -1. */
static int op_group45(struct dec *d, uint32_t op, struct kvm_insn *out)
{
    int regf;

    if (modrm(d, out, &regf) != 0) {
        return -1;
    }
    if (regf < 2) {
        out->op = KVM_OP_UNARY;
        out->alu = regf == 0 ? KVM_ALU_INC : KVM_ALU_DEC;
        out->flags |= KVM_INSN_READ | KVM_INSN_WRITE;
        out->size = op & 1 ? d->osize : 1;
        d->lockable = 1;
        return 0;
    }
    if (op == 0xFF && regf == 6) {
        out->op = KVM_OP_PUSH;
        out->flags |= KVM_INSN_READ;
        out->size = stack_size(d);
        return 0;
    }
    return -1;
}

/** Decode the opcode `op` and what follows it into `out`: 0 or -1. */
static int dispatch(struct dec *d, uint32_t op, struct kvm_insn *out)
{
    if (op < 0x40) {
        return (op & 7) < 4 ? op_alu(d, op, out) : -1;
    }
    if (op >= 0x80 && op <= 0x83) {
        return op_group1(d, op, out);
    }
    if (op >= 0x84 && op <= 0x8B) {
        return op_reg(d, op, out);
    }
    if (op >= 0xA0 && op <= 0xA3) {
        return op_moffs(d, op, out);
    }
    switch (op) {
    case 0x63:
        return op_movsxd(d, out);
    case 0x8F:
    case 0xC6:
    case 0xC7:
        return op_pop_movi(d, op, out);
    case 0xA4:
    case 0xA5:
    case 0xAA:
    case 0xAB:
        return op_string(d, op, out);
    case 0xF6:
    case 0xF7:
        return op_group3(d, op, out);
    case 0xFE:
    case 0xFF:
        return op_group45(d, op, out);
    default:
        return -1;
    }
}

int kvm_decode(const uint8_t *bytes, uint32_t n, int mode, struct kvm_insn *out)
{
    struct dec d;
    uint64_t op;
    uint8_t *raw;
    uint32_t i;

    raw = (uint8_t *)out;
    for (i = 0; i < sizeof(*out); i++) {
        raw[i] = 0;
    }
    if (mode != 16 && mode != 32 && mode != 64) {
        return -1;
    }
    raw = (uint8_t *)&d;
    for (i = 0; i < sizeof(d); i++) {
        raw[i] = 0;
    }
    d.p = bytes;
    d.n = n < KVM_INSN_MAX ? n : KVM_INSN_MAX;
    d.mode = mode;
    d.seg = -1;
    out->reg = KVM_REG_NONE;
    out->base = KVM_REG_NONE;
    out->index = KVM_REG_NONE;
    out->scale = 1;
    out->seg = KVM_SEG_DS;
    if (prefixes(&d, &op) != 0) {
        return -1;
    }
    out->addr_size = d.asize;
    if ((op == 0x0F ? decode_0f(&d, out) : dispatch(&d, (uint32_t)op, out)) != 0) {
        return -1;
    }
    if (d.lock) {
        if (!d.lockable) {
            return -1;
        }
        out->flags |= KVM_INSN_LOCK;
    }
    if (out->reg_size == 0) {
        out->reg_size = out->size;
    }
    for (i = 0; i < d.at; i++) {
        out->bytes[i] = bytes[i];
    }
    out->len = d.at;
    return 0;
}

void kvm_decode_fail(struct kvm_run_internal *out, const uint8_t *bytes, uint32_t n,
                     const uint64_t info[5])
{
    uint8_t *raw;
    uint32_t i, at = 1;

    raw = (uint8_t *)out;
    for (i = 0; i < sizeof(*out); i++) {
        raw[i] = 0;
    }
    out->suberror = KVM_INTERNAL_ERROR_EMULATION;
    if (n > KVM_INSN_MAX) {
        n = KVM_INSN_MAX;
    }
    if (n > 0) {
        out->data[0] = KVM_INTERNAL_ERROR_EMULATION_FLAG_INSTRUCTION_BYTES;
        /* insn_size and insn_bytes[15] of Linux's emulation_failure. */
        raw = (uint8_t *)&out->data[1];
        raw[0] = (uint8_t)n;
        for (i = 0; i < KVM_INSN_MAX; i++) {
            raw[1 + i] = i < n ? bytes[i] : 0x90;
        }
        at = 3;
    }
    for (i = 0; i < 5; i++) {
        out->data[at + i] = info ? info[i] : 0;
    }
    out->ndata = at + 5;
}
