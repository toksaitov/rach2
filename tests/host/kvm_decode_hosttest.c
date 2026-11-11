/**
 * Check the decoder of the guest instructions the kernel carries out
 * (rach2/sys/kvm/decode.c) against a table of encodings.
 *
 * Each row is a mode, an instruction's bytes, its length and the decoded
 * instruction written out as text: the operation and its form, the
 * operands (a register and its size, memory as segment:[address] and its
 * size, an immediate, a segment or SSE or MMX register), the operand size
 * when no operand shows it, the address size, how the destination is
 * accessed (wr, rd or rw), and the flags: str for a string instruction, na
 * for memory that is named and not accessed, al for memory that must be
 * aligned, the prefixes that count, and ud for an encoding the processor
 * refuses. The row's bytes are decoded with 0xCC bytes after them, which
 * the decoder must not take, then cut to their length, which decodes the
 * same, and to one byte less, which fails. A row with no text is an
 * instruction the decoder does not know, decoded from its bytes alone;
 * its internal error is then checked against the layout of Linux's
 * emulation_failure.
 */
#include "kvm/decode.c"

extern int printf(const char *format, ...);
extern int snprintf(char *s, unsigned long n, const char *fmt, ...);
extern int strcmp(const char *a, const char *b);

static int failures;

/* One row of the table: `want` null for an instruction that fails. */
struct trow {
    int mode;
    const char *hex;
    uint32_t len;
    const char *want;
};

static const struct trow rows[] = {
    /* MOV in 64-bit mode: ModRM, SIB, RIP-relative, REX, prefixes. */
    {64, "8b 04 b3", 3, "mov ax.4, ds:[bx+si*4].4 a8 wr"},
    {64, "48 8b 05 10 00 00 00", 7, "mov ax.8, ds:[rip+0x10].8 a8 wr"},
    {64, "48 8b 05 f0 ff ff ff", 7, "mov ax.8, ds:[rip-0x10].8 a8 wr"},
    {64, "88 07", 2, "mov ds:[di].1, ax.1 a8 wr"},
    {64, "88 27", 2, "mov ds:[di].1, ah.1 a8 wr"},
    {64, "40 88 27", 3, "mov ds:[di].1, sp.1 a8 wr"},
    {64, "66 89 0a", 3, "mov ds:[dx].2, cx.2 a8 wr"},
    {64, "44 89 4d f8", 4, "mov ss:[bp-0x8].4, r9.4 a8 wr"},
    {64, "8b 44 24 f8", 4, "mov ax.4, ss:[sp-0x8].4 a8 wr"},
    {64, "41 8b 44 24 08", 5, "mov ax.4, ds:[r12+0x8].4 a8 wr"},
    {64, "41 8b 45 00", 4, "mov ax.4, ds:[r13].4 a8 wr"},
    {64, "42 8b 04 25 00 10 00 00", 8, "mov ax.4, ds:[r12+0x1000].4 a8 wr"},
    {64, "8b 04 25 00 10 00 00", 7, "mov ax.4, ds:[0x1000].4 a8 wr"},
    {64, "4c 8b 1c c5 00 00 00 00", 8, "mov r11.8, ds:[ax*8].8 a8 wr"},
    {64, "4f 8b 84 c8 00 01 00 00", 8, "mov r8.8, ds:[r8+r9*8+0x100].8 a8 wr"},
    {64, "67 8b 03", 3, "mov ax.4, ds:[bx].4 a4 wr"},
    {64, "67 8b 05 10 00 00 00", 7, "mov ax.4, ds:[rip+0x10].4 a4 wr"},
    {64, "64 48 8b 04 25 28 00 00 00", 9, "mov ax.8, fs:[0x28].8 a8 wr"},
    {64, "65 8b 04 25 00 00 00 00", 8, "mov ax.4, gs:[0x0].4 a8 wr"},
    {64, "3e 8b 45 00", 4, "mov ax.4, ds:[bp].4 a8 wr"},
    {64, "48 66 8b 07", 4, "mov ax.2, ds:[di].2 a8 wr"},
    {64, "66 48 8b 07", 4, "mov ax.8, ds:[di].8 a8 wr"},
    {64, "c6 00 ff", 3, "mov ds:[ax].1, $-1 a8 wr"},
    {64, "66 c7 00 34 12", 5, "mov ds:[ax].2, $4660 a8 wr"},
    {64, "c7 80 00 01 00 00 78 56 34 12", 10, "mov ds:[ax+0x100].4, $305419896 a8 wr"},
    {64, "48 c7 40 08 ff ff ff ff", 8, "mov ds:[ax+0x8].8, $-1 a8 wr"},
    {64, "48 a1 88 77 66 55 44 33 22 11", 10, "mov ax.8, ds:[0x1122334455667788].8 a8 wr"},
    {64, "a2 00 10 00 00 00 00 00 00", 9, "mov ds:[0x1000].1, ax.1 a8 wr"},
    {64, "67 a3 00 10 00 00", 6, "mov ds:[0x1000].4, ax.4 a4 wr"},
    {64, "65 a1 00 10 00 00 00 00 00 00", 10, "mov ax.4, gs:[0x1000].4 a8 wr"},
    {64, "2e 26 64 48 c7 84 88 78 56 34 12 ef be ad de", 15,
     "mov fs:[ax+cx*4+0x12345678].8, $-559038737 a8 wr"},

    /* MOVZX, MOVSX, MOVSXD and MOVNTI. */
    {64, "0f b6 06", 3, "mov:zx ax.4, ds:[si].1 a8 wr"},
    {64, "66 0f b6 06", 4, "mov:zx ax.2, ds:[si].1 a8 wr"},
    {64, "0f b7 06", 3, "mov:zx ax.4, ds:[si].2 a8 wr"},
    {64, "48 0f be 06", 4, "mov:sx ax.8, ds:[si].1 a8 wr"},
    {64, "48 0f bf 06", 4, "mov:sx ax.8, ds:[si].2 a8 wr"},
    {64, "44 0f b6 06", 4, "mov:zx r8.4, ds:[si].1 a8 wr"},
    {64, "48 63 06", 3, "mov:sx ax.8, ds:[si].4 a8 wr"},
    {64, "63 06", 2, "mov:sx ax.4, ds:[si].4 a8 wr"},
    {64, "0f c3 07", 3, "mov ds:[di].4, ax.4 a8 wr"},
    {64, "48 0f c3 07", 4, "mov ds:[di].8, ax.8 a8 wr"},

    /* MOVS and STOS. */
    {64, "a4", 1, "mov es:di.1, ds:si.1 a8 wr str"},
    {64, "f3 48 a5", 3, "mov es:di.8, ds:si.8 a8 wr str rep"},
    {64, "66 f3 a5", 3, "mov es:di.2, ds:si.2 a8 wr str rep"},
    {64, "f2 a4", 2, "mov es:di.1, ds:si.1 a8 wr str repne"},
    {64, "67 f3 a4", 3, "mov es:di.1, ds:si.1 a4 wr str rep"},
    {64, "2e a4", 2, "mov es:di.1, cs:si.1 a8 wr str"},
    {64, "f3 aa", 2, "mov es:di.1, ax.1 a8 wr str rep"},
    {64, "26 ab", 2, "mov es:di.4, ax.4 a8 wr str"},
    {64, "64 48 ab", 3, "mov es:di.8, ax.8 a8 wr str"},

    /* The arithmetic and logic group, with a register or an immediate. */
    {64, "00 07", 2, "alu:add ds:[di].1, ax.1 a8 rw"},
    {64, "01 07", 2, "alu:add ds:[di].4, ax.4 a8 rw"},
    {64, "03 07", 2, "alu:add ax.4, ds:[di].4 a8 rw"},
    {64, "08 27", 2, "alu:or ds:[di].1, ah.1 a8 rw"},
    {64, "48 09 07", 3, "alu:or ds:[di].8, ax.8 a8 rw"},
    {64, "11 07", 2, "alu:adc ds:[di].4, ax.4 a8 rw"},
    {64, "19 07", 2, "alu:sbb ds:[di].4, ax.4 a8 rw"},
    {64, "20 07", 2, "alu:and ds:[di].1, ax.1 a8 rw"},
    {64, "23 0f", 2, "alu:and cx.4, ds:[di].4 a8 rw"},
    {64, "29 07", 2, "alu:sub ds:[di].4, ax.4 a8 rw"},
    {64, "31 07", 2, "alu:xor ds:[di].4, ax.4 a8 rw"},
    {64, "4c 32 07", 3, "alu:xor r8.1, ds:[di].1 a8 rw"},
    {64, "39 07", 2, "alu:cmp ds:[di].4, ax.4 a8 rd"},
    {64, "3b 07", 2, "alu:cmp ax.4, ds:[di].4 a8 rd"},
    {64, "80 0f 01", 3, "alu:or ds:[di].1, $1 a8 rw"},
    {64, "81 27 ff 00 00 00", 6, "alu:and ds:[di].4, $255 a8 rw"},
    {64, "66 81 27 ff 00", 5, "alu:and ds:[di].2, $255 a8 rw"},
    {64, "48 83 2f f0", 4, "alu:sub ds:[di].8, $-16 a8 rw"},
    {64, "80 3f 00", 3, "alu:cmp ds:[di].1, $0 a8 rd"},
    {64, "48 81 3f ff ff ff 7f", 7, "alu:cmp ds:[di].8, $2147483647 a8 rd"},
    {64, "48 81 37 00 00 00 80", 7, "alu:xor ds:[di].8, $-2147483648 a8 rw"},
    {64, "f0 81 07 01 00 00 00", 7, "alu:add ds:[di].4, $1 a8 rw lock"},

    /* TEST, XCHG, INC, DEC, NOT and NEG. */
    {64, "85 07", 2, "alu:test ds:[di].4, ax.4 a8 rd"},
    {64, "84 27", 2, "alu:test ds:[di].1, ah.1 a8 rd"},
    {64, "f6 07 80", 3, "alu:test ds:[di].1, $-128 a8 rd"},
    {64, "f6 0f 01", 3, "alu:test ds:[di].1, $1 a8 rd"},
    {64, "f7 07 00 00 00 80", 6, "alu:test ds:[di].4, $-2147483648 a8 rd"},
    {64, "66 f7 07 34 12", 5, "alu:test ds:[di].2, $4660 a8 rd"},
    {64, "48 f7 07 ff ff ff ff", 7, "alu:test ds:[di].8, $-1 a8 rd"},
    {64, "86 07", 2, "xchg ds:[di].1, ax.1 a8 rw"},
    {64, "48 87 07", 3, "xchg ds:[di].8, ax.8 a8 rw"},
    {64, "f0 87 07", 3, "xchg ds:[di].4, ax.4 a8 rw lock"},
    {64, "f6 17", 2, "unary:not ds:[di].1 a8 rw"},
    {64, "48 f7 1f", 3, "unary:neg ds:[di].8 a8 rw"},
    {64, "fe 07", 2, "unary:inc ds:[di].1 a8 rw"},
    {64, "ff 0f", 2, "unary:dec ds:[di].4 a8 rw"},
    {64, "f0 ff 07", 3, "unary:inc ds:[di].4 a8 rw lock"},
    {64, "f0 f7 17", 3, "unary:not ds:[di].4 a8 rw lock"},

    /* PUSH and POP of memory. */
    {64, "ff 37", 2, "push ds:[di].8 a8"},
    {64, "66 ff 37", 3, "push ds:[di].2 a8"},
    {64, "48 ff 37", 3, "push ds:[di].8 a8"},
    {64, "66 48 ff 37", 4, "push ds:[di].8 a8"},
    {64, "8f 07", 2, "pop ds:[di].8 a8 wr"},
    {64, "41 8f 04 24", 4, "pop ds:[r12].8 a8 wr"},
    {64, "66 8f 07", 3, "pop ds:[di].2 a8 wr"},

    /* 32-bit mode. */
    {32, "8b 03", 2, "mov ax.4, ds:[bx].4 a4 wr"},
    {32, "8b 05 00 10 00 00", 6, "mov ax.4, ds:[0x1000].4 a4 wr"},
    {32, "66 89 0e", 3, "mov ds:[si].2, cx.2 a4 wr"},
    {32, "88 3e", 2, "mov ds:[si].1, bh.1 a4 wr"},
    {32, "a1 00 10 00 00", 5, "mov ax.4, ds:[0x1000].4 a4 wr"},
    {32, "67 a1 00 10", 4, "mov ax.4, ds:[0x1000].4 a2 wr"},
    {32, "67 8b 07", 3, "mov ax.4, ds:[bx].4 a2 wr"},
    {32, "8b 04 24", 3, "mov ax.4, ss:[sp].4 a4 wr"},
    {32, "0f b7 03", 3, "mov:zx ax.4, ds:[bx].2 a4 wr"},
    {32, "0f be 45 fc", 4, "mov:sx ax.4, ss:[bp-0x4].1 a4 wr"},
    {32, "c7 05 00 10 00 00 01 00 00 00", 10, "mov ds:[0x1000].4, $1 a4 wr"},
    {32, "82 07 01", 3, "alu:add ds:[di].1, $1 a4 rw"},
    {32, "f3 a5", 2, "mov es:di.4, ds:si.4 a4 wr str rep"},
    {32, "66 ab", 2, "mov es:di.2, ax.2 a4 wr str"},
    {32, "ff 30", 2, "push ds:[ax].4 a4"},
    {32, "66 ff 30", 3, "push ds:[ax].2 a4"},
    {32, "8f 00", 2, "pop ds:[ax].4 a4 wr"},
    {32, "f0 09 1d 00 10 00 00", 7, "alu:or ds:[0x1000].4, bx.4 a4 rw lock"},

    /* 16-bit mode: the eight ModRM forms, disp16 and the 0x67 forms. */
    {16, "8b 00", 2, "mov ax.2, ds:[bx+si].2 a2 wr"},
    {16, "8b 01", 2, "mov ax.2, ds:[bx+di].2 a2 wr"},
    {16, "8b 02", 2, "mov ax.2, ss:[bp+si].2 a2 wr"},
    {16, "8b 03", 2, "mov ax.2, ss:[bp+di].2 a2 wr"},
    {16, "8b 04", 2, "mov ax.2, ds:[si].2 a2 wr"},
    {16, "8b 05", 2, "mov ax.2, ds:[di].2 a2 wr"},
    {16, "8b 06 00 10", 4, "mov ax.2, ds:[0x1000].2 a2 wr"},
    {16, "8b 07", 2, "mov ax.2, ds:[bx].2 a2 wr"},
    {16, "8b 46 fe", 3, "mov ax.2, ss:[bp-0x2].2 a2 wr"},
    {16, "8b 87 34 12", 4, "mov ax.2, ds:[bx+0x1234].2 a2 wr"},
    {16, "8b 87 00 80", 4, "mov ax.2, ds:[bx-0x8000].2 a2 wr"},
    {16, "66 8b 07", 3, "mov ax.4, ds:[bx].4 a2 wr"},
    {16, "67 8b 04 b3", 4, "mov ax.2, ds:[bx+si*4].2 a4 wr"},
    {16, "66 67 8b 04 b3", 5, "mov ax.4, ds:[bx+si*4].4 a4 wr"},
    {16, "26 88 07", 3, "mov es:[bx].1, ax.1 a2 wr"},
    {16, "c6 07 12", 3, "mov ds:[bx].1, $18 a2 wr"},
    {16, "c7 07 34 12", 4, "mov ds:[bx].2, $4660 a2 wr"},
    {16, "66 c7 07 78 56 34 12", 7, "mov ds:[bx].4, $305419896 a2 wr"},
    {16, "a1 00 10", 3, "mov ax.2, ds:[0x1000].2 a2 wr"},
    {16, "0f b6 07", 3, "mov:zx ax.2, ds:[bx].1 a2 wr"},
    {16, "80 3e 00 10 00", 5, "alu:cmp ds:[0x1000].1, $0 a2 rd"},
    {16, "83 06 00 10 ff", 5, "alu:add ds:[0x1000].2, $-1 a2 rw"},
    {16, "a4", 1, "mov es:di.1, ds:si.1 a2 wr str"},
    {16, "f3 a5", 2, "mov es:di.2, ds:si.2 a2 wr str rep"},
    {16, "66 f3 a5", 3, "mov es:di.4, ds:si.4 a2 wr str rep"},
    {16, "ff 36 00 10", 4, "push ds:[0x1000].2 a2"},
    {16, "66 ff 36 00 10", 5, "push ds:[0x1000].4 a2"},
    {16, "8f 06 00 10", 4, "pop ds:[0x1000].2 a2 wr"},

    /* The string compares, scans and loads, and XLAT. */
    {64, "a6", 1, "alu:cmpr es:di.1, ds:si.1 a8 rd str"},
    {64, "f3 a6", 2, "alu:cmpr es:di.1, ds:si.1 a8 rd str rep"},
    {64, "f3 48 a7", 3, "alu:cmpr es:di.8, ds:si.8 a8 rd str rep"},
    {64, "f2 66 a7", 3, "alu:cmpr es:di.2, ds:si.2 a8 rd str repne"},
    {64, "ae", 1, "alu:cmpr es:di.1, ax.1 a8 rd str"},
    {64, "f2 af", 2, "alu:cmpr es:di.4, ax.4 a8 rd str repne"},
    {64, "f3 48 af", 3, "alu:cmpr es:di.8, ax.8 a8 rd str rep"},
    {64, "ac", 1, "mov ax.1, ds:si.1 a8 wr str"},
    {64, "f3 48 ad", 3, "mov ax.8, ds:si.8 a8 wr str rep"},
    {64, "65 ac", 2, "mov ax.1, gs:si.1 a8 wr str"},
    {32, "d7", 1, "mov ax.1, ds:xlat.1 a4 wr"},
    {32, "2e d7", 2, "mov ax.1, cs:xlat.1 a4 wr"},
    {16, "f3 a7", 2, "alu:cmpr es:di.2, ds:si.2 a2 rd str rep"},

    /* The register forms, the accumulator's and the short ones. */
    {64, "8b c0", 2, "mov ax.4, ax.4 a8 wr"},
    {64, "48 89 e5", 3, "mov bp.8, sp.8 a8 wr"},
    {64, "0f b6 c0", 3, "mov:zx ax.4, ax.1 a8 wr"},
    {64, "0f b6 c4", 3, "mov:zx ax.4, ah.1 a8 wr"},
    {64, "01 d8", 2, "alu:add ax.4, bx.4 a8 rw"},
    {64, "04 7f", 2, "alu:add ax.1, $127 a8 rw"},
    {64, "48 3d 00 00 00 80", 6, "alu:cmp ax.8, $-2147483648 a8 rd"},
    {64, "a8 01", 2, "alu:test ax.1, $1 a8 rd"},
    {64, "b0 ff", 2, "mov ax.1, $-1 a8 wr"},
    {64, "b4 01", 2, "mov ah.1, $1 a8 wr"},
    {64, "41 b8 78 56 34 12", 6, "mov r8.4, $305419896 a8 wr"},
    {64, "48 b8 88 77 66 55 44 33 22 11", 10, "mov ax.8, $1234605616436508552 a8 wr"},
    {64, "c7 c0 01 00 00 00", 6, "mov ax.4, $1 a8 wr"},
    {64, "90", 1, "xchg ax.4, ax.4 a8 rw"},
    {64, "f3 90", 2, "xchg ax.4, ax.4 a8 rw rep"},
    {64, "49 93", 2, "xchg r11.8, ax.8 a8 rw"},
    {64, "ff c0", 2, "unary:inc ax.4 a8 rw"},
    {64, "66 0f c3 07", 4, "mov ds:[di].4, ax.4 a8 wr"},
    {64, "f3 0f c3 07", 4, "mov ds:[di].4, ax.4 a8 wr rep"},
    {32, "40", 1, "unary:inc ax.4 a4 rw"},
    {32, "4f", 1, "unary:dec di.4 a4 rw"},
    {32, "48", 1, "unary:dec ax.4 a4 rw"},
    {16, "43", 1, "unary:inc bx.2 a2 rw"},

    /* The shifts and rotates, and SHLD and SHRD. */
    {64, "c0 27 03", 3, "shift:shl ds:[di].1, $3 a8 rw"},
    {64, "c1 2f 1f", 3, "shift:shr ds:[di].4, $31 a8 rw"},
    {64, "d0 07", 2, "shift:rol ds:[di].1, $1 a8 rw"},
    {64, "48 d1 3f", 3, "shift:sar ds:[di].8, $1 a8 rw"},
    {64, "d2 0f", 2, "shift:ror ds:[di].1, cx.1 a8 rw"},
    {64, "d3 17", 2, "shift:rcl ds:[di].4, cx.1 a8 rw"},
    {64, "66 d3 1f", 3, "shift:rcr ds:[di].2, cx.1 a8 rw"},
    {64, "d1 37", 2, "shift:sal ds:[di].4, $1 a8 rw"},
    {64, "c1 e0 04", 3, "shift:shl ax.4, $4 a8 rw"},
    {64, "0f a4 07 04", 4, "dshift ds:[di].4, ax.4, $4 a8 rw"},
    {64, "0f a5 0f", 3, "dshift ds:[di].4, cx.4, cx.1 a8 rw"},
    {64, "48 0f ac 17 3f", 5, "dshift:1 ds:[di].8, dx.8, $63 a8 rw"},
    {64, "66 0f ad 07", 4, "dshift:1 ds:[di].2, ax.2, cx.1 a8 rw"},

    /* The multiplies and divides. */
    {64, "f6 27", 2, "muldiv:mul ds:[di].1 a8"},
    {64, "f7 2f", 2, "muldiv:imul ds:[di].4 a8"},
    {64, "48 f7 37", 3, "muldiv:div ds:[di].8 a8"},
    {64, "66 f7 3f", 3, "muldiv:idiv ds:[di].2 a8"},
    {64, "0f af 07", 3, "imul ax.4, ds:[di].4 a8 rw"},
    {64, "6b 07 f0", 3, "imul ax.4, ds:[di].4, $-16 a8 rw"},
    {64, "48 69 07 00 01 00 00", 7, "imul ax.8, ds:[di].8, $256 a8 rw"},
    {64, "66 69 07 34 12", 5, "imul ax.2, ds:[di].2, $4660 a8 rw"},

    /* The bit tests and scans. */
    {64, "0f a3 07", 3, "bt:bt ds:[di].4, ax.4 a8 rd"},
    {64, "0f ab 07", 3, "bt:bts ds:[di].4, ax.4 a8 rw"},
    {64, "f0 0f b3 07", 4, "bt:btr ds:[di].4, ax.4 a8 rw lock"},
    {64, "48 0f bb 07", 4, "bt:btc ds:[di].8, ax.8 a8 rw"},
    {64, "0f ba 27 07", 4, "bt:bt ds:[di].4, $7 a8 rd"},
    {64, "0f ba 3f 1f", 4, "bt:btc ds:[di].4, $31 a8 rw"},
    {64, "66 0f ba 2f 0f", 5, "bt:bts ds:[di].2, $15 a8 rw"},
    {64, "f0 0f ba 37 00", 5, "bt:btr ds:[di].4, $0 a8 rw lock"},
    {64, "0f bc 07", 3, "bscan ax.4, ds:[di].4 a8 rw"},
    {64, "48 0f bd 07", 4, "bscan:1 ax.8, ds:[di].8 a8 rw"},

    /* CMPXCHG, XADD and CMPXCHG8B. */
    {64, "0f b0 0f", 3, "cmpxchg ds:[di].1, cx.1 a8 rw"},
    {64, "f0 48 0f b1 0f", 5, "cmpxchg ds:[di].8, cx.8 a8 rw lock"},
    {64, "0f c0 07", 3, "xadd ds:[di].1, ax.1 a8 rw"},
    {64, "f0 0f c1 07", 4, "xadd ds:[di].4, ax.4 a8 rw lock"},
    {64, "0f c7 0f", 3, "cmpxchg8b ds:[di].8 a8 rw"},
    {64, "f0 0f c7 0f", 4, "cmpxchg8b ds:[di].8 a8 rw lock"},

    /* CMOVcc, SETcc, MOVBE, BSWAP, LEA and the no-operations. */
    {64, "0f 45 07", 3, "cmov:5 ax.4, ds:[di].4 a8 rw"},
    {64, "48 0f 4f c1", 4, "cmov:15 ax.8, cx.8 a8 rw"},
    {64, "0f 94 07", 3, "setcc:4 ds:[di].1 a8 wr"},
    {64, "0f 9f c1", 3, "setcc:15 cx.1 a8 wr"},
    {64, "0f 38 f0 07", 4, "movbe ax.4, ds:[di].4 a8 wr"},
    {64, "66 0f 38 f1 07", 5, "movbe ds:[di].2, ax.2 a8 wr"},
    {64, "0f c8", 2, "bswap ax.4 a8 rw"},
    {64, "49 0f cf", 3, "bswap r15.8 a8 rw"},
    {64, "48 8d 44 24 08", 5, "lea ax.8, ss:[sp+0x8].8 a8 wr na"},
    {64, "0f 1f 44 00 00", 5, "nop ds:[ax+ax].4 a8 na"},
    {64, "0f 18 07", 3, "nop ds:[di].4 a8 na"},
    {64, "0f ae 3f", 3, "nop ds:[di].1 a8"},

    /* The stack instructions. */
    {64, "50", 1, "push ax.8 a8"},
    {64, "41 57", 2, "push r15.8 a8"},
    {64, "66 50", 2, "push ax.2 a8"},
    {64, "5d", 1, "pop bp.8 a8 wr"},
    {64, "6a ff", 2, "push $-1 s8 a8"},
    {64, "68 78 56 34 12", 5, "push $305419896 s8 a8"},
    {64, "66 6a 01", 3, "push $1 s2 a8"},
    {32, "6a 01", 2, "push $1 s4 a4"},
    {64, "9c", 1, "pushf s8 a8"},
    {64, "9d", 1, "popf s8 a8"},
    {32, "66 9c", 2, "pushf s2 a4"},
    {64, "c8 10 00 01", 4, "enter $16, $1 s8 a8"},
    {64, "c9", 1, "leave s8 a8"},
    {32, "60", 1, "pusha s4 a4"},
    {32, "61", 1, "popa s4 a4"},
    {16, "60", 1, "pusha s2 a2"},
    {32, "06", 1, "push_sreg s4 a4"},
    {32, "1e", 1, "push_sreg:3 s4 a4"},
    {32, "17", 1, "pop_sreg:2 s4 a4"},
    {64, "0f a0", 2, "push_sreg:4 s8 a8"},
    {64, "0f a9", 2, "pop_sreg:5 s8 a8"},

    /* The jumps, calls and returns, near and far. */
    {64, "eb fe", 2, "jmp $-2 s8 a8"},
    {64, "e9 00 01 00 00", 5, "jmp $256 s8 a8"},
    {64, "74 10", 2, "jcc:4 $16 s8 a8"},
    {64, "0f 85 00 ff ff ff", 6, "jcc:5 $-256 s8 a8"},
    {16, "e9 00 01", 3, "jmp $256 s2 a2"},
    {64, "e8 10 00 00 00", 5, "call $16 s8 a8"},
    {64, "ff 17", 2, "call_abs ds:[di].8 a8"},
    {64, "ff 27", 2, "jmp_abs ds:[di].8 a8"},
    {64, "ff e0", 2, "jmp_abs ax.8 a8"},
    {32, "ff 20", 2, "jmp_abs ds:[ax].4 a4"},
    {64, "ff 1f", 2, "call_far ds:[di].6 a8"},
    {64, "48 ff 2f", 3, "jmp_far ds:[di].10 a8"},
    {32, "ff 2f", 2, "jmp_far ds:[di].6 a4"},
    {16, "ff 2f", 2, "jmp_far ds:[bx].4 a2"},
    {32, "ea 78 56 34 12 08 00", 7, "jmp_far $305419896, $8 s4 a4"},
    {16, "9a 00 10 00 f0", 5, "call_far $4096, $61440 s2 a2"},
    {64, "c3", 1, "ret s8 a8"},
    {64, "c2 10 00", 3, "ret $16 s8 a8"},
    {64, "cb", 1, "ret_far s4 a8"},
    {64, "48 cb", 2, "ret_far s8 a8"},
    {32, "ca 08 00", 3, "ret_far $8 s4 a4"},
    {64, "e0 05", 2, "loop $5 s8 a8"},
    {64, "e2 fe", 2, "loop:2 $-2 s8 a8"},
    {64, "e3 05", 2, "loop:3 $5 s8 a8"},

    /* The segment registers and the descriptor tables. */
    {64, "8e 17", 2, "mov_sreg %ss, ds:[di].2 a8 wr"},
    {64, "8e d8", 2, "mov_sreg %ds, ax.2 a8 wr"},
    {64, "8c 0f", 2, "store_seg ds:[di].2, %cs a8 wr"},
    {64, "66 8c 0f", 3, "store_seg ds:[di].2, %cs a8 wr"},
    {64, "8c d8", 2, "store_seg ax.4, %ds a8 wr"},
    {64, "48 8c d8", 3, "store_seg ax.8, %ds a8 wr"},
    {64, "0f 00 07", 3, "store_seg:6 ds:[di].2 a8 wr"},
    {64, "0f 00 0f", 3, "store_seg:7 ds:[di].2 a8 wr"},
    {64, "0f 00 17", 3, "load_seg:6 ds:[di].2 a8"},
    {64, "0f 00 1f", 3, "load_seg:7 ds:[di].2 a8"},
    {64, "0f 00 d8", 3, "load_seg:7 ax.2 a8"},
    {32, "c4 07", 2, "lseg ax.4, ds:[di].6 a4 wr"},
    {32, "c5 07", 2, "lseg:3 ax.4, ds:[di].6 a4 wr"},
    {16, "c4 07", 2, "lseg ax.2, ds:[bx].4 a2 wr"},
    {64, "0f b2 07", 3, "lseg:2 ax.4, ds:[di].6 a8 wr"},
    {64, "48 0f b4 07", 4, "lseg:4 ax.8, ds:[di].10 a8 wr"},
    {64, "0f b5 07", 3, "lseg:5 ax.4, ds:[di].6 a8 wr"},
    {64, "0f 01 07", 3, "store_dt ds:[di].4 a8 wr na"},
    {64, "0f 01 0f", 3, "store_dt:1 ds:[di].4 a8 wr na"},
    {64, "0f 01 17", 3, "load_dt ds:[di].4 a8 na"},
    {64, "0f 01 1f", 3, "load_dt:1 ds:[di].4 a8 na"},
    {64, "0f 01 27", 3, "smsw ds:[di].2 a8 wr"},
    {64, "0f 01 e0", 3, "smsw ax.4 a8 wr"},
    {64, "48 0f 01 e0", 4, "smsw ax.8 a8 wr"},
    {64, "0f 01 37", 3, "lmsw ds:[di].2 a8"},
    {64, "0f 01 f0", 3, "lmsw ax.2 a8"},

    /* The flag instructions, the sign extensions and the BCD ones. */
    {64, "f5", 1, "flag:cmc s4 a8"},
    {64, "f8", 1, "flag:clc s4 a8"},
    {64, "f9", 1, "flag:stc s4 a8"},
    {64, "fa", 1, "flag:cli s4 a8"},
    {64, "fb", 1, "flag:sti s4 a8"},
    {64, "fc", 1, "flag:cld s4 a8"},
    {64, "fd", 1, "flag:std s4 a8"},
    {64, "98", 1, "cbw s4 a8"},
    {64, "48 98", 2, "cbw s8 a8"},
    {64, "66 99", 2, "cwd s2 a8"},
    {64, "9e", 1, "sahf s4 a8"},
    {64, "9f", 1, "lahf s4 a8"},
    {32, "2f", 1, "bcd:das s4 a4"},
    {32, "d4 0a", 2, "bcd:aam $10 s4 a4"},
    {32, "d5 0a", 2, "bcd:aad $10 s4 a4"},
    {32, "d6", 1, "bcd:salc s4 a4"},

    /* The SSE and MMX moves and the x87 control instructions. */
    {64, "0f 10 07", 3, "mov xmm0, ds:[di].16 a8 wr"},
    {64, "0f 11 0f", 3, "mov ds:[di].16, xmm1 a8 wr"},
    {64, "66 0f 10 07", 4, "mov xmm0, ds:[di].16 a8 wr"},
    {64, "0f 28 07", 3, "mov xmm0, ds:[di].16 a8 wr al"},
    {64, "66 0f 29 07", 4, "mov ds:[di].16, xmm0 a8 wr al"},
    {64, "44 0f 28 c1", 4, "mov xmm8, xmm1 a8 wr al"},
    {64, "0f 2b 07", 3, "mov ds:[di].16, xmm0 a8 wr al"},
    {64, "66 0f e7 07", 4, "mov ds:[di].16, xmm0 a8 wr al"},
    {64, "66 0f 6f 07", 4, "mov xmm0, ds:[di].16 a8 wr al"},
    {64, "f3 0f 6f 07", 4, "mov xmm0, ds:[di].16 a8 wr"},
    {64, "f3 0f 7f 07", 4, "mov ds:[di].16, xmm0 a8 wr"},
    {64, "0f 6f 07", 3, "mov mm0, ds:[di].8 a8 wr"},
    {64, "0f 7f 0f", 3, "mov ds:[di].8, mm1 a8 wr"},
    {64, "41 0f 6f c1", 4, "mov mm0, mm1 a8 wr"},
    {64, "d9 3f", 2, "fnstcw ds:[di].2 a8 wr"},
    {64, "dd 3f", 2, "fnstsw ds:[di].2 a8 wr"},
    {64, "db e3", 2, "fninit s4 a8"},
    {32, "0f ae 07", 3, "fxsave ds:[di].4 a4 wr na al"},
    {32, "0f ae 0f", 3, "fxrstor ds:[di].4 a4 na al"},

    /* SYSCALL, SYSENTER and SYSEXIT. */
    {64, "0f 05", 2, "syscall s4 a8"},
    {32, "0f 34", 2, "sysenter s4 a4"},
    {64, "48 0f 35", 3, "sysexit s8 a8"},

    /* VMCALL and VMMCALL, which the emulator rewrites as the host's. */
    {64, "0f 01 c1", 3, "hypercall s4 a8"},
    {64, "0f 01 d9", 3, "hypercall:1 s4 a8"},
    {32, "0f 01 d9", 3, "hypercall:1 s4 a4"},

    /* RDPID, group 9's register form under 0xF3. */
    {64, "f3 0f c7 f8", 4, "rdpid ax.8 a8 wr rep"},
    {64, "f3 41 0f c7 ff", 5, "rdpid r15.8 a8 wr rep"},
    {32, "f3 0f c7 f9", 4, "rdpid cx.4 a4 wr rep"},
    {16, "f3 0f c7 fa", 4, "rdpid dx.4 a2 wr rep"},

    /* Refused by the processor with #UD: LOCK where it is not taken,
     * an instruction 64-bit mode lacks, a group's undefined member. */
    {64, "f0 8b 07", 3, "mov ax.4, ds:[di].4 a8 wr lock ud"},
    {64, "f0 39 07", 3, "alu:cmp ds:[di].4, ax.4 a8 rd lock ud"},
    {64, "f0 03 07", 3, "alu:add ax.4, ds:[di].4 a8 rw lock ud"},
    {64, "f0 85 07", 3, "alu:test ds:[di].4, ax.4 a8 rd lock ud"},
    {64, "f0 a4", 2, "mov es:di.1, ds:si.1 a8 wr str lock ud"},
    {64, "f0 ff 37", 3, "push ds:[di].8 a8 lock ud"},
    {64, "f0 01 d8", 3, "alu:add ax.4, bx.4 a8 rw lock ud"},
    {64, "f0 48 0f c3 07", 5, "mov ds:[di].8, ax.8 a8 wr lock ud"},
    {64, "c6 08", 2, "nop s0 a8 ud"},
    {64, "ff 3f", 2, "nop s0 a8 ud"},
    {64, "82 07", 2, "nop s0 a8 ud"},
    {64, "06", 1, "nop s0 a8 ud"},
    {64, "60", 1, "nop s0 a8 ud"},
    {64, "d4", 1, "nop s0 a8 ud"},
    {64, "ea", 1, "nop s0 a8 ud"},

    /* Not decoded: outside the set, the backends' own exits, the bytes
     * ending early, past 15 bytes, a mode that is none. */
    {64, "0f 0b", 0, 0},
    {64, "c4 e2 79 18 07", 0, 0},
    {64, "c5 07", 0, 0},
    {32, "c4 c0", 0, 0},
    {64, "8f 08", 0, 0},
    {64, "fe 17", 0, 0},
    {64, "0f c3 c0", 0, 0},
    {64, "8d c0", 0, 0},
    {64, "0f 01 3f", 0, 0},
    {64, "0f 01 c2", 0, 0},
    {64, "0f c7 f8", 0, 0},
    {64, "66 f3 0f c7 f8", 0, 0},
    {64, "f3 0f c7 f0", 0, 0},
    {64, "0f 01 d8", 0, 0},
    {64, "0f 01 c9", 0, 0},
    {64, "0f a2", 0, 0},
    {64, "f4", 0, 0},
    {64, "e4 10", 0, 0},
    {64, "6c", 0, 0},
    {64, "cf", 0, 0},
    {64, "0f 20 c0", 0, 0},
    {64, "0f 22 c0", 0, 0},
    {64, "0f 30", 0, 0},
    {64, "f2 0f 10 07", 0, 0},
    {64, "f3 0f 10 07", 0, 0},
    {64, "0f e7 07", 0, 0},
    {64, "66 0f 2b c0", 0, 0},
    {64, "f2 0f 38 f0 07", 0, 0},
    {64, "48 0f c7 0f", 0, 0},
    {64, "0f c7 c8", 0, 0},
    {64, "d9 07", 0, 0},
    {64, "db 07", 0, 0},
    {64, "dd 07", 0, 0},
    {64, "27", 0, 0},
    {64, "0f ae 17", 0, 0},
    {64, "41", 0, 0},
    {64, "0f", 0, 0},
    {64, "8b", 0, 0},
    {64, "8b 04", 0, 0},
    {64, "8b 87 00", 0, 0},
    {64, "c7 00 01 00", 0, 0},
    {64, "a1 00 10 00 00", 0, 0},
    {64, "2e 2e 26 64 48 c7 84 88 78 56 34 12 ef be ad de", 0, 0},
    {64, "66 66 66 66 66 66 66 66 66 66 66 66 66 66 66 8b 07", 0, 0},
    {32, "63 07", 0, 0},
    {32, "8b 05 00 10", 0, 0},
    {16, "8b 06 00", 0, 0},
    {8, "8b 07", 0, 0},
};

static const char *const regs[17] = {"ax", "cx",  "dx",  "bx",  "sp",  "bp",  "si",  "di", "r8",
                                     "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rip"};
static const char *const highs[4] = {"ah", "ch", "dh", "bh"};
static const char *const segs[8] = {"es", "cs", "ss", "ds", "fs", "gs", "ldtr", "tr"};
static const char *const ops[KVM_OP_LAST + 1] = {
    "?",        "mov",       "alu",       "unary",    "xchg",      "shift",   "muldiv",    "imul",
    "bt",       "bscan",     "dshift",    "cmpxchg",  "cmpxchg8b", "xadd",    "cmov",      "setcc",
    "movbe",    "bswap",     "lea",       "nop",      "push",      "pop",     "pusha",     "popa",
    "pushf",    "popf",      "push_sreg", "pop_sreg", "enter",     "leave",   "jcc",       "jmp",
    "jmp_abs",  "jmp_far",   "call",      "call_abs", "call_far",  "ret",     "ret_far",   "loop",
    "mov_sreg", "store_seg", "load_seg",  "lseg",     "store_dt",  "load_dt", "smsw",      "lmsw",
    "flag",     "cbw",       "cwd",       "sahf",     "lahf",      "bcd",     "fnstcw",    "fnstsw",
    "fninit",   "fxsave",    "fxrstor",   "syscall",  "sysenter",  "sysexit", "hypercall", "rdpid"};
static const char *const alus[14] = {"add", "or",   "adc", "sbb", "and", "sub", "xor",
                                     "cmp", "test", "inc", "dec", "not", "neg", "cmpr"};
static const char *const movs[3] = {"", "zx", "sx"};
static const char *const shifts[8] = {"rol", "ror", "rcl", "rcr", "shl", "shr", "sal", "sar"};
static const char *const muldivs[8] = {"?", "?", "?", "?", "mul", "imul", "div", "idiv"};
static const char *const bts[4] = {"bt", "bts", "btr", "btc"};
static const char *const flagops[7] = {"cmc", "clc", "stc", "cli", "sti", "cld", "std"};
static const char *const bcds[4] = {"das", "aam", "aad", "salc"};

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

/** Write the operand `o` of `in`, after `sep`, when the instruction has it. */
static void put_operand(const struct kvm_insn *in, const struct kvm_operand *o, const char *sep,
                        char *buf, uint32_t size, uint32_t *at)
{
    const char *seg = in->seg < 6 ? segs[in->seg] : "?";

    if (o->kind == KVM_OPND_NONE) {
        return;
    }
    PUT("%s", sep);
    switch (o->kind) {
    case KVM_OPND_REG:
        if (o->high8) {
            PUT("%s.%u", o->reg < 4 ? highs[o->reg] : "?", o->size);
        } else {
            PUT("%s.%u", o->reg < 16 ? regs[o->reg] : "?", o->size);
        }
        break;
    case KVM_OPND_MEM:
        if (o->mem == KVM_MEM_SI) {
            PUT("%s:si", seg);
        } else if (o->mem == KVM_MEM_DI) {
            PUT("es:di");
        } else if (o->mem == KVM_MEM_XLAT) {
            PUT("%s:xlat", seg);
        } else {
            PUT("%s:", seg);
            put_addr(in, buf, size, at);
        }
        PUT(".%u", o->size);
        break;
    case KVM_OPND_IMM:
        PUT("$%lld", (long long)o->imm);
        break;
    case KVM_OPND_SREG:
        PUT("%%%s", o->reg < 8 ? segs[o->reg] : "?");
        break;
    case KVM_OPND_XMM:
        PUT("xmm%u", o->reg);
        break;
    case KVM_OPND_MM:
        PUT("mm%u", o->reg);
        break;
    default:
        PUT("?");
        break;
    }
}

/** Write the operation of `in` and its form: a name, or a number not 0. */
static void put_op(const struct kvm_insn *in, char *buf, uint32_t size, uint32_t *at)
{
    const char *const *names = 0;
    uint32_t count = 0;

    PUT("%s", in->op <= KVM_OP_LAST ? ops[in->op] : "?");
    switch (in->op) {
    case KVM_OP_ALU:
    case KVM_OP_UNARY:
        names = alus;
        count = 14;
        break;
    case KVM_OP_MOV:
        names = movs;
        count = 3;
        break;
    case KVM_OP_SHIFT:
        names = shifts;
        count = 8;
        break;
    case KVM_OP_MULDIV:
        names = muldivs;
        count = 8;
        break;
    case KVM_OP_BT:
        names = bts;
        count = 4;
        break;
    case KVM_OP_FLAG:
        names = flagops;
        count = 7;
        break;
    case KVM_OP_BCD:
        names = bcds;
        count = 4;
        break;
    default:
        break;
    }
    if (!names) {
        if (in->sub != 0) {
            PUT(":%u", in->sub);
        }
    } else if (in->sub >= count) {
        PUT(":?");
    } else if (names[in->sub][0]) {
        PUT(":%s", names[in->sub]);
    }
}

/** Whether `o` shows the operand size: a register or memory. */
static int sized(const struct kvm_operand *o)
{
    return o->kind == KVM_OPND_REG || o->kind == KVM_OPND_MEM || o->kind == KVM_OPND_XMM ||
           o->kind == KVM_OPND_MM;
}

/** Write `in` in the table's text form into `buf` of `size` bytes. */
static void format(const struct kvm_insn *in, char *buf, uint32_t size)
{
    uint32_t n = 0, *at = &n;
    int first = in->dst.kind == KVM_OPND_NONE;

    buf[0] = 0;
    put_op(in, buf, size, at);
    put_operand(in, &in->dst, " ", buf, size, at);
    put_operand(in, &in->src, first ? " " : ", ", buf, size, at);
    put_operand(in, &in->src2, ", ", buf, size, at);
    if (!sized(&in->dst) && !sized(&in->src)) {
        PUT(" s%u", in->size);
    }
    PUT(" a%u", in->addr_size);
    if (in->dst.kind != KVM_OPND_NONE) {
        PUT(" %s", in->flags & KVM_INSN_MOV ? "wr" : in->flags & KVM_INSN_NOWRITE ? "rd" : "rw");
    }
    if (in->flags & KVM_INSN_STRING) {
        PUT(" str");
    }
    if (in->flags & KVM_INSN_NOACCESS) {
        PUT(" na");
    }
    if (in->flags & KVM_INSN_ALIGNED) {
        PUT(" al");
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
    if (in->flags & KVM_INSN_UD) {
        PUT(" ud");
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
static void check_row(const struct trow *r)
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
