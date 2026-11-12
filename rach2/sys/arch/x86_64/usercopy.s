# The copy between kernel and user memory that takes no lock (mm/usercopy.c).
# A fault in either string move that the page fault handler cannot resolve
# resumes at its fixup (`arch_user_copy_fixup`, arch/x86/cpu.c), which
# returns the bytes not copied, as Linux's copy_user_generic does
# (arch/x86/lib/copy_user_64.S).
.section .text
.code64

# size_t arch_user_copy(void *dst, const void *src, size_t n): the bytes
# of the n not copied, 0 when all were.
.global arch_user_copy
.type arch_user_copy, @function
arch_user_copy:
    cld
    movq %rdx, %rcx
    shrq $3, %rcx
    andl $7, %edx
.global arch_user_copy_words
arch_user_copy_words:
    rep movsq
    movl %edx, %ecx
.global arch_user_copy_bytes
arch_user_copy_bytes:
    rep movsb
    xorl %eax, %eax
    ret
.size arch_user_copy, .-arch_user_copy

# A fault in the words leaves RCX words and the RDX bytes after them; one in
# the bytes leaves RCX bytes.
.global arch_user_copy_words_fault
arch_user_copy_words_fault:
    leaq (%rdx,%rcx,8), %rax
    ret

.global arch_user_copy_bytes_fault
arch_user_copy_bytes_fault:
    movq %rcx, %rax
    ret

.section .note.GNU-stack,"",@progbits
