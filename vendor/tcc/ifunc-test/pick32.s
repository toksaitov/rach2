# The IFUNC pick of the i386 checks, as GNU as makes one: a resolver, also
# named pick_resolver, that returns triple, with the size of its code.
	.text
	.globl	pick, pick_resolver
	.type	pick, @gnu_indirect_function
	.type	pick_resolver, @function
pick_resolver:
pick:
	movl	$triple, %eax
	ret
	.size	pick, .-pick
	.size	pick_resolver, .-pick_resolver
