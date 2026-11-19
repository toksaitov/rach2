# boot.sh KERNEL INITRD OUT [APPEND]: one bench boot, four processors, KVM.
k=$1 i=$2 o=$3
if [ -n "${4:-}" ]; then
    timeout 900 qemu-system-x86_64 -accel kvm -cpu max -smp 4 -m 2048 -display none -no-reboot -kernel "$k" -initrd "$i" -append "$4" -serial file:"$o" -monitor none
else
    timeout 900 qemu-system-x86_64 -accel kvm -cpu max -smp 4 -m 2048 -display none -no-reboot -kernel "$k" -initrd "$i" -serial file:"$o" -monitor none
fi
echo "boot: $o: qemu $?"
