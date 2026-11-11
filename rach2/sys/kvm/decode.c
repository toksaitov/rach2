/*
 * decode.c: the decoder of the guest instructions the kernel carries out
 * itself, from their bytes to an operation and its operands, and the
 * internal error that reports one it does not know. decode.h has the
 * contract; the opcode tables follow Linux's emulator, arch/x86/kvm/emulate.c.
 */

#include "decode.h"

/* The REX bits. */
#define REX_W 0x8
#define REX_R 0x4
#define REX_X 0x2
#define REX_B 0x1

/* How the tables name an operand, before the ModRM byte and the prefixes
 * make a struct kvm_operand of it. */
#define O_NONE   0
#define O_REG    1 /* ModRM's reg field, a general register */
#define O_RM     2 /* ModRM's r/m, a register or memory */
#define O_RM8    3 /* the same, one byte whatever the operand size */
#define O_RM16   4
#define O_RM32   5
#define O_M      6  /* ModRM's r/m, memory alone */
#define O_ACC    7  /* rAX */
#define O_OPREG  8  /* the register in the opcode's low three bits */
#define O_IMM    9  /* an immediate of the operand size, four bytes at most */
#define O_IMM8   10 /* a byte, sign-extended */
#define O_IMMU8  11 /* a byte, not extended */
#define O_IMMU16 12
#define O_IMM64  13 /* an immediate of the operand size, eight bytes too */
#define O_ONE    14 /* the constant 1 */
#define O_CL     15
#define O_MOFFS  16 /* memory at an absolute address as wide as the address */
#define O_SI     17
#define O_DI     18
#define O_XLAT   19
#define O_FARM   20 /* memory that holds a far pointer */
#define O_FARIMM 21 /* a far pointer in the instruction: src and src2 */
#define O_SREG   22 /* ModRM's reg field, a segment register */
#define O_XMM    23 /* ModRM's reg field, an SSE register */
#define O_XMMRM  24 /* ModRM's r/m, an SSE register or memory */
#define O_MM     25
#define O_MMRM   26
#define O_BASE   27 /* a group's member takes the opcode's own operand */

/* What the tables say of an opcode. */
#define T_MODRM   0x00001u
#define T_BYTE    0x00002u /* the operand size is one byte */
#define T_LOCK    0x00004u /* takes LOCK when dst is memory */
#define T_NO64    0x00008u /* #UD in 64-bit mode */
#define T_STACK   0x00010u /* a 4-byte operand is 8 in 64-bit mode */
#define T_GROUP   0x00020u /* `sub` names a group; ModRM's reg picks the member */
#define T_MOV     0x00040u /* KVM_INSN_MOV */
#define T_NOWRITE 0x00080u /* KVM_INSN_NOWRITE */
#define T_STRING  0x00100u /* KVM_INSN_STRING */
#define T_NEAR    0x00200u /* a near branch: the operand is 8 bytes in 64-bit mode */
#define T_NOACC   0x00400u /* KVM_INSN_NOACCESS */
#define T_ONLY64  0x00800u /* another instruction outside 64-bit mode */
#define T_CC      0x01000u /* `sub` is the opcode's low four bits, a condition */
#define T_UD      0x02000u /* #UD */
#define T_SSE     0x04000u /* the prefix picks the form (`sse`) */
#define T_ESC     0x08000u /* an x87 escape (`escape`) */
#define T_NO16    0x10000u /* a 2-byte operand is 4 */
#define T_ALIGN   0x20000u /* KVM_INSN_ALIGNED */

/* The groups, by the `sub` of their opcode's row. */
#define G1  0
#define G1A 1
#define G2  2
#define G3  3
#define G4  4
#define G5  5
#define G6  6
#define G7  7
#define G7R 8 /* group 7 with a register operand */
#define G8  9
#define G9  10
#define G11 11
#define G15 12

/* One entry of the tables: the operation and where its operands are. */
struct opc {
    uint8_t op; /* KVM_OP_*, or 0 for an opcode outside the set */
    uint8_t sub;
    uint8_t dst, src, src2; /* O_* */
    uint32_t flags;         /* T_* */
};

/* A row of an opcode table: the opcodes `first` to `last` share `e`. */
struct row {
    uint8_t first, last;
    struct opc e;
};

/* Opcodes 0x00 to 0x3F by their low three bits, for those below 6: ADD to
 * CMP between r/m and a register, and of rAX and an immediate. */
static const struct opc alu_forms[6] = {
    {KVM_OP_ALU, 0, O_RM, O_REG, 0, T_MODRM | T_BYTE | T_LOCK},
    {KVM_OP_ALU, 0, O_RM, O_REG, 0, T_MODRM | T_LOCK},
    {KVM_OP_ALU, 0, O_REG, O_RM, 0, T_MODRM | T_BYTE},
    {KVM_OP_ALU, 0, O_REG, O_RM, 0, T_MODRM},
    {KVM_OP_ALU, 0, O_ACC, O_IMM, 0, T_BYTE},
    {KVM_OP_ALU, 0, O_ACC, O_IMM, 0, 0},
};

/* The one-byte opcodes from 0x06, in order. */
static const struct row one_byte[] = {
    {0x06, 0x06, {KVM_OP_PUSH_SREG, KVM_SEG_ES, 0, 0, 0, T_NO64}},
    {0x07, 0x07, {KVM_OP_POP_SREG, KVM_SEG_ES, 0, 0, 0, T_NO64}},
    {0x0E, 0x0E, {KVM_OP_PUSH_SREG, KVM_SEG_CS, 0, 0, 0, T_NO64}},
    {0x16, 0x16, {KVM_OP_PUSH_SREG, KVM_SEG_SS, 0, 0, 0, T_NO64}},
    {0x17, 0x17, {KVM_OP_POP_SREG, KVM_SEG_SS, 0, 0, 0, T_NO64}},
    {0x1E, 0x1E, {KVM_OP_PUSH_SREG, KVM_SEG_DS, 0, 0, 0, T_NO64}},
    {0x1F, 0x1F, {KVM_OP_POP_SREG, KVM_SEG_DS, 0, 0, 0, T_NO64}},
    {0x2F, 0x2F, {KVM_OP_BCD, KVM_BCD_DAS, 0, 0, 0, T_NO64}},
    {0x40, 0x47, {KVM_OP_UNARY, KVM_ALU_INC, O_OPREG, 0, 0, 0}},
    {0x48, 0x4F, {KVM_OP_UNARY, KVM_ALU_DEC, O_OPREG, 0, 0, 0}},
    {0x50, 0x57, {KVM_OP_PUSH, 0, 0, O_OPREG, 0, T_STACK}},
    {0x58, 0x5F, {KVM_OP_POP, 0, O_OPREG, 0, 0, T_STACK | T_MOV}},
    {0x60, 0x60, {KVM_OP_PUSHA, 0, 0, 0, 0, T_NO64}},
    {0x61, 0x61, {KVM_OP_POPA, 0, 0, 0, 0, T_NO64}},
    {0x63, 0x63, {KVM_OP_MOV, KVM_MOV_SX, O_REG, O_RM32, 0, T_MODRM | T_MOV | T_ONLY64}},
    {0x68, 0x68, {KVM_OP_PUSH, 0, 0, O_IMM, 0, T_STACK}},
    {0x69, 0x69, {KVM_OP_IMUL, 0, O_REG, O_RM, O_IMM, T_MODRM}},
    {0x6A, 0x6A, {KVM_OP_PUSH, 0, 0, O_IMM8, 0, T_STACK}},
    {0x6B, 0x6B, {KVM_OP_IMUL, 0, O_REG, O_RM, O_IMM8, T_MODRM}},
    {0x70, 0x7F, {KVM_OP_JCC, 0, 0, O_IMM8, 0, T_NEAR | T_CC}},
    {0x80, 0x80, {0, G1, O_RM, O_IMM, 0, T_MODRM | T_BYTE | T_GROUP}},
    {0x81, 0x81, {0, G1, O_RM, O_IMM, 0, T_MODRM | T_GROUP}},
    {0x82, 0x82, {0, G1, O_RM, O_IMM, 0, T_MODRM | T_BYTE | T_GROUP | T_NO64}},
    {0x83, 0x83, {0, G1, O_RM, O_IMM8, 0, T_MODRM | T_GROUP}},
    {0x84, 0x84, {KVM_OP_ALU, KVM_ALU_TEST, O_RM, O_REG, 0, T_MODRM | T_BYTE | T_NOWRITE}},
    {0x85, 0x85, {KVM_OP_ALU, KVM_ALU_TEST, O_RM, O_REG, 0, T_MODRM | T_NOWRITE}},
    {0x86, 0x86, {KVM_OP_XCHG, 0, O_RM, O_REG, 0, T_MODRM | T_BYTE | T_LOCK}},
    {0x87, 0x87, {KVM_OP_XCHG, 0, O_RM, O_REG, 0, T_MODRM | T_LOCK}},
    {0x88, 0x88, {KVM_OP_MOV, 0, O_RM, O_REG, 0, T_MODRM | T_BYTE | T_MOV}},
    {0x89, 0x89, {KVM_OP_MOV, 0, O_RM, O_REG, 0, T_MODRM | T_MOV}},
    {0x8A, 0x8A, {KVM_OP_MOV, 0, O_REG, O_RM, 0, T_MODRM | T_BYTE | T_MOV}},
    {0x8B, 0x8B, {KVM_OP_MOV, 0, O_REG, O_RM, 0, T_MODRM | T_MOV}},
    {0x8C, 0x8C, {KVM_OP_STORE_SEG, 0, O_RM, O_SREG, 0, T_MODRM | T_MOV}},
    {0x8D, 0x8D, {KVM_OP_LEA, 0, O_REG, O_M, 0, T_MODRM | T_MOV | T_NOACC}},
    {0x8E, 0x8E, {KVM_OP_MOV_SREG, 0, O_SREG, O_RM16, 0, T_MODRM | T_MOV}},
    {0x8F, 0x8F, {0, G1A, 0, 0, 0, T_MODRM | T_GROUP}},
    {0x90, 0x97, {KVM_OP_XCHG, 0, O_OPREG, O_ACC, 0, 0}},
    {0x98, 0x98, {KVM_OP_CBW, 0, 0, 0, 0, 0}},
    {0x99, 0x99, {KVM_OP_CWD, 0, 0, 0, 0, 0}},
    {0x9A, 0x9A, {KVM_OP_CALL_FAR, 0, 0, O_FARIMM, 0, T_NO64}},
    {0x9C, 0x9C, {KVM_OP_PUSHF, 0, 0, 0, 0, T_STACK}},
    {0x9D, 0x9D, {KVM_OP_POPF, 0, 0, 0, 0, T_STACK}},
    {0x9E, 0x9E, {KVM_OP_SAHF, 0, 0, 0, 0, 0}},
    {0x9F, 0x9F, {KVM_OP_LAHF, 0, 0, 0, 0, 0}},
    {0xA0, 0xA0, {KVM_OP_MOV, 0, O_ACC, O_MOFFS, 0, T_BYTE | T_MOV}},
    {0xA1, 0xA1, {KVM_OP_MOV, 0, O_ACC, O_MOFFS, 0, T_MOV}},
    {0xA2, 0xA2, {KVM_OP_MOV, 0, O_MOFFS, O_ACC, 0, T_BYTE | T_MOV}},
    {0xA3, 0xA3, {KVM_OP_MOV, 0, O_MOFFS, O_ACC, 0, T_MOV}},
    {0xA4, 0xA4, {KVM_OP_MOV, 0, O_DI, O_SI, 0, T_BYTE | T_MOV | T_STRING}},
    {0xA5, 0xA5, {KVM_OP_MOV, 0, O_DI, O_SI, 0, T_MOV | T_STRING}},
    {0xA6, 0xA6, {KVM_OP_ALU, KVM_ALU_CMPR, O_DI, O_SI, 0, T_BYTE | T_NOWRITE | T_STRING}},
    {0xA7, 0xA7, {KVM_OP_ALU, KVM_ALU_CMPR, O_DI, O_SI, 0, T_NOWRITE | T_STRING}},
    {0xA8, 0xA8, {KVM_OP_ALU, KVM_ALU_TEST, O_ACC, O_IMM, 0, T_BYTE | T_NOWRITE}},
    {0xA9, 0xA9, {KVM_OP_ALU, KVM_ALU_TEST, O_ACC, O_IMM, 0, T_NOWRITE}},
    {0xAA, 0xAA, {KVM_OP_MOV, 0, O_DI, O_ACC, 0, T_BYTE | T_MOV | T_STRING}},
    {0xAB, 0xAB, {KVM_OP_MOV, 0, O_DI, O_ACC, 0, T_MOV | T_STRING}},
    {0xAC, 0xAC, {KVM_OP_MOV, 0, O_ACC, O_SI, 0, T_BYTE | T_MOV | T_STRING}},
    {0xAD, 0xAD, {KVM_OP_MOV, 0, O_ACC, O_SI, 0, T_MOV | T_STRING}},
    {0xAE, 0xAE, {KVM_OP_ALU, KVM_ALU_CMPR, O_DI, O_ACC, 0, T_BYTE | T_NOWRITE | T_STRING}},
    {0xAF, 0xAF, {KVM_OP_ALU, KVM_ALU_CMPR, O_DI, O_ACC, 0, T_NOWRITE | T_STRING}},
    {0xB0, 0xB7, {KVM_OP_MOV, 0, O_OPREG, O_IMM, 0, T_BYTE | T_MOV}},
    {0xB8, 0xBF, {KVM_OP_MOV, 0, O_OPREG, O_IMM64, 0, T_MOV}},
    {0xC0, 0xC0, {0, G2, O_RM, O_IMMU8, 0, T_MODRM | T_BYTE | T_GROUP}},
    {0xC1, 0xC1, {0, G2, O_RM, O_IMMU8, 0, T_MODRM | T_GROUP}},
    {0xC2, 0xC2, {KVM_OP_RET, 0, 0, O_IMMU16, 0, T_NEAR}},
    {0xC3, 0xC3, {KVM_OP_RET, 0, 0, 0, 0, T_NEAR}},
    {0xC4, 0xC4, {KVM_OP_LSEG, KVM_SEG_ES, O_REG, O_FARM, 0, T_MODRM | T_MOV}},
    {0xC5, 0xC5, {KVM_OP_LSEG, KVM_SEG_DS, O_REG, O_FARM, 0, T_MODRM | T_MOV}},
    {0xC6, 0xC6, {0, G11, O_RM, O_IMM, 0, T_MODRM | T_BYTE | T_GROUP}},
    {0xC7, 0xC7, {0, G11, O_RM, O_IMM, 0, T_MODRM | T_GROUP}},
    {0xC8, 0xC8, {KVM_OP_ENTER, 0, 0, O_IMMU16, O_IMMU8, T_STACK}},
    {0xC9, 0xC9, {KVM_OP_LEAVE, 0, 0, 0, 0, T_STACK}},
    {0xCA, 0xCA, {KVM_OP_RET_FAR, 0, 0, O_IMMU16, 0, 0}},
    {0xCB, 0xCB, {KVM_OP_RET_FAR, 0, 0, 0, 0, 0}},
    {0xD0, 0xD0, {0, G2, O_RM, O_ONE, 0, T_MODRM | T_BYTE | T_GROUP}},
    {0xD1, 0xD1, {0, G2, O_RM, O_ONE, 0, T_MODRM | T_GROUP}},
    {0xD2, 0xD2, {0, G2, O_RM, O_CL, 0, T_MODRM | T_BYTE | T_GROUP}},
    {0xD3, 0xD3, {0, G2, O_RM, O_CL, 0, T_MODRM | T_GROUP}},
    {0xD4, 0xD4, {KVM_OP_BCD, KVM_BCD_AAM, 0, O_IMMU8, 0, T_NO64}},
    {0xD5, 0xD5, {KVM_OP_BCD, KVM_BCD_AAD, 0, O_IMMU8, 0, T_NO64}},
    {0xD6, 0xD6, {KVM_OP_BCD, KVM_BCD_SALC, 0, 0, 0, T_NO64}},
    {0xD7, 0xD7, {KVM_OP_MOV, 0, O_ACC, O_XLAT, 0, T_BYTE | T_MOV}},
    {0xD9, 0xD9, {0, 0, 0, 0, 0, T_MODRM | T_ESC}},
    {0xDB, 0xDB, {0, 0, 0, 0, 0, T_MODRM | T_ESC}},
    {0xDD, 0xDD, {0, 0, 0, 0, 0, T_MODRM | T_ESC}},
    {0xE0, 0xE0, {KVM_OP_LOOP, 0, 0, O_IMM8, 0, T_NEAR}},
    {0xE1, 0xE1, {KVM_OP_LOOP, 1, 0, O_IMM8, 0, T_NEAR}},
    {0xE2, 0xE2, {KVM_OP_LOOP, 2, 0, O_IMM8, 0, T_NEAR}},
    {0xE3, 0xE3, {KVM_OP_LOOP, 3, 0, O_IMM8, 0, T_NEAR}},
    {0xE8, 0xE8, {KVM_OP_CALL, 0, 0, O_IMM, 0, T_NEAR}},
    {0xE9, 0xE9, {KVM_OP_JMP, 0, 0, O_IMM, 0, T_NEAR}},
    {0xEA, 0xEA, {KVM_OP_JMP_FAR, 0, 0, O_FARIMM, 0, T_NO64}},
    {0xEB, 0xEB, {KVM_OP_JMP, 0, 0, O_IMM8, 0, T_NEAR}},
    {0xF5, 0xF5, {KVM_OP_FLAG, KVM_FLAG_CMC, 0, 0, 0, 0}},
    {0xF6, 0xF6, {0, G3, O_RM, 0, 0, T_MODRM | T_BYTE | T_GROUP}},
    {0xF7, 0xF7, {0, G3, O_RM, 0, 0, T_MODRM | T_GROUP}},
    {0xF8, 0xF8, {KVM_OP_FLAG, KVM_FLAG_CLC, 0, 0, 0, 0}},
    {0xF9, 0xF9, {KVM_OP_FLAG, KVM_FLAG_STC, 0, 0, 0, 0}},
    {0xFA, 0xFA, {KVM_OP_FLAG, KVM_FLAG_CLI, 0, 0, 0, 0}},
    {0xFB, 0xFB, {KVM_OP_FLAG, KVM_FLAG_STI, 0, 0, 0, 0}},
    {0xFC, 0xFC, {KVM_OP_FLAG, KVM_FLAG_CLD, 0, 0, 0, 0}},
    {0xFD, 0xFD, {KVM_OP_FLAG, KVM_FLAG_STD, 0, 0, 0, 0}},
    {0xFE, 0xFE, {0, G4, O_RM, 0, 0, T_MODRM | T_BYTE | T_GROUP}},
    {0xFF, 0xFF, {0, G5, O_RM, 0, 0, T_MODRM | T_GROUP}},
};

/* The opcodes after 0x0F, in order. */
static const struct row two_byte[] = {
    {0x00, 0x00, {0, G6, O_RM, 0, 0, T_MODRM | T_GROUP}},
    {0x01, 0x01, {0, G7, O_RM, 0, 0, T_MODRM | T_GROUP}},
    {0x05, 0x05, {KVM_OP_SYSCALL, 0, 0, 0, 0, 0}},
    {0x0D, 0x0D, {KVM_OP_NOP, 0, 0, O_RM, 0, T_MODRM | T_NOACC}},
    {0x10, 0x11, {KVM_OP_MOV, 0, 0, 0, 0, T_MODRM | T_MOV | T_SSE}},
    {0x18, 0x19, {KVM_OP_NOP, 0, 0, O_RM, 0, T_MODRM | T_NOACC}},
    {0x1C, 0x1F, {KVM_OP_NOP, 0, 0, O_RM, 0, T_MODRM | T_NOACC}},
    {0x28, 0x29, {KVM_OP_MOV, 0, 0, 0, 0, T_MODRM | T_MOV | T_SSE}},
    {0x2B, 0x2B, {KVM_OP_MOV, 0, 0, 0, 0, T_MODRM | T_MOV | T_SSE}},
    {0x34, 0x34, {KVM_OP_SYSENTER, 0, 0, 0, 0, 0}},
    {0x35, 0x35, {KVM_OP_SYSEXIT, 0, 0, 0, 0, 0}},
    {0x40, 0x4F, {KVM_OP_CMOV, 0, O_REG, O_RM, 0, T_MODRM | T_CC}},
    {0x6F, 0x6F, {KVM_OP_MOV, 0, 0, 0, 0, T_MODRM | T_MOV | T_SSE}},
    {0x7F, 0x7F, {KVM_OP_MOV, 0, 0, 0, 0, T_MODRM | T_MOV | T_SSE}},
    {0x80, 0x8F, {KVM_OP_JCC, 0, 0, O_IMM, 0, T_NEAR | T_CC}},
    {0x90, 0x9F, {KVM_OP_SETCC, 0, O_RM, 0, 0, T_MODRM | T_BYTE | T_MOV | T_CC}},
    {0xA0, 0xA0, {KVM_OP_PUSH_SREG, KVM_SEG_FS, 0, 0, 0, T_STACK}},
    {0xA1, 0xA1, {KVM_OP_POP_SREG, KVM_SEG_FS, 0, 0, 0, T_STACK}},
    {0xA3, 0xA3, {KVM_OP_BT, KVM_BT_BT, O_RM, O_REG, 0, T_MODRM | T_NOWRITE}},
    {0xA4, 0xA4, {KVM_OP_DSHIFT, 0, O_RM, O_REG, O_IMMU8, T_MODRM}},
    {0xA5, 0xA5, {KVM_OP_DSHIFT, 0, O_RM, O_REG, O_CL, T_MODRM}},
    {0xA8, 0xA8, {KVM_OP_PUSH_SREG, KVM_SEG_GS, 0, 0, 0, T_STACK}},
    {0xA9, 0xA9, {KVM_OP_POP_SREG, KVM_SEG_GS, 0, 0, 0, T_STACK}},
    {0xAB, 0xAB, {KVM_OP_BT, KVM_BT_BTS, O_RM, O_REG, 0, T_MODRM | T_LOCK}},
    {0xAC, 0xAC, {KVM_OP_DSHIFT, 1, O_RM, O_REG, O_IMMU8, T_MODRM}},
    {0xAD, 0xAD, {KVM_OP_DSHIFT, 1, O_RM, O_REG, O_CL, T_MODRM}},
    {0xAE, 0xAE, {0, G15, 0, 0, 0, T_MODRM | T_GROUP}},
    {0xAF, 0xAF, {KVM_OP_IMUL, 0, O_REG, O_RM, 0, T_MODRM}},
    {0xB0, 0xB0, {KVM_OP_CMPXCHG, 0, O_RM, O_REG, 0, T_MODRM | T_BYTE | T_LOCK}},
    {0xB1, 0xB1, {KVM_OP_CMPXCHG, 0, O_RM, O_REG, 0, T_MODRM | T_LOCK}},
    {0xB2, 0xB2, {KVM_OP_LSEG, KVM_SEG_SS, O_REG, O_FARM, 0, T_MODRM | T_MOV}},
    {0xB3, 0xB3, {KVM_OP_BT, KVM_BT_BTR, O_RM, O_REG, 0, T_MODRM | T_LOCK}},
    {0xB4, 0xB4, {KVM_OP_LSEG, KVM_SEG_FS, O_REG, O_FARM, 0, T_MODRM | T_MOV}},
    {0xB5, 0xB5, {KVM_OP_LSEG, KVM_SEG_GS, O_REG, O_FARM, 0, T_MODRM | T_MOV}},
    {0xB6, 0xB6, {KVM_OP_MOV, KVM_MOV_ZX, O_REG, O_RM8, 0, T_MODRM | T_MOV}},
    {0xB7, 0xB7, {KVM_OP_MOV, KVM_MOV_ZX, O_REG, O_RM16, 0, T_MODRM | T_MOV}},
    {0xBA, 0xBA, {0, G8, O_RM, O_IMMU8, 0, T_MODRM | T_GROUP}},
    {0xBB, 0xBB, {KVM_OP_BT, KVM_BT_BTC, O_RM, O_REG, 0, T_MODRM | T_LOCK}},
    {0xBC, 0xBC, {KVM_OP_BSCAN, 0, O_REG, O_RM, 0, T_MODRM}},
    {0xBD, 0xBD, {KVM_OP_BSCAN, 1, O_REG, O_RM, 0, T_MODRM}},
    {0xBE, 0xBE, {KVM_OP_MOV, KVM_MOV_SX, O_REG, O_RM8, 0, T_MODRM | T_MOV}},
    {0xBF, 0xBF, {KVM_OP_MOV, KVM_MOV_SX, O_REG, O_RM16, 0, T_MODRM | T_MOV}},
    {0xC0, 0xC0, {KVM_OP_XADD, 0, O_RM, O_REG, 0, T_MODRM | T_BYTE | T_LOCK}},
    {0xC1, 0xC1, {KVM_OP_XADD, 0, O_RM, O_REG, 0, T_MODRM | T_LOCK}},
    {0xC3, 0xC3, {KVM_OP_MOV, 0, O_M, O_REG, 0, T_MODRM | T_MOV | T_NO16}},
    {0xC7, 0xC7, {0, G9, 0, 0, 0, T_MODRM | T_GROUP}},
    {0xC8, 0xCF, {KVM_OP_BSWAP, 0, O_OPREG, 0, 0, 0}},
    {0xE7, 0xE7, {KVM_OP_MOV, 0, 0, 0, 0, T_MODRM | T_MOV | T_SSE}},
};

/* The opcodes after 0x0F 0x38: MOVBE's two. */
static const struct row three_byte[] = {
    {0xF0, 0xF0, {KVM_OP_MOVBE, 0, O_REG, O_M, 0, T_MODRM | T_MOV}},
    {0xF1, 0xF1, {KVM_OP_MOVBE, 0, O_M, O_REG, 0, T_MODRM | T_MOV}},
};

/* The members of each group, by ModRM's reg field. A member with no
 * operation is outside the set, or #UD with T_UD. */
static const struct opc groups[13][8] = {
    /* G1: 0x80 to 0x83. */
    {{KVM_OP_ALU, KVM_ALU_ADD, O_BASE, O_BASE, 0, T_LOCK},
     {KVM_OP_ALU, KVM_ALU_OR, O_BASE, O_BASE, 0, T_LOCK},
     {KVM_OP_ALU, KVM_ALU_ADC, O_BASE, O_BASE, 0, T_LOCK},
     {KVM_OP_ALU, KVM_ALU_SBB, O_BASE, O_BASE, 0, T_LOCK},
     {KVM_OP_ALU, KVM_ALU_AND, O_BASE, O_BASE, 0, T_LOCK},
     {KVM_OP_ALU, KVM_ALU_SUB, O_BASE, O_BASE, 0, T_LOCK},
     {KVM_OP_ALU, KVM_ALU_XOR, O_BASE, O_BASE, 0, T_LOCK},
     {KVM_OP_ALU, KVM_ALU_CMP, O_BASE, O_BASE, 0, T_NOWRITE}},
    /* G1A: 0x8F. */
    {{KVM_OP_POP, 0, O_RM, 0, 0, T_STACK | T_MOV},
     {0, 0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0, 0}},
    /* G2: 0xC0, 0xC1 and 0xD0 to 0xD3. */
    {{KVM_OP_SHIFT, KVM_SHIFT_ROL, O_BASE, O_BASE, 0, 0},
     {KVM_OP_SHIFT, KVM_SHIFT_ROR, O_BASE, O_BASE, 0, 0},
     {KVM_OP_SHIFT, KVM_SHIFT_RCL, O_BASE, O_BASE, 0, 0},
     {KVM_OP_SHIFT, KVM_SHIFT_RCR, O_BASE, O_BASE, 0, 0},
     {KVM_OP_SHIFT, KVM_SHIFT_SHL, O_BASE, O_BASE, 0, 0},
     {KVM_OP_SHIFT, KVM_SHIFT_SHR, O_BASE, O_BASE, 0, 0},
     {KVM_OP_SHIFT, KVM_SHIFT_SAL, O_BASE, O_BASE, 0, 0},
     {KVM_OP_SHIFT, KVM_SHIFT_SAR, O_BASE, O_BASE, 0, 0}},
    /* G3: 0xF6 and 0xF7. */
    {{KVM_OP_ALU, KVM_ALU_TEST, O_RM, O_IMM, 0, T_NOWRITE},
     {KVM_OP_ALU, KVM_ALU_TEST, O_RM, O_IMM, 0, T_NOWRITE},
     {KVM_OP_UNARY, KVM_ALU_NOT, O_RM, 0, 0, T_LOCK},
     {KVM_OP_UNARY, KVM_ALU_NEG, O_RM, 0, 0, T_LOCK},
     {KVM_OP_MULDIV, KVM_MD_MUL, 0, O_RM, 0, 0},
     {KVM_OP_MULDIV, KVM_MD_IMUL, 0, O_RM, 0, 0},
     {KVM_OP_MULDIV, KVM_MD_DIV, 0, O_RM, 0, 0},
     {KVM_OP_MULDIV, KVM_MD_IDIV, 0, O_RM, 0, 0}},
    /* G4: 0xFE. */
    {{KVM_OP_UNARY, KVM_ALU_INC, O_RM, 0, 0, T_LOCK},
     {KVM_OP_UNARY, KVM_ALU_DEC, O_RM, 0, 0, T_LOCK},
     {0, 0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0, 0}},
    /* G5: 0xFF. */
    {{KVM_OP_UNARY, KVM_ALU_INC, O_RM, 0, 0, T_LOCK},
     {KVM_OP_UNARY, KVM_ALU_DEC, O_RM, 0, 0, T_LOCK},
     {KVM_OP_CALL_ABS, 0, 0, O_RM, 0, T_NEAR},
     {KVM_OP_CALL_FAR, 0, 0, O_FARM, 0, 0},
     {KVM_OP_JMP_ABS, 0, 0, O_RM, 0, T_NEAR},
     {KVM_OP_JMP_FAR, 0, 0, O_FARM, 0, 0},
     {KVM_OP_PUSH, 0, 0, O_RM, 0, T_STACK},
     {0, 0, 0, 0, 0, T_UD}},
    /* G6: 0x0F 0x00. */
    {{KVM_OP_STORE_SEG, KVM_SEG_LDTR, O_RM, 0, 0, T_MOV},
     {KVM_OP_STORE_SEG, KVM_SEG_TR, O_RM, 0, 0, T_MOV},
     {KVM_OP_LOAD_SEG, KVM_SEG_LDTR, 0, O_RM16, 0, 0},
     {KVM_OP_LOAD_SEG, KVM_SEG_TR, 0, O_RM16, 0, 0},
     {0, 0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0, 0}},
    /* G7: 0x0F 0x01 with a memory operand. */
    {{KVM_OP_STORE_DT, 0, O_M, 0, 0, T_MOV | T_NOACC},
     {KVM_OP_STORE_DT, 1, O_M, 0, 0, T_MOV | T_NOACC},
     {KVM_OP_LOAD_DT, 0, 0, O_M, 0, T_NOACC},
     {KVM_OP_LOAD_DT, 1, 0, O_M, 0, T_NOACC},
     {KVM_OP_SMSW, 0, O_RM, 0, 0, T_MOV},
     {0, 0, 0, 0, 0, 0},
     {KVM_OP_LMSW, 0, 0, O_RM16, 0, 0},
     {0, 0, 0, 0, 0, 0}},
    /* G7R: 0x0F 0x01 with a register operand. */
    {{0, 0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0, 0},
     {KVM_OP_SMSW, 0, O_RM, 0, 0, T_MOV},
     {0, 0, 0, 0, 0, 0},
     {KVM_OP_LMSW, 0, 0, O_RM16, 0, 0},
     {0, 0, 0, 0, 0, 0}},
    /* G8: 0x0F 0xBA. */
    {{0, 0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0, 0},
     {KVM_OP_BT, KVM_BT_BT, O_BASE, O_BASE, 0, T_NOWRITE},
     {KVM_OP_BT, KVM_BT_BTS, O_BASE, O_BASE, 0, T_LOCK},
     {KVM_OP_BT, KVM_BT_BTR, O_BASE, O_BASE, 0, T_LOCK},
     {KVM_OP_BT, KVM_BT_BTC, O_BASE, O_BASE, 0, T_LOCK}},
    /* G9: 0x0F 0xC7. */
    {{0, 0, 0, 0, 0, 0},
     {KVM_OP_CMPXCHG8B, 0, O_M, 0, 0, T_LOCK},
     {0, 0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0, 0}},
    /* G11: 0xC6 and 0xC7. */
    {{KVM_OP_MOV, 0, O_BASE, O_BASE, 0, T_MOV},
     {0, 0, 0, 0, 0, T_UD},
     {0, 0, 0, 0, 0, T_UD},
     {0, 0, 0, 0, 0, T_UD},
     {0, 0, 0, 0, 0, T_UD},
     {0, 0, 0, 0, 0, T_UD},
     {0, 0, 0, 0, 0, T_UD},
     {0, 0, 0, 0, 0, T_UD}},
    /* G15: 0x0F 0xAE. */
    {{KVM_OP_FXSAVE, 0, O_M, 0, 0, T_MOV | T_NOACC | T_ALIGN},
     {KVM_OP_FXRSTOR, 0, 0, O_M, 0, T_NOACC | T_ALIGN},
     {0, 0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0, 0},
     {KVM_OP_NOP, 0, 0, O_M, 0, T_BYTE}},
};

/* VMCALL, 0x0F 0x01 0xC1, and VMMCALL, 0x0F 0x01 0xD9, which group 7's
 * register forms tell apart by the whole ModRM byte. */
static const struct opc hypercalls[2] = {
    {KVM_OP_HYPERCALL, 0, 0, 0, 0, 0},
    {KVM_OP_HYPERCALL, 1, 0, 0, 0, 0},
};

/* RDPID, 0xF3 0x0F 0xC7 /7 with a register, group 9's one register
 * form: 8 bytes in 64-bit mode and 4 outside it. */
static const struct opc rdpid = {KVM_OP_RDPID, 0, O_RM, 0, 0, T_MOV | T_NO16 | T_NEAR};

/*
 * The decoder's state while it reads one instruction: the bytes it may
 * read, how many it has, and what the prefixes and the ModRM byte said.
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
    int opsize; /* a 0x66 prefix */
    uint32_t osize;
    uint32_t asize;
    uint32_t opcode; /* the opcode's last byte */
    int modrm;       /* the ModRM byte, or -1 */
    int mod, regf, rm;
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
 * Make `o` the immediate of `k` bytes that comes next, an operand of
 * `size` bytes, sign-extended with `sign`: 0 or -1.
 */
static int imm(struct dec *d, struct kvm_operand *o, uint32_t k, uint32_t size, int sign)
{
    uint64_t v;

    if (fetch(d, k, &v) != 0) {
        return -1;
    }
    o->kind = KVM_OPND_IMM;
    o->size = (uint8_t)size;
    o->imm = sign ? sext(v, k) : v;
    return 0;
}

/** Make `o` the general register `r`, 0 to 15, of `size` bytes: AH, CH, DH
 * or BH for a byte register 4 to 7 with no REX. */
static void set_reg(const struct dec *d, struct kvm_operand *o, int r, uint32_t size)
{
    o->kind = KVM_OPND_REG;
    o->size = (uint8_t)size;
    if (size == 1 && d->rex == 0 && r >= 4 && r < 8) {
        o->reg = (uint8_t)(r - 4);
        o->high8 = 1;
    } else {
        o->reg = (uint8_t)r;
    }
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
 * Read a ModRM byte into `d` and, when it names memory, the SIB byte and
 * displacement after it, and set the memory operand's address in `out`:
 * 0, or -1 when the bytes end.
 */
static int modrm(struct dec *d, struct kvm_insn *out)
{
    uint64_t b, disp;
    uint32_t dsize;

    if (fetch(d, 1, &b) != 0) {
        return -1;
    }
    d->modrm = (int)b;
    d->mod = (int)(b >> 6);
    d->regf = (int)(b >> 3) & 7;
    d->rm = (int)b & 7;
    if (d->mod == 3) {
        return 0;
    }
    if (d->asize == 2) {
        dsize = addr16(out, d->mod, d->rm);
    } else if (addr32(d, out, d->mod, d->rm, &dsize) != 0) {
        return -1;
    }
    if (dsize != 0) {
        if (fetch(d, dsize, &disp) != 0) {
            return -1;
        }
        out->disp = sext(disp, dsize);
    }
    /* rBP and rSP as the base address the stack segment; r12, r13 do not. */
    if (d->seg < 0 && (out->base == 4 || out->base == 5)) {
        out->seg = KVM_SEG_SS;
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

/** The row of `rows`, `n` of them, that has the opcode `op`, or null. */
static const struct opc *find(const struct row *rows, uint32_t n, uint32_t op)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (op >= rows[i].first && op <= rows[i].last) {
            return &rows[i].e;
        }
    }
    return 0;
}

/**
 * Read the opcode that begins with the byte `op` and copy its table entry
 * to `e`, with ADD to CMP's and a condition's `sub` from the opcode: 0, or
 * -1 for an opcode outside the tables.
 */
static int lookup(struct dec *d, uint64_t op, struct opc *e)
{
    const struct opc *t;
    uint64_t b;

    d->opcode = (uint32_t)op;
    if (op < 0x40 && (op & 7) < 6) {
        *e = alu_forms[op & 7];
        e->sub = (uint8_t)(op >> 3);
        if (e->sub == KVM_ALU_CMP) {
            e->flags = (e->flags & ~T_LOCK) | T_NOWRITE;
        }
        return 0;
    }
    if (op != 0x0F) {
        t = find(one_byte, sizeof(one_byte) / sizeof(one_byte[0]), (uint32_t)op);
    } else {
        if (fetch(d, 1, &b) != 0) {
            return -1;
        }
        if (b != 0x38) {
            t = find(two_byte, sizeof(two_byte) / sizeof(two_byte[0]), (uint32_t)b);
        } else {
            /* 0xF2 before MOVBE's opcodes makes them CRC32. */
            if (fetch(d, 1, &b) != 0 || d->rep) {
                return -1;
            }
            t = find(three_byte, sizeof(three_byte) / sizeof(three_byte[0]), (uint32_t)b);
        }
        d->opcode = 0x0F00u | (uint32_t)b;
    }
    if (!t) {
        return -1;
    }
    *e = *t;
    if (e->flags & T_CC) {
        e->sub = (uint8_t)(d->opcode & 15u);
    }
    return 0;
}

/**
 * Replace the group's row `e` by its member that ModRM's reg field names,
 * which takes the row's operands where it says O_BASE and keeps the row's
 * operand size: 0, or -1 for a member outside the set.
 */
static int member(const struct dec *d, struct opc *e)
{
    uint32_t g = e->sub;
    const struct opc *m;
    struct opc row = *e;

    if (g == G7 && d->mod == 3) {
        g = G7R;
    }
    m = &groups[g][d->regf];
    if (g == G7R && (d->modrm == 0xC1 || d->modrm == 0xD9)) {
        m = &hypercalls[d->modrm == 0xD9];
    }
    if (g == G9 && d->mod == 3) {
        if (d->regf != 7 || d->rep != 0xF3 || d->opsize) {
            return -1;
        }
        m = &rdpid;
    }
    /* Group 15's /7 is CLFLUSH, or CLFLUSHOPT under 0x66; Linux's
     * pfx_0f_ae_7 refuses it under 0xF2 or 0xF3. */
    if (g == G15 && d->regf == 7 && d->rep) {
        return -1;
    }
    if (m->op == 0 && !(m->flags & T_UD)) {
        return -1;
    }
    *e = *m;
    e->flags |= row.flags & (T_MODRM | T_BYTE | T_NO64);
    if (e->dst == O_BASE) {
        e->dst = row.dst;
    }
    if (e->src == O_BASE) {
        e->src = row.src;
    }
    return 0;
}

/**
 * Make `e` the x87 instruction of the escape opcode with `d`'s ModRM byte:
 * FNSTCW, FNSTSW or FNINIT. 0, or -1 for any other.
 */
static int escape(const struct dec *d, struct opc *e)
{
    if (d->opcode == 0xDB) {
        if (d->modrm != 0xE3) {
            return -1;
        }
        e->op = KVM_OP_FNINIT;
        return 0;
    }
    if (d->mod == 3 || d->regf != 7) {
        return -1;
    }
    e->op = d->opcode == 0xD9 ? KVM_OP_FNSTCW : KVM_OP_FNSTSW;
    e->dst = O_RM16;
    e->flags |= T_MOV;
    return 0;
}

/**
 * Make `e` the SSE or MMX move its opcode is under `d`'s prefix: MOVUPS and
 * MOVUPD, MOVAPS and MOVAPD, MOVNTPS and MOVNTPD, MOVNTDQ, and MOVQ, MOVDQA
 * and MOVDQU by the prefix. 0, or -1 for a form Linux's emulator lacks.
 */
static int sse(const struct dec *d, struct opc *e)
{
    uint32_t op = d->opcode & 0xFFu;
    int pfx = d->opsize ? 0x66 : d->rep;
    int store = op == 0x11 || op == 0x29 || op == 0x2B || op == 0x7F || op == 0xE7;
    int mmx = 0;

    if ((d->opsize && d->rep) || pfx == 0xF2) {
        return -1;
    }
    if (op == 0x6F || op == 0x7F) {
        mmx = pfx == 0;
        if (pfx == 0x66) {
            e->flags |= T_ALIGN;
        }
    } else if (pfx == 0xF3 || (op == 0xE7 && pfx != 0x66)) {
        return -1;
    } else if (op != 0x10 && op != 0x11) {
        e->flags |= T_ALIGN;
    }
    if ((op == 0x2B || op == 0xE7) && d->mod == 3) {
        return -1;
    }
    e->dst = (uint8_t)(mmx ? O_MM : O_XMM);
    e->src = (uint8_t)(mmx ? O_MMRM : O_XMMRM);
    if (store) {
        e->src = e->dst;
        e->dst = (uint8_t)(mmx ? O_MMRM : O_XMMRM);
    }
    return 0;
}

/** The operand size of `e` under `d`'s prefixes and mode. */
static uint32_t operand_size(const struct dec *d, const struct opc *e)
{
    uint32_t size = d->osize;

    if (e->flags & T_BYTE) {
        return 1;
    }
    if ((e->flags & T_NO16) && size == 2) {
        size = 4;
    }
    if (d->mode == 64 && (((e->flags & T_STACK) && size == 4) || (e->flags & T_NEAR))) {
        size = 8;
    }
    return size;
}

/**
 * Make `o` the operand of kind `kind`, one of those ModRM's bytes or the
 * opcode name, of `size` bytes: 0, -1 for a register where memory alone is
 * taken, or 1 when `kind` is not of these.
 */
static int operand_rm(const struct dec *d, int kind, uint32_t size, struct kvm_operand *o)
{
    int rm = d->rm | (d->rex & REX_B ? 8 : 0), reg = d->regf | (d->rex & REX_R ? 8 : 0);

    switch (kind) {
    case O_REG:
        set_reg(d, o, reg, size);
        return 0;
    case O_OPREG:
        set_reg(d, o, (int)(d->opcode & 7u) | (d->rex & REX_B ? 8 : 0), size);
        return 0;
    case O_SREG:
        o->kind = KVM_OPND_SREG;
        o->size = 2;
        o->reg = (uint8_t)d->regf;
        return 0;
    case O_XMM:
    case O_MM:
        o->kind = (uint8_t)(kind == O_XMM ? KVM_OPND_XMM : KVM_OPND_MM);
        o->size = (uint8_t)size;
        o->reg = (uint8_t)(kind == O_XMM ? reg : d->regf);
        return 0;
    case O_RM8:
        size = 1;
        break;
    case O_RM16:
        size = 2;
        break;
    case O_RM32:
        size = 4;
        break;
    case O_FARM:
        size += 2;
        break;
    case O_RM:
    case O_M:
    case O_XMMRM:
    case O_MMRM:
        break;
    default:
        return 1;
    }
    if (d->mod != 3) {
        o->kind = KVM_OPND_MEM;
        o->size = (uint8_t)size;
    } else if (kind == O_M || kind == O_FARM) {
        return -1;
    } else if (kind == O_XMMRM || kind == O_MMRM) {
        o->kind = (uint8_t)(kind == O_XMMRM ? KVM_OPND_XMM : KVM_OPND_MM);
        o->size = (uint8_t)size;
        o->reg = (uint8_t)(kind == O_XMMRM ? rm : d->rm);
    } else {
        set_reg(d, o, rm, size);
    }
    return 0;
}

/**
 * Make `o` the operand of kind `kind` of `out`, of `size` bytes, reading
 * the immediate or the offset it has: 0 or -1.
 */
static int operand(struct dec *d, struct kvm_insn *out, int kind, struct kvm_operand *o)
{
    uint32_t size = out->size;
    int r = operand_rm(d, kind, size, o);

    if (r <= 0) {
        return r;
    }
    switch (kind) {
    case O_ACC:
        o->kind = KVM_OPND_REG;
        o->size = (uint8_t)size;
        return 0;
    case O_CL:
        o->kind = KVM_OPND_REG;
        o->size = 1;
        o->reg = 1;
        return 0;
    case O_ONE:
        o->kind = KVM_OPND_IMM;
        o->size = 1;
        o->imm = 1;
        return 0;
    case O_IMM:
        return imm(d, o, size == 8 ? 4 : size, size, 1);
    case O_IMM8:
        return imm(d, o, 1, size, 1);
    case O_IMMU8:
        return imm(d, o, 1, 1, 0);
    case O_IMMU16:
        return imm(d, o, 2, 2, 0);
    case O_IMM64:
        return imm(d, o, size, size, 1);
    case O_FARIMM:
        return imm(d, o, size, size, 0) != 0 ? -1 : imm(d, &out->src2, 2, 2, 0);
    case O_MOFFS:
        o->kind = KVM_OPND_MEM;
        o->size = (uint8_t)size;
        return fetch(d, d->asize, &out->disp);
    case O_SI:
    case O_DI:
    case O_XLAT:
        o->kind = KVM_OPND_MEM;
        o->size = (uint8_t)size;
        o->mem = (uint8_t)(kind == O_SI ? KVM_MEM_SI : kind == O_DI ? KVM_MEM_DI : KVM_MEM_XLAT);
        return 0;
    default:
        return 0;
    }
}

/**
 * Finish the operands of the few instructions whose sizes the tables do
 * not give: a selector or the machine status word stored to memory is two
 * bytes, and CMPXCHG8B's operand eight, with REX.W's sixteen refused as
 * Linux refuses it. A segment register the processor lacks, and CS as a
 * move's destination, are #UD. 0 or -1.
 */
static int finish(const struct dec *d, struct kvm_insn *out)
{
    if ((out->dst.kind == KVM_OPND_SREG &&
         (out->dst.reg == KVM_SEG_CS || out->dst.reg > KVM_SEG_GS)) ||
        (out->src.kind == KVM_OPND_SREG && out->src.reg > KVM_SEG_GS)) {
        out->flags |= KVM_INSN_UD;
    }
    if ((out->op == KVM_OP_STORE_SEG || out->op == KVM_OP_SMSW) && out->dst.kind == KVM_OPND_MEM) {
        out->dst.size = 2;
    }
    if (out->op == KVM_OP_CMPXCHG8B) {
        if (d->rex & REX_W) {
            return -1;
        }
        out->size = 8;
        out->dst.size = 8;
    }
    return 0;
}

/** Set `out`'s flags from the table's `e` and `d`'s prefixes: a LOCK the
 * instruction does not take, or takes only on memory, is #UD. */
static void set_flags(const struct dec *d, const struct opc *e, struct kvm_insn *out)
{
    if (e->flags & T_MOV) {
        out->flags |= KVM_INSN_MOV;
    }
    if (e->flags & T_NOWRITE) {
        out->flags |= KVM_INSN_NOWRITE;
    }
    if (e->flags & T_STRING) {
        out->flags |= KVM_INSN_STRING;
    }
    if (e->flags & T_NOACC) {
        out->flags |= KVM_INSN_NOACCESS;
    }
    if (e->flags & T_ALIGN) {
        out->flags |= KVM_INSN_ALIGNED;
    }
    if (!(e->flags & T_SSE) && d->rep) {
        out->flags |= d->rep == 0xF3 ? KVM_INSN_REP : KVM_INSN_REPNE;
    }
    if (d->lock) {
        out->flags |= KVM_INSN_LOCK;
        if (!(e->flags & T_LOCK) || out->dst.kind != KVM_OPND_MEM) {
            out->flags |= KVM_INSN_UD;
        }
    }
}

/**
 * Decode what follows the prefixes, whose opcode begins with `op`, into
 * `out`: 0 or -1. An encoding that is #UD ends at the byte that made it so.
 */
static int decode(struct dec *d, uint64_t op, struct kvm_insn *out)
{
    struct opc e;

    if (lookup(d, op, &e) != 0) {
        return -1;
    }
    if ((e.flags & T_MODRM) && modrm(d, out) != 0) {
        return -1;
    }
    /* 0xC4 and 0xC5 begin a VEX prefix in 64-bit mode and before a
     * register's ModRM; else they are LES and LDS. */
    if ((d->opcode == 0xC4 || d->opcode == 0xC5) && (d->mode == 64 || d->mod == 3)) {
        return -1;
    }
    if ((e.flags & T_GROUP) && member(d, &e) != 0) {
        return -1;
    }
    if (((e.flags & T_ESC) && escape(d, &e) != 0) || ((e.flags & T_SSE) && sse(d, &e) != 0) ||
        ((e.flags & T_ONLY64) && d->mode != 64)) {
        return -1;
    }
    if ((e.flags & T_UD) || ((e.flags & T_NO64) && d->mode == 64)) {
        out->op = KVM_OP_NOP;
        out->flags |= KVM_INSN_UD;
        return 0;
    }
    out->op = e.op;
    out->sub = e.sub;
    out->size = operand_size(d, &e);
    if (e.flags & T_SSE) {
        out->size = e.dst == O_MM || e.src == O_MM ? 8 : 16;
    }
    if (operand(d, out, e.src, &out->src) != 0 || operand(d, out, e.src2, &out->src2) != 0 ||
        operand(d, out, e.dst, &out->dst) != 0) {
        return -1;
    }
    set_flags(d, &e, out);
    return finish(d, out);
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
    d.modrm = -1;
    out->base = KVM_REG_NONE;
    out->index = KVM_REG_NONE;
    out->scale = 1;
    out->seg = KVM_SEG_DS;
    if (prefixes(&d, &op) != 0) {
        return -1;
    }
    out->addr_size = d.asize;
    if (d.seg >= 0) {
        out->seg = (uint32_t)d.seg;
    }
    if (decode(&d, op, out) != 0) {
        return -1;
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
