/*
 * decode.h: the decoder of the guest instructions that touch MMIO, which a
 * backend calls when a guest's access to a frame no slot holds exits, and
 * the internal error it reports for an instruction it does not know.
 */
#ifndef _KVM_DECODE_H_
#define _KVM_DECODE_H_

#include "../lib/kint.h"
#include "kvm_abi.h"

/* The longest x86 instruction, in bytes. */
#define KVM_INSN_MAX 15

/*
 * What an instruction does with its memory operand, `op`. The register is
 * `reg`, the immediate `imm`; each name below is the instruction's effect.
 */
#define KVM_OP_LOAD  1 /* reg = memory, widened as ZX or SX says */
#define KVM_OP_STORE 2 /* memory = reg, or imm with IMM */
#define KVM_OP_ALU   3 /* memory = memory `alu` reg or imm; reg with TO_REG */
#define KVM_OP_UNARY 4 /* memory = `alu` memory */
#define KVM_OP_XCHG  5 /* memory and reg swap */
#define KVM_OP_MOVS  6 /* [ES:rDI] = [seg:rSI], both stepped by size */
#define KVM_OP_STOS  7 /* [ES:rDI] = reg (rAX), rDI stepped by size */
#define KVM_OP_PUSH  8 /* [SS:rSP - size] = memory, then rSP -= size */
#define KVM_OP_POP   9 /* rSP += size, then memory = [SS:old rSP] */

/*
 * The operation of KVM_OP_ALU and KVM_OP_UNARY, `alu`. ADD to CMP are the
 * reg field of opcodes 0x80 to 0x83, and bits 3 to 5 of opcodes 0x00 to
 * 0x3B. CMP and TEST set the flags and write nothing.
 */
#define KVM_ALU_ADD  0
#define KVM_ALU_OR   1
#define KVM_ALU_ADC  2
#define KVM_ALU_SBB  3
#define KVM_ALU_AND  4
#define KVM_ALU_SUB  5
#define KVM_ALU_XOR  6
#define KVM_ALU_CMP  7
#define KVM_ALU_TEST 8
#define KVM_ALU_INC  9
#define KVM_ALU_DEC  10
#define KVM_ALU_NOT  11
#define KVM_ALU_NEG  12

/* The bits of `flags`. */
#define KVM_INSN_READ   0x001u /* the memory operand is read */
#define KVM_INSN_WRITE  0x002u /* the memory operand is written */
#define KVM_INSN_IMM    0x004u /* the source is `imm`, not `reg` */
#define KVM_INSN_TO_REG 0x008u /* KVM_OP_ALU's result goes to `reg` */
#define KVM_INSN_ZX     0x010u /* KVM_OP_LOAD zero-extends to reg_size */
#define KVM_INSN_SX     0x020u /* KVM_OP_LOAD sign-extends to reg_size */
#define KVM_INSN_HIGH8  0x040u /* `reg` 0 to 3 names AH, CH, DH or BH */
#define KVM_INSN_REP    0x080u /* MOVS or STOS with F3, repeated rCX times */
#define KVM_INSN_REPNE  0x100u /* MOVS or STOS with F2, which repeats as F3 */
#define KVM_INSN_LOCK   0x200u /* a LOCK prefix, where one is allowed */

/* The segments of `seg`, in the processor's numbering. */
#define KVM_SEG_ES 0
#define KVM_SEG_CS 1
#define KVM_SEG_SS 2
#define KVM_SEG_DS 3
#define KVM_SEG_FS 4
#define KVM_SEG_GS 5

/* `reg`, `base` and `index` name general registers 0 (rAX) to 15 (r15),
 * in the processor's numbering, or one of these. */
#define KVM_REG_NONE (-1)
#define KVM_REG_RIP  16 /* `base` alone: the next instruction's address */

/*
 * One decoded instruction that accesses memory. The memory operand is at
 * seg:(base + index * scale + disp), cut to `addr_size` bytes, for every
 * op but MOVS and STOS, whose operands are the string registers' and
 * whose `base` and `index` are none. In 64-bit mode only an FS or GS
 * `seg` has a base. The stack access of PUSH and POP takes its address
 * size from SS, not from `addr_size`. A write of a 4-byte register in
 * 64-bit mode clears the register's upper half, as the processor does.
 */
struct kvm_insn {
    uint8_t bytes[KVM_INSN_MAX]; /* the instruction, `len` bytes of it */
    uint32_t len;
    uint32_t op;        /* KVM_OP_* */
    uint32_t alu;       /* KVM_ALU_* for KVM_OP_ALU and KVM_OP_UNARY */
    uint32_t flags;     /* KVM_INSN_* */
    uint32_t size;      /* the memory operand's bytes: 1, 2, 4 or 8 */
    uint32_t reg_size;  /* reg's bytes: `size`, or wider with ZX or SX */
    int32_t reg;        /* the register operand, or KVM_REG_NONE */
    uint64_t imm;       /* the immediate, sign-extended to 64 bits */
    uint32_t addr_size; /* the address's bytes: 2, 4 or 8 */
    uint32_t seg;       /* KVM_SEG_*: the default or the override */
    int32_t base;       /* a register, KVM_REG_RIP or KVM_REG_NONE */
    int32_t index;      /* a register or KVM_REG_NONE */
    uint32_t scale;     /* 1, 2, 4 or 8; 1 with no index */
    uint64_t disp;      /* sign-extended; a moffs is zero-extended */
};

/**
 * Decode the instruction at the start of the `n` bytes at `bytes`, run in
 * `mode` (16, 32 or 64, the code segment's), that accessed guest memory.
 *
 * Decoded are MOV of each size to and from memory, of an immediate, and
 * of moffs; MOVZX, MOVSX, MOVSXD and MOVNTI; MOVS and STOS with REP;
 * ADD, OR, ADC, SBB, AND, SUB, XOR and CMP with a memory operand; TEST,
 * XCHG, INC, DEC, NOT and NEG of memory; PUSH and POP of memory; with the
 * operand-size, address-size, segment, LOCK and REP prefixes and REX.
 * Bytes past the 15th are not read.
 *
 * Returns 0 with `out` filled, or -1 for an instruction outside that set,
 * one with no memory operand, a LOCK the processor refuses, or one longer
 * than `n` or 15 bytes. The caller then injects #UD above CPL 0 and at
 * CPL 0 reports kvm_decode_fail's error, as Linux's
 * handle_emulation_failure does.
 */
int kvm_decode(const uint8_t *bytes, uint32_t n, int mode, struct kvm_insn *out);

/**
 * Fill `out`, the run page's KVM_EXIT_INTERNAL_ERROR, as Linux's
 * prepare_emulation_failure_exit does for an instruction its emulator
 * refused: suberror KVM_INTERNAL_ERROR_EMULATION; data[0] the flags,
 * KVM_INTERNAL_ERROR_EMULATION_FLAG_INSTRUCTION_BYTES when `n` is not 0;
 * then the instruction's length and its first 15 bytes in 16 bytes,
 * padded with 0x90, in data[1] and data[2]; then `info`, the backend's exit
 * reason, its two qualifications, the interrupt information and the error
 * code, or zeros when `info` is null. `n` above 15 counts as 15.
 */
void kvm_decode_fail(struct kvm_run_internal *out, const uint8_t *bytes, uint32_t n,
                     const uint64_t info[5]);

#endif /* _KVM_DECODE_H_ */
