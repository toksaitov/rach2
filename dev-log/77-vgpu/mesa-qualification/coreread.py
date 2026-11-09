# coreread.py CORE: for each thread of an x86-64 Linux core file whose rip is
# 0, print its registers, the first word on its stack (the return address of
# the call through a null pointer), the mapping that word falls in, and the 64
# bytes of code before it in code.bin, for objdump -D -b binary -mi386:x86-64.
# Used without gdb on the testing PC, for virgl_render_server's core.
import struct, sys

d = open(sys.argv[1], 'rb').read()
phoff, = struct.unpack_from('<Q', d, 0x20)
phnum, = struct.unpack_from('<H', d, 0x38)
loads, notes = [], []
for i in range(phnum):
    t, _, off, va, _, fs, _, _ = struct.unpack_from('<IIQQQQQQ', d, phoff + i * 56)
    if t == 1:
        loads.append((va, off, fs))
    elif t == 4:
        notes.append((off, fs))


def mem(a, n):
    """Return n bytes of the process's memory at a, or None if not dumped."""
    for va, off, fs in loads:
        if va <= a and a + n <= va + fs:
            return d[off + a - va:off + a - va + n]


files, threads = [], []
for off, fs in notes:
    p = off
    while p < off + fs:
        nsz, dsz, typ = struct.unpack_from('<III', d, p)
        p += 12 + ((nsz + 3) & ~3)
        desc = d[p:p + dsz]
        p += (dsz + 3) & ~3
        if typ == 1:  # NT_PRSTATUS: pid at 32, the registers at 112
            threads.append((struct.unpack_from('<I', desc, 32)[0],
                            struct.unpack_from('<27Q', desc, 112)))
        elif typ == 0x46494c45:  # NT_FILE
            cnt, page = struct.unpack_from('<QQ', desc, 0)
            names = desc[16 + cnt * 24:].split(b'\0')
            for k in range(cnt):
                s, e, o = struct.unpack_from('<QQQ', desc, 16 + k * 24)
                files.append((s, e, o * page, names[k].decode()))

NAMES = ('r15 r14 r13 r12 rbp rbx r11 r10 r9 r8 rax rcx rdx rsi rdi orig_rax '
         'rip cs eflags rsp').split()
for tid, r in threads:
    if r[16] != 0:
        continue
    print('thread %d' % tid)
    print(' '.join('%s=0x%x' % (k, r[i]) for i, k in enumerate(NAMES)))
    ret, = struct.unpack('<Q', mem(r[19], 8))
    hit = [n + '+0x%x' % (ret - s + o) for s, e, o, n in files if s <= ret < e]
    print('return address 0x%x in %s' % (ret, hit[0] if hit else
                                          'no file mapping (anonymous memory)'))
    open('code.bin', 'wb').write(mem(ret - 64, 64))
    print('code.bin: the 64 bytes before it; objdump --adjust-vma=0x%x' % (ret - 64))
