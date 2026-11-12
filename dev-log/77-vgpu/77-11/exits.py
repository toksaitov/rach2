# Time each processor's exits to QEMU from profile.sh's TRACE, within the
# measured round. Usage: exits.py TRACE DIR: DIR/measured/stat-before and
# stat-after bound the round by their times. An exit runs from kvm_run_exit
# to the processor's next KVM_RUN; one in which QEMU took a virtio notify is
# counted by the queue notified, the rest by KVM's exit reason.
import collections, datetime, os, re, sys
KVM_RUN = 0xae80
REASONS = {2: 'io', 6: 'mmio', 7: 'irq window', 10: 'intr', 29: 'rdmsr', 30: 'wrmsr', 5: 'hlt'}
d = sys.argv[2]
lo = os.stat(d + '/measured/stat-before').st_mtime
hi = os.stat(d + '/measured/stat-after').st_mtime
line = re.compile(r'(\S+)Z (\w+) (.*)')
open_exit = {}          # cpu -> [start, reason, label]
count = collections.Counter(); total = collections.Counter()
notifies = collections.Counter(); submits = 0; fences = 0
for l in open(sys.argv[1]):
    m = line.match(l)
    if not m:
        continue
    t = datetime.datetime.fromisoformat(m.group(1) + '+00:00').timestamp()
    ev, rest = m.group(2), m.group(3)
    inside = lo <= t <= hi
    if ev == 'kvm_run_exit':
        f = re.match(r'cpu_index (\d+), reason (\d+)', rest)
        open_exit[int(f.group(1))] = [t, int(f.group(2)), None]
    elif ev == 'kvm_vcpu_ioctl':
        f = re.match(r'cpu_index (\d+), type (0x[0-9a-f]+)', rest)
        cpu, typ = int(f.group(1)), int(f.group(2), 16)
        if typ == KVM_RUN and cpu in open_exit:
            start, reason, label = open_exit.pop(cpu)
            if lo <= start <= hi:
                k = label or REASONS.get(reason, 'reason %d' % reason)
                count[k] += 1
                total[k] += t - start
    elif ev == 'virtio_queue_notify' and inside:
        f = re.search(r'vdev (\S+) n (\d+)', rest)
        k = 'notify %s queue %s' % (f.group(1)[-4:], f.group(2))
        notifies[k] += 1
        for e in open_exit.values():
            if e[1] in (2, 6) and e[2] is None and e[0] <= t:
                e[2] = k
                break
    elif ev == 'virtio_gpu_cmd_ctx_submit' and inside:
        submits += 1
    elif ev == 'virtio_gpu_fence_ctrl' and inside:
        fences += 1
print('round %.1f s; gpu submissions %d, fenced commands %d' % (hi - lo, submits, fences))
print('%-24s %8s %10s %9s' % ('exit', 'count', 'total ms', 'mean us'))
for k, n in sorted(total.items(), key=lambda x: -x[1]):
    print('%-24s %8d %10.1f %9.1f' % (k, count[k], 1000 * n, 1e6 * n / count[k]))
