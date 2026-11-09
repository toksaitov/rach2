/**
 * Check the decoder of guest instructions that touch MMIO
 * (rach2/sys/kvm/decode.c) against a table of encodings.
 *
 * Each row is a mode, an instruction's bytes, its length and the decoded
 * access written out as text: the operation, the memory operand's size
 * and the register's when it differs, the register or immediate, the
 * segment and address, the address size, whether memory is read, written
 * or both, and the prefixes that count. The row's bytes are decoded with
 * 0xCC bytes after them, which the decoder must not take, then cut to
 * their length, which decodes the same, and to one byte less, which fails.
 * A row with no text is an instruction the decoder refuses, decoded from
 * its bytes alone; its internal error is then checked against the layout
 * of Linux's emulation_failure.
 */
#include "kvm/decode.c"

extern int printf(const char *format, ...);
extern int snprintf(char *s, unsigned long n, const char *fmt, ...);
extern int strcmp(const char *a, const char *b);

static int failures;

/* One row of the table: `want` null for an instruction that fails. */
struct row {
    int mode;
    const char *hex;
    uint32_t len;
    const char *want;
};

static const struct row rows[] = {
    /* MOV in 64-bit mode: ModRM, SIB, RIP-relative, REX, prefixes. */
    {64, "8b 04 b3", 3, "load m4 ax ds:[bx+si*4] a8 rd"},
    {64, "48 8b 05 10 00 00 00", 7, "load m8 ax ds:[rip+0x10] a8 rd"},
    {64, "48 8b 05 f0 ff ff ff", 7, "load m8 ax ds:[rip-0x10] a8 rd"},
    {64, "88 07", 2, "store m1 ax ds:[di] a8 wr"},
    {64, "88 27", 2, "store m1 ah ds:[di] a8 wr"},
    {64, "40 88 27", 3, "store m1 sp ds:[di] a8 wr"},
    {64, "66 89 0a", 3, "store m2 cx ds:[dx] a8 wr"},
    {64, "44 89 4d f8", 4, "store m4 r9 ss:[bp-0x8] a8 wr"},
    {64, "8b 44 24 f8", 4, "load m4 ax ss:[sp-0x8] a8 rd"},
    {64, "41 8b 44 24 08", 5, "load m4 ax ds:[r12+0x8] a8 rd"},
    {64, "41 8b 45 00", 4, "load m4 ax ds:[r13] a8 rd"},
    {64, "42 8b 04 25 00 10 00 00", 8, "load m4 ax ds:[r12+0x1000] a8 rd"},
    {64, "8b 04 25 00 10 00 00", 7, "load m4 ax ds:[0x1000] a8 rd"},
    {64, "4c 8b 1c c5 00 00 00 00", 8, "load m8 r11 ds:[ax*8] a8 rd"},
    {64, "4f 8b 84 c8 00 01 00 00", 8, "load m8 r8 ds:[r8+r9*8+0x100] a8 rd"},
    {64, "67 8b 03", 3, "load m4 ax ds:[bx] a4 rd"},
    {64, "67 8b 05 10 00 00 00", 7, "load m4 ax ds:[rip+0x10] a4 rd"},
    {64, "64 48 8b 04 25 28 00 00 00", 9, "load m8 ax fs:[0x28] a8 rd"},
    {64, "65 8b 04 25 00 00 00 00", 8, "load m4 ax gs:[0x0] a8 rd"},
    {64, "3e 8b 45 00", 4, "load m4 ax ds:[bp] a8 rd"},
    {64, "48 66 8b 07", 4, "load m2 ax ds:[di] a8 rd"},
    {64, "66 48 8b 07", 4, "load m8 ax ds:[di] a8 rd"},
    {64, "c6 00 ff", 3, "store m1 $-1 ds:[ax] a8 wr"},
    {64, "66 c7 00 34 12", 5, "store m2 $4660 ds:[ax] a8 wr"},
    {64, "c7 80 00 01 00 00 78 56 34 12", 10, "store m4 $305419896 ds:[ax+0x100] a8 wr"},
    {64, "48 c7 40 08 ff ff ff ff", 8, "store m8 $-1 ds:[ax+0x8] a8 wr"},
    {64, "48 a1 88 77 66 55 44 33 22 11", 10, "load m8 ax ds:[0x1122334455667788] a8 rd"},
    {64, "a2 00 10 00 00 00 00 00 00", 9, "store m1 ax ds:[0x1000] a8 wr"},
    {64, "67 a3 00 10 00 00", 6, "store m4 ax ds:[0x1000] a4 wr"},
    {64, "65 a1 00 10 00 00 00 00 00 00", 10, "load m4 ax gs:[0x1000] a8 rd"},
    {64, "2e 26 64 48 c7 84 88 78 56 34 12 ef be ad de", 15,
     "store m8 $-559038737 fs:[ax+cx*4+0x12345678] a8 wr"},

    /* MOVZX, MOVSX, MOVSXD and MOVNTI. */
    {64, "0f b6 06", 3, "load m1 r4 zx ax ds:[si] a8 rd"},
    {64, "66 0f b6 06", 4, "load m1 r2 zx ax ds:[si] a8 rd"},
    {64, "0f b7 06", 3, "load m2 r4 zx ax ds:[si] a8 rd"},
    {64, "48 0f be 06", 4, "load m1 r8 sx ax ds:[si] a8 rd"},
    {64, "48 0f bf 06", 4, "load m2 r8 sx ax ds:[si] a8 rd"},
    {64, "44 0f b6 06", 4, "load m1 r4 zx r8 ds:[si] a8 rd"},
    {64, "48 63 06", 3, "load m4 r8 sx ax ds:[si] a8 rd"},
    {64, "63 06", 2, "load m4 ax ds:[si] a8 rd"},
    {64, "0f c3 07", 3, "store m4 ax ds:[di] a8 wr"},
    {64, "48 0f c3 07", 4, "store m8 ax ds:[di] a8 wr"},

    /* MOVS and STOS. */
    {64, "a4", 1, "movs m1 ds a8 rw"},
    {64, "f3 48 a5", 3, "movs m8 ds a8 rw rep"},
    {64, "66 f3 a5", 3, "movs m2 ds a8 rw rep"},
    {64, "f2 a4", 2, "movs m1 ds a8 rw repne"},
    {64, "67 f3 a4", 3, "movs m1 ds a4 rw rep"},
    {64, "2e a4", 2, "movs m1 cs a8 rw"},
    {64, "f3 aa", 2, "stos m1 ax es a8 wr rep"},
    {64, "26 ab", 2, "stos m4 ax es a8 wr"},
    {64, "64 48 ab", 3, "stos m8 ax es a8 wr"},

    /* The arithmetic and logic group, with a register or an immediate. */
    {64, "00 07", 2, "alu:add m1 ax ds:[di] a8 rw"},
    {64, "01 07", 2, "alu:add m4 ax ds:[di] a8 rw"},
    {64, "03 07", 2, "alu:add m4 ax ds:[di] a8 rd to-reg"},
    {64, "08 27", 2, "alu:or m1 ah ds:[di] a8 rw"},
    {64, "48 09 07", 3, "alu:or m8 ax ds:[di] a8 rw"},
    {64, "11 07", 2, "alu:adc m4 ax ds:[di] a8 rw"},
    {64, "19 07", 2, "alu:sbb m4 ax ds:[di] a8 rw"},
    {64, "20 07", 2, "alu:and m1 ax ds:[di] a8 rw"},
    {64, "23 0f", 2, "alu:and m4 cx ds:[di] a8 rd to-reg"},
    {64, "29 07", 2, "alu:sub m4 ax ds:[di] a8 rw"},
    {64, "31 07", 2, "alu:xor m4 ax ds:[di] a8 rw"},
    {64, "4c 32 07", 3, "alu:xor m1 r8 ds:[di] a8 rd to-reg"},
    {64, "39 07", 2, "alu:cmp m4 ax ds:[di] a8 rd"},
    {64, "3b 07", 2, "alu:cmp m4 ax ds:[di] a8 rd to-reg"},
    {64, "80 0f 01", 3, "alu:or m1 $1 ds:[di] a8 rw"},
    {64, "81 27 ff 00 00 00", 6, "alu:and m4 $255 ds:[di] a8 rw"},
    {64, "66 81 27 ff 00", 5, "alu:and m2 $255 ds:[di] a8 rw"},
    {64, "48 83 2f f0", 4, "alu:sub m8 $-16 ds:[di] a8 rw"},
    {64, "80 3f 00", 3, "alu:cmp m1 $0 ds:[di] a8 rd"},
    {64, "48 81 3f ff ff ff 7f", 7, "alu:cmp m8 $2147483647 ds:[di] a8 rd"},
    {64, "48 81 37 00 00 00 80", 7, "alu:xor m8 $-2147483648 ds:[di] a8 rw"},
    {64, "f0 81 07 01 00 00 00", 7, "alu:add m4 $1 ds:[di] a8 rw lock"},

    /* TEST, XCHG, INC, DEC, NOT and NEG. */
    {64, "85 07", 2, "alu:test m4 ax ds:[di] a8 rd"},
    {64, "84 27", 2, "alu:test m1 ah ds:[di] a8 rd"},
    {64, "f6 07 80", 3, "alu:test m1 $-128 ds:[di] a8 rd"},
    {64, "f6 0f 01", 3, "alu:test m1 $1 ds:[di] a8 rd"},
    {64, "f7 07 00 00 00 80", 6, "alu:test m4 $-2147483648 ds:[di] a8 rd"},
    {64, "66 f7 07 34 12", 5, "alu:test m2 $4660 ds:[di] a8 rd"},
    {64, "48 f7 07 ff ff ff ff", 7, "alu:test m8 $-1 ds:[di] a8 rd"},
    {64, "86 07", 2, "xchg m1 ax ds:[di] a8 rw"},
    {64, "48 87 07", 3, "xchg m8 ax ds:[di] a8 rw"},
    {64, "f0 87 07", 3, "xchg m4 ax ds:[di] a8 rw lock"},
    {64, "f6 17", 2, "unary:not m1 ds:[di] a8 rw"},
    {64, "48 f7 1f", 3, "unary:neg m8 ds:[di] a8 rw"},
    {64, "fe 07", 2, "unary:inc m1 ds:[di] a8 rw"},
    {64, "ff 0f", 2, "unary:dec m4 ds:[di] a8 rw"},
    {64, "f0 ff 07", 3, "unary:inc m4 ds:[di] a8 rw lock"},
    {64, "f0 f7 17", 3, "unary:not m4 ds:[di] a8 rw lock"},

    /* PUSH and POP of memory. */
    {64, "ff 37", 2, "push m8 ds:[di] a8 rd"},
    {64, "66 ff 37", 3, "push m2 ds:[di] a8 rd"},
    {64, "48 ff 37", 3, "push m8 ds:[di] a8 rd"},
    {64, "66 48 ff 37", 4, "push m8 ds:[di] a8 rd"},
    {64, "8f 07", 2, "pop m8 ds:[di] a8 wr"},
    {64, "41 8f 04 24", 4, "pop m8 ds:[r12] a8 wr"},
    {64, "66 8f 07", 3, "pop m2 ds:[di] a8 wr"},

    /* 32-bit mode. */
    {32, "8b 03", 2, "load m4 ax ds:[bx] a4 rd"},
    {32, "8b 05 00 10 00 00", 6, "load m4 ax ds:[0x1000] a4 rd"},
    {32, "66 89 0e", 3, "store m2 cx ds:[si] a4 wr"},
    {32, "88 3e", 2, "store m1 bh ds:[si] a4 wr"},
    {32, "a1 00 10 00 00", 5, "load m4 ax ds:[0x1000] a4 rd"},
    {32, "67 a1 00 10", 4, "load m4 ax ds:[0x1000] a2 rd"},
    {32, "67 8b 07", 3, "load m4 ax ds:[bx] a2 rd"},
    {32, "8b 04 24", 3, "load m4 ax ss:[sp] a4 rd"},
    {32, "0f b7 03", 3, "load m2 r4 zx ax ds:[bx] a4 rd"},
    {32, "0f be 45 fc", 4, "load m1 r4 sx ax ss:[bp-0x4] a4 rd"},
    {32, "c7 05 00 10 00 00 01 00 00 00", 10, "store m4 $1 ds:[0x1000] a4 wr"},
    {32, "82 07 01", 3, "alu:add m1 $1 ds:[di] a4 rw"},
    {32, "f3 a5", 2, "movs m4 ds a4 rw rep"},
    {32, "66 ab", 2, "stos m2 ax es a4 wr"},
    {32, "ff 30", 2, "push m4 ds:[ax] a4 rd"},
    {32, "66 ff 30", 3, "push m2 ds:[ax] a4 rd"},
    {32, "8f 00", 2, "pop m4 ds:[ax] a4 wr"},
    {32, "f0 09 1d 00 10 00 00", 7, "alu:or m4 bx ds:[0x1000] a4 rw lock"},

    /* 16-bit mode: the eight ModRM forms, disp16 and the 0x67 forms. */
    {16, "8b 00", 2, "load m2 ax ds:[bx+si] a2 rd"},
    {16, "8b 01", 2, "load m2 ax ds:[bx+di] a2 rd"},
    {16, "8b 02", 2, "load m2 ax ss:[bp+si] a2 rd"},
    {16, "8b 03", 2, "load m2 ax ss:[bp+di] a2 rd"},
    {16, "8b 04", 2, "load m2 ax ds:[si] a2 rd"},
    {16, "8b 05", 2, "load m2 ax ds:[di] a2 rd"},
    {16, "8b 06 00 10", 4, "load m2 ax ds:[0x1000] a2 rd"},
    {16, "8b 07", 2, "load m2 ax ds:[bx] a2 rd"},
    {16, "8b 46 fe", 3, "load m2 ax ss:[bp-0x2] a2 rd"},
    {16, "8b 87 34 12", 4, "load m2 ax ds:[bx+0x1234] a2 rd"},
    {16, "8b 87 00 80", 4, "load m2 ax ds:[bx-0x8000] a2 rd"},
    {16, "66 8b 07", 3, "load m4 ax ds:[bx] a2 rd"},
    {16, "67 8b 04 b3", 4, "load m2 ax ds:[bx+si*4] a4 rd"},
    {16, "66 67 8b 04 b3", 5, "load m4 ax ds:[bx+si*4] a4 rd"},
    {16, "26 88 07", 3, "store m1 ax es:[bx] a2 wr"},
    {16, "c6 07 12", 3, "store m1 $18 ds:[bx] a2 wr"},
    {16, "c7 07 34 12", 4, "store m2 $4660 ds:[bx] a2 wr"},
    {16, "66 c7 07 78 56 34 12", 7, "store m4 $305419896 ds:[bx] a2 wr"},
    {16, "a1 00 10", 3, "load m2 ax ds:[0x1000] a2 rd"},
    {16, "0f b6 07", 3, "load m1 r2 zx ax ds:[bx] a2 rd"},
    {16, "80 3e 00 10 00", 5, "alu:cmp m1 $0 ds:[0x1000] a2 rd"},
    {16, "83 06 00 10 ff", 5, "alu:add m2 $-1 ds:[0x1000] a2 rw"},
    {16, "a4", 1, "movs m1 ds a2 rw"},
    {16, "f3 a5", 2, "movs m2 ds a2 rw rep"},
    {16, "66 f3 a5", 3, "movs m4 ds a2 rw rep"},
    {16, "ff 36 00 10", 4, "push m2 ds:[0x1000] a2 rd"},
    {16, "66 ff 36 00 10", 5, "push m4 ds:[0x1000] a2 rd"},
    {16, "8f 06 00 10", 4, "pop m2 ds:[0x1000] a2 wr"},

    /* Refused: no memory operand, outside the set, LOCK where the
     * processor refuses it, the bytes ending early, past 15 bytes. */
    {64, "8b c0", 0, 0},
    {64, "90", 0, 0},
    {64, "0f 0b", 0, 0},
    {64, "0f af 07", 0, 0},
    {64, "0f b6 c0", 0, 0},
    {64, "c4 e2 79 18 07", 0, 0},
    {64, "f0 8b 07", 0, 0},
    {64, "f0 39 07", 0, 0},
    {64, "f0 03 07", 0, 0},
    {64, "f0 85 07", 0, 0},
    {64, "f0 a4", 0, 0},
    {64, "f0 ff 37", 0, 0},
    {64, "f0 48 0f c3 07", 0, 0},
    {64, "c6 08 00", 0, 0},
    {64, "c7 c0 01 00 00 00", 0, 0},
    {64, "8f 08", 0, 0},
    {64, "ff 17", 0, 0},
    {64, "ff 27", 0, 0},
    {64, "ff 3f", 0, 0},
    {64, "fe 17", 0, 0},
    {64, "f6 27", 0, 0},
    {64, "f7 3f", 0, 0},
    {64, "66 0f c3 07", 0, 0},
    {64, "f3 0f c3 07", 0, 0},
    {64, "0f c3 c0", 0, 0},
    {64, "82 07 01", 0, 0},
    {64, "06", 0, 0},
    {64, "41", 0, 0},
    {64, "0f", 0, 0},
    {64, "8b", 0, 0},
    {64, "8b 04", 0, 0},
    {64, "8b 87 00", 0, 0},
    {64, "c7 00 01 00", 0, 0},
    {64, "a1 00 10 00 00", 0, 0},
    {64, "2e 2e 26 64 48 c7 84 88 78 56 34 12 ef be ad de", 0, 0},
    {64, "66 66 66 66 66 66 66 66 66 66 66 66 66 66 66 8b 07", 0, 0},
    {32, "48 8b 07", 0, 0},
    {32, "63 07", 0, 0},
    {32, "8b 05 00 10", 0, 0},
    {16, "8b 06 00", 0, 0},
    {8, "8b 07", 0, 0},
};

static const char *const regs[17] = {"ax", "cx",  "dx",  "bx",  "sp",  "bp",  "si",  "di", "r8",
                                     "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rip"};
static const char *const highs[4] = {"ah", "ch", "dh", "bh"};
static const char *const segs[6] = {"es", "cs", "ss", "ds", "fs", "gs"};
static const char *const ops[10] = {"?",    "load", "store", "alu",  "unary",
                                    "xchg", "movs", "stos",  "push", "pop"};
static const char *const alus[13] = {"add", "or",   "adc", "sbb", "and", "sub", "xor",
                                     "cmp", "test", "inc", "dec", "not", "neg"};

/** Append the formatted text to `buf`, whose length so far is `*at`. */
#define PUT(...)                                                                   \
    do {                                                                           \
        int put_n = snprintf(buf + *at, (unsigned long)(size - *at), __VA_ARGS__); \
        if (put_n > 0) {                                                           \
            *at += (uint32_t)put_n;                                                \
        }                                                                          \
        if (*at >= size) {                                                         \
            *at = size - 1;                                                        \
        }                                                                          \
    } while (0)

/** Write the memory operand's address of `in` as `[base+index*scale+disp]`. */
static void put_addr(const struct kvm_insn *in, char *buf, uint32_t size, uint32_t *at)
{
    int any = 0;

    PUT("[");
    if (in->base != KVM_REG_NONE) {
        PUT("%s", in->base >= 0 && in->base <= 16 ? regs[in->base] : "?");
        any = 1;
    }
    if (in->index != KVM_REG_NONE) {
        PUT("%s%s", any ? "+" : "", in->index >= 0 && in->index < 16 ? regs[in->index] : "?");
        if (in->scale != 1) {
            PUT("*%u", in->scale);
        }
        any = 1;
    }
    if (!any) {
        PUT("0x%llx", (unsigned long long)in->disp);
    } else if ((int64_t)in->disp < 0) {
        PUT("-0x%llx", (unsigned long long)(0 - in->disp));
    } else if (in->disp != 0) {
        PUT("+0x%llx", (unsigned long long)in->disp);
    }
    PUT("]");
}

/** Write `in` in the table's text form into `buf` of `size` bytes. */
static void format(const struct kvm_insn *in, char *buf, uint32_t size)
{
    uint32_t n = 0, *at = &n;
    uint32_t rw = in->flags & (KVM_INSN_READ | KVM_INSN_WRITE);

    buf[0] = 0;
    PUT("%s", in->op < 10 ? ops[in->op] : "?");
    if (in->op == KVM_OP_ALU || in->op == KVM_OP_UNARY) {
        PUT(":%s", in->alu < 13 ? alus[in->alu] : "?");
    }
    PUT(" m%u", in->size);
    if (in->reg_size != in->size) {
        PUT(" r%u", in->reg_size);
    }
    if (in->flags & KVM_INSN_ZX) {
        PUT(" zx");
    }
    if (in->flags & KVM_INSN_SX) {
        PUT(" sx");
    }
    if (in->reg != KVM_REG_NONE) {
        if (in->flags & KVM_INSN_HIGH8) {
            PUT(" %s", in->reg >= 0 && in->reg < 4 ? highs[in->reg] : "?");
        } else {
            PUT(" %s", in->reg >= 0 && in->reg < 16 ? regs[in->reg] : "?");
        }
    }
    if (in->flags & KVM_INSN_IMM) {
        PUT(" $%lld", (long long)in->imm);
    }
    PUT(" %s", in->seg < 6 ? segs[in->seg] : "?");
    if (in->op != KVM_OP_MOVS && in->op != KVM_OP_STOS) {
        PUT(":");
        put_addr(in, buf, size, at);
    }
    PUT(" a%u", in->addr_size);
    PUT(" %s", rw == (KVM_INSN_READ | KVM_INSN_WRITE) ? "rw" : rw == KVM_INSN_READ ? "rd" : "wr");
    if (in->flags & KVM_INSN_TO_REG) {
        PUT(" to-reg");
    }
    if (in->flags & KVM_INSN_REP) {
        PUT(" rep");
    }
    if (in->flags & KVM_INSN_REPNE) {
        PUT(" repne");
    }
    if (in->flags & KVM_INSN_LOCK) {
        PUT(" lock");
    }
}

/** Parse the hex bytes of `hex` into `out`: their count. */
static uint32_t parse(const char *hex, uint8_t *out, uint32_t max)
{
    uint32_t n = 0;
    int v, half = 0, digit;

    v = 0;
    for (; *hex; hex++) {
        if (*hex >= '0' && *hex <= '9') {
            digit = *hex - '0';
        } else if (*hex >= 'a' && *hex <= 'f') {
            digit = *hex - 'a' + 10;
        } else {
            continue;
        }
        v = v * 16 + digit;
        if (++half == 2) {
            if (n < max) {
                out[n] = (uint8_t)v;
            }
            n++;
            v = 0;
            half = 0;
        }
    }
    return n;
}

#define CHECK(cond, ...)                \
    do {                                \
        if (!(cond)) {                  \
            printf("FAIL %s: ", label); \
            printf(__VA_ARGS__);        \
            printf("\n");               \
            failures++;                 \
        }                               \
    } while (0)

/** Check one row of the table. */
static void check_row(const struct row *r)
{
    char label[96], got[160];
    uint8_t b[32];
    struct kvm_insn in, again;
    uint32_t n, i;
    int rc;

    snprintf(label, sizeof(label), "%d: %s", r->mode, r->hex);
    n = parse(r->hex, b, sizeof(b));
    if (r->want == 0) {
        rc = kvm_decode(b, n, r->mode, &in);
        CHECK(rc == -1, "decoded, len %u", in.len);
        return;
    }
    CHECK(n == r->len, "the row's bytes number %u, its length %u", n, r->len);
    for (i = n; i < sizeof(b); i++) {
        b[i] = 0xCC;
    }
    rc = kvm_decode(b, sizeof(b), r->mode, &in);
    CHECK(rc == 0, "refused");
    if (rc != 0) {
        return;
    }
    format(&in, got, sizeof(got));
    CHECK(in.len == r->len, "len %u, want %u", in.len, r->len);
    CHECK(strcmp(got, r->want) == 0, "got \"%s\", want \"%s\"", got, r->want);
    for (i = 0; i < in.len && i < KVM_INSN_MAX; i++) {
        CHECK(in.bytes[i] == b[i], "byte %u copied as %02x", i, in.bytes[i]);
    }
    rc = kvm_decode(b, r->len, r->mode, &again);
    CHECK(rc == 0 && again.len == r->len, "cut to its length: rc %d len %u", rc, again.len);
    rc = kvm_decode(b, r->len - 1, r->mode, &again);
    CHECK(rc == -1, "cut one byte short: decoded");
}

/** Check kvm_decode_fail's layout against Linux's emulation_failure. */
static void check_fail(void)
{
    static const char label[] = "kvm_decode_fail";
    static const uint8_t ud2[2] = {0x0F, 0x0B};
    uint8_t many[20];
    uint64_t info[5] = {0x400, 0x11, 0x22, 0x33, 0x44};
    struct kvm_run_internal e;
    const uint8_t *raw;
    uint32_t i;

    for (i = 0; i < sizeof(many); i++) {
        many[i] = (uint8_t)(0xA0 + i);
    }
    kvm_decode_fail(&e, ud2, 2, info);
    raw = (const uint8_t *)&e.data[1];
    CHECK(e.suberror == KVM_INTERNAL_ERROR_EMULATION, "suberror %u", e.suberror);
    CHECK(e.ndata == 8, "ndata %u", e.ndata);
    CHECK(e.data[0] == KVM_INTERNAL_ERROR_EMULATION_FLAG_INSTRUCTION_BYTES, "flags %llx",
          (unsigned long long)e.data[0]);
    CHECK(raw[0] == 2 && raw[1] == 0x0F && raw[2] == 0x0B, "insn %02x %02x %02x", raw[0], raw[1],
          raw[2]);
    for (i = 3; i < 16; i++) {
        CHECK(raw[i] == 0x90, "insn_bytes[%u] %02x, want the 0x90 padding", i - 1, raw[i]);
    }
    for (i = 0; i < 5; i++) {
        CHECK(e.data[3 + i] == info[i], "info %u at data[%u]", i, 3 + i);
    }
    CHECK(e.data[8] == 0, "data[8] %llx", (unsigned long long)e.data[8]);

    kvm_decode_fail(&e, many, sizeof(many), 0);
    raw = (const uint8_t *)&e.data[1];
    CHECK(raw[0] == 15 && raw[15] == 0xAE, "20 bytes kept as %u, last %02x", raw[0], raw[15]);
    CHECK(e.ndata == 8 && e.data[3] == 0 && e.data[7] == 0, "no info: ndata %u", e.ndata);

    kvm_decode_fail(&e, many, 0, info);
    CHECK(e.data[0] == 0 && e.ndata == 6, "no bytes: flags %llx ndata %u",
          (unsigned long long)e.data[0], e.ndata);
    CHECK(e.data[1] == info[0] && e.data[5] == info[4], "no bytes: info from data[1]");
}

int main(void)
{
    uint32_t i, nrows = sizeof(rows) / sizeof(rows[0]);

    for (i = 0; i < nrows; i++) {
        check_row(&rows[i]);
    }
    check_fail();
    if (failures) {
        printf("kvm_decode_hosttest: %d failure(s)\n", failures);
        return 1;
    }
    printf("kvm_decode_hosttest: %u rows ok\n", nrows);
    return 0;
}
