# The entry to a VMX guest, for vmx.c: a stub until the VMX backend
# replaces it.

.text

# int kvm_vmx_vmrun(void *vcpu)
#
# Return -EOPNOTSUPP (-95): this kernel cannot enter a guest yet.
.globl kvm_vmx_vmrun
kvm_vmx_vmrun:
    movl $-95, %eax
    ret
