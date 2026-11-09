# The entry to an SVM guest, for svm.c: a stub until the SVM backend
# replaces it.

.text

# int kvm_svm_vmrun(void *vcpu)
#
# Return -EOPNOTSUPP (-95): this kernel cannot enter a guest yet.
.globl kvm_svm_vmrun
kvm_svm_vmrun:
    movl $-95, %eax
    ret
