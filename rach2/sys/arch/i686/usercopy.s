# The copy between kernel and user memory that takes no lock (mm/usercopy.c).
# A fault in either string move that the page fault handler cannot resolve
# resumes at its fixup (`arch_user_copy_fixup`, arch/x86/cpu.c), which
# returns the bytes not copied, as Linux's copy_user_generic does.
.text

# size_t arch_user_copy(void *dst, const void *src, size_t n): the bytes
# of the n not copied, 0 when all were.
.global arch_user_copy
.type arch_user_copy, @function
arch_user_copy:
    pushl %esi
    pushl %edi
    movl 12(%esp), %edi
    movl 16(%esp), %esi
    movl 20(%esp), %edx
    cld
    movl %edx, %ecx
    shrl $2, %ecx
    andl $3, %edx
.global arch_user_copy_words
arch_user_copy_words:
    rep movsl
    movl %edx, %ecx
.global arch_user_copy_bytes
arch_user_copy_bytes:
    rep movsb
    xorl %eax, %eax
    popl %edi
    popl %esi
    ret
.size arch_user_copy, .-arch_user_copy

# A fault in the words leaves ECX words and the EDX bytes after them, which
# the bytes then copy one at a time from the faulting word, so that the copy
# ends at the exact byte, as Linux's clear_user_original retries a faulting
# quadword byte by byte (arch/x86/lib/clear_page_64.S). A fault in the bytes
# leaves ECX bytes.
.global arch_user_copy_words_fault
arch_user_copy_words_fault:
    leal (%edx,%ecx,4), %ecx
    jmp arch_user_copy_bytes

.global arch_user_copy_bytes_fault
arch_user_copy_bytes_fault:
    movl %ecx, %eax
    popl %edi
    popl %esi
    ret
