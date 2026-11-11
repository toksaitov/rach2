/*
 * decode.h: the decoder of the guest instructions the kernel carries out
 * itself, those Linux's emulator knows, which a backend calls when a
 * guest's access to a frame no slot holds exits or an instruction the
 * processor refuses is one the guest's CPUID has, and the internal error
 * it reports for an instruction it does not know.
 */
#ifndef _KVM_DECODE_H_
#define _KVM_DECODE_H_

#include "../lib/kint.h"
#include "kvm_abi.h"

/* The longest x86 instruction, in bytes. */
#define KVM_INSN_MAX 15

/*
 * What an instruction does, `op`, with its operands `dst`, `src` and
 * `src2`; `sub` chooses among the forms of one operation.
 */
#define KVM_OP_MOV       1  /* dst = src, widened as `sub` says (KVM_MOV_*) */
#define KVM_OP_ALU       2  /* dst = dst `sub` src (KVM_ALU_ADD to KVM_ALU_CMPR) */
#define KVM_OP_UNARY     3  /* dst = `sub` dst (KVM_ALU_INC to KVM_ALU_NEG) */
#define KVM_OP_XCHG      4  /* dst and src swap */
#define KVM_OP_SHIFT     5  /* dst rotated or shifted by src (KVM_SHIFT_*) */
#define KVM_OP_MULDIV    6  /* rDX:rAX = rAX * src, or divided by src (KVM_MD_*) */
#define KVM_OP_IMUL      7  /* dst = src * src2, or dst * src with no src2 */
#define KVM_OP_BT        8  /* the bit src of dst tested, and changed (KVM_BT_*) */
#define KVM_OP_BSCAN     9  /* dst = the lowest set bit of src, or the highest */
#define KVM_OP_DSHIFT    10 /* SHLD, or SHRD with `sub` 1: dst, src, count src2 */
#define KVM_OP_CMPXCHG   11
#define KVM_OP_CMPXCHG8B 12
#define KVM_OP_XADD      13
#define KVM_OP_CMOV      14 /* dst = src under the condition `sub` */
#define KVM_OP_SETCC     15 /* dst = the condition `sub`, a byte */
#define KVM_OP_MOVBE     16
#define KVM_OP_BSWAP     17
#define KVM_OP_LEA       18 /* dst = src's address */
#define KVM_OP_NOP       19 /* NOP, PAUSE, a prefetch, CLFLUSH */
#define KVM_OP_PUSH      20 /* src pushed */
#define KVM_OP_POP       21 /* dst popped */
#define KVM_OP_PUSHA     22
#define KVM_OP_POPA      23
#define KVM_OP_PUSHF     24
#define KVM_OP_POPF      25
#define KVM_OP_PUSH_SREG 26 /* the segment register `sub` pushed */
#define KVM_OP_POP_SREG  27
#define KVM_OP_ENTER     28 /* src bytes of frame, nesting src2 */
#define KVM_OP_LEAVE     29
#define KVM_OP_JCC       30 /* a relative jump by src under the condition `sub` */
#define KVM_OP_JMP       31 /* a relative jump by src */
#define KVM_OP_JMP_ABS   32 /* a jump to src */
#define KVM_OP_JMP_FAR   33 /* to src's far pointer, or src2:src immediate */
#define KVM_OP_CALL      34
#define KVM_OP_CALL_ABS  35
#define KVM_OP_CALL_FAR  36
#define KVM_OP_RET       37 /* near, releasing src bytes of stack */
#define KVM_OP_RET_FAR   38
#define KVM_OP_LOOP      39 /* LOOPNE, LOOPE, LOOP and JrCXZ, `sub` 0 to 3 */
#define KVM_OP_MOV_SREG  40 /* the segment register dst = src */
#define KVM_OP_STORE_SEG 41 /* dst = a selector: src's, or with `sub` LDTR or TR */
#define KVM_OP_LOAD_SEG  42 /* LLDT or LTR of src, `sub` KVM_SEG_LDTR or KVM_SEG_TR */
#define KVM_OP_LSEG      43 /* LES to LGS: dst and the segment `sub` from src */
#define KVM_OP_STORE_DT  44 /* SGDT, or SIDT with `sub` 1 */
#define KVM_OP_LOAD_DT   45 /* LGDT, or LIDT with `sub` 1 */
#define KVM_OP_SMSW      46
#define KVM_OP_LMSW      47
#define KVM_OP_FLAG      48 /* one flag changed (KVM_FLAG_*) */
#define KVM_OP_CBW       49 /* CBW, CWDE, CDQE */
#define KVM_OP_CWD       50 /* CWD, CDQ, CQO */
#define KVM_OP_SAHF      51
#define KVM_OP_LAHF      52
#define KVM_OP_BCD       53 /* DAS, AAM, AAD and SALC (KVM_BCD_*) */
#define KVM_OP_FNSTCW    54
#define KVM_OP_FNSTSW    55
#define KVM_OP_FNINIT    56
#define KVM_OP_FXSAVE    57
#define KVM_OP_FXRSTOR   58
#define KVM_OP_SYSCALL   59
#define KVM_OP_SYSENTER  60
#define KVM_OP_SYSEXIT   61
#define KVM_OP_HYPERCALL 62 /* VMCALL, or VMMCALL with `sub` 1 */
#define KVM_OP_RDPID     63 /* dst = IA32_TSC_AUX */
#define KVM_OP_LAST      63

/*
 * The operation of KVM_OP_ALU and KVM_OP_UNARY, `sub`. ADD to CMP are the
 * reg field of opcodes 0x80 to 0x83, and bits 3 to 5 of opcodes 0x00 to
 * 0x3B. CMP, TEST and CMPR set the flags and write nothing; CMPR compares
 * src with dst, as CMPS and SCAS do.
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
#define KVM_ALU_CMPR 13

/* How KVM_OP_MOV widens src to dst, `sub`. */
#define KVM_MOV_PLAIN 0
#define KVM_MOV_ZX    1
#define KVM_MOV_SX    2

/* KVM_OP_SHIFT's `sub`, the reg field of opcodes 0xC0 to 0xD3. */
#define KVM_SHIFT_ROL 0
#define KVM_SHIFT_ROR 1
#define KVM_SHIFT_RCL 2
#define KVM_SHIFT_RCR 3
#define KVM_SHIFT_SHL 4
#define KVM_SHIFT_SHR 5
#define KVM_SHIFT_SAL 6
#define KVM_SHIFT_SAR 7

/* KVM_OP_MULDIV's `sub`, the reg field of opcodes 0xF6 and 0xF7. */
#define KVM_MD_MUL  4
#define KVM_MD_IMUL 5
#define KVM_MD_DIV  6
#define KVM_MD_IDIV 7

/* KVM_OP_BT's `sub`. */
#define KVM_BT_BT  0
#define KVM_BT_BTS 1
#define KVM_BT_BTR 2
#define KVM_BT_BTC 3

/* KVM_OP_FLAG's `sub`. */
#define KVM_FLAG_CMC 0
#define KVM_FLAG_CLC 1
#define KVM_FLAG_STC 2
#define KVM_FLAG_CLI 3
#define KVM_FLAG_STI 4
#define KVM_FLAG_CLD 5
#define KVM_FLAG_STD 6

/* KVM_OP_BCD's `sub`. */
#define KVM_BCD_DAS  0
#define KVM_BCD_AAM  1
#define KVM_BCD_AAD  2
#define KVM_BCD_SALC 3

/* The bits of `flags`. */
#define KVM_INSN_REP      0x001u /* an F3 prefix: REP, or REPE of CMPS and SCAS */
#define KVM_INSN_REPNE    0x002u /* an F2 prefix: REPNE of CMPS and SCAS, else REP */
#define KVM_INSN_LOCK     0x004u /* a LOCK prefix */
#define KVM_INSN_UD       0x008u /* the processor refuses the encoding with #UD */
#define KVM_INSN_MOV      0x010u /* dst is written without being read */
#define KVM_INSN_NOWRITE  0x020u /* dst is read and not written */
#define KVM_INSN_STRING   0x040u /* a string instruction, which REP repeats */
#define KVM_INSN_NOACCESS 0x080u /* the memory operand is named, not accessed */
#define KVM_INSN_ALIGNED  0x100u /* the memory operand is on a 16-byte boundary */
#define KVM_INSN_ALL      0x1FFu

/* The segments of `seg`, in the processor's numbering, and the two table
 * registers LLDT, LTR, SLDT and STR name. */
#define KVM_SEG_ES   0
#define KVM_SEG_CS   1
#define KVM_SEG_SS   2
#define KVM_SEG_DS   3
#define KVM_SEG_FS   4
#define KVM_SEG_GS   5
#define KVM_SEG_LDTR 6
#define KVM_SEG_TR   7

/* `base` and `index` name general registers 0 (rAX) to 15 (r15), in the
 * processor's numbering, or one of these. */
#define KVM_REG_NONE (-1)
#define KVM_REG_RIP  16 /* `base` alone: the next instruction's address */

/* The kinds of operand, `kind`. */
#define KVM_OPND_NONE 0
#define KVM_OPND_REG  1 /* the general register `reg`; AH to BH with `high8` */
#define KVM_OPND_MEM  2 /* memory, at the address `mem` names */
#define KVM_OPND_IMM  3 /* the immediate `imm` */
#define KVM_OPND_SREG 4 /* the segment register `reg` (KVM_SEG_*) */
#define KVM_OPND_XMM  5 /* the SSE register `reg` */
#define KVM_OPND_MM   6 /* the MMX register `reg` */

/* Where a memory operand is, `mem`. */
#define KVM_MEM_MODRM 0 /* at seg:(base + index * scale + disp) */
#define KVM_MEM_SI    1 /* at seg:rSI, a string's source */
#define KVM_MEM_DI    2 /* at ES:rDI, a string's destination */
#define KVM_MEM_XLAT  3 /* at seg:(rBX + AL) */

/* One operand of an instruction. */
struct kvm_operand {
    uint8_t kind;  /* KVM_OPND_* */
    uint8_t size;  /* its bytes: 1, 2, 4, 8, 16, or a far pointer's 4, 6 or 10 */
    uint8_t reg;   /* the register; 0 to 3 for AH, CH, DH and BH with `high8` */
    uint8_t high8; /* `reg` names a high byte register */
    uint8_t mem;   /* KVM_MEM_* */
    uint64_t imm;  /* the immediate, sign-extended to 64 bits unless unsigned */
};

/*
 * One decoded instruction. The memory operand of a ModRM byte or of a
 * moffs is at seg:(base + index * scale + disp), cut to `addr_size` bytes;
 * a string's operands are the string registers', with `seg` the source's
 * segment. In 64-bit mode only an FS or GS `seg` has a base. A stack access
 * takes its address size from SS, not from `addr_size`. A write of a 4-byte
 * register in 64-bit mode clears the register's upper half, as the
 * processor does. A far jump's or call's immediate target is `src2`'s
 * selector and `src`'s offset.
 */
struct kvm_insn {
    uint8_t bytes[KVM_INSN_MAX]; /* the instruction, `len` bytes of it */
    uint32_t len;
    uint32_t op;        /* KVM_OP_* */
    uint32_t sub;       /* the operation's form */
    uint32_t flags;     /* KVM_INSN_* */
    uint32_t size;      /* the operand size in bytes: 1, 2, 4, 8 or 16 */
    uint32_t addr_size; /* the address's bytes: 2, 4 or 8 */
    struct kvm_operand dst, src, src2;
    uint32_t seg;   /* KVM_SEG_ES to KVM_SEG_GS: the default or the override */
    int32_t base;   /* a register, KVM_REG_RIP or KVM_REG_NONE */
    int32_t index;  /* a register or KVM_REG_NONE */
    uint32_t scale; /* 1, 2, 4 or 8; 1 with no index */
    uint64_t disp;  /* sign-extended; a moffs is zero-extended */
};

/**
 * Decode the instruction at the start of the `n` bytes at `bytes`, run in
 * `mode` (16, 32 or 64, the code segment's).
 *
 * Decoded are the instructions Linux's emulator carries out that can touch
 * memory, with their register forms: the moves with their extensions, the
 * arithmetic, logic, shift, multiply, divide and bit instructions, the
 * string instructions with REP, the stack instructions, the near and far
 * jumps, calls and returns, the conditional moves, sets and jumps, the
 * segment and descriptor table loads and stores, SMSW and LMSW, the SSE
 * and MMX moves, FNSTCW, FNSTSW, FNINIT, FXSAVE and FXRSTOR, and those it
 * carries out after a #UD: SYSCALL, SYSENTER, SYSEXIT, RDPID, VMCALL and
 * VMMCALL, with the operand-size, address-size, segment, LOCK and REP
 * prefixes and REX. Port I/O, the control and debug registers, the MSRs
 * and the instructions that always exit are the backends'. Bytes past the
 * 15th are not read.
 *
 * Returns 0 with `out` filled, or -1 for an instruction outside that set or
 * one longer than `n` or 15 bytes. The caller then queues #UD and at CPL 0
 * also reports kvm_decode_fail's error, as Linux's
 * handle_emulation_failure does. An encoding of the set that the processor
 * refuses, such as a LOCK it does not take or an instruction 64-bit mode
 * lacks, decodes with KVM_INSN_UD and may have no operands.
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
