# drive.py: boot a Rach2 kernel over the GNU disk under nested KVM and run
# exits.sh around kvm-boot.sh with THP off and on, printing the console.
import os, socket, subprocess, sys, time
work, kernel, disk, kvm, pw = sys.argv[1:6]
sock = os.path.join(work, 'console.sock')
cpu = ('host,svm=on,npt=on,nrip-save=on,tsc-scale=on,vmcb-clean=on,flushbyasid=on,'
       'pause-filter=on,decodeassists=off,v-vmsave-vmload=off,vgif=off')
env = dict(os.environ, TMPDIR=work)
q = subprocess.Popen(['qemu-system-x86_64', '-accel', 'kvm', '-nodefaults', '-display', 'none',
    '-no-reboot', '-cpu', cpu, '-smp', '2', '-m', '4294967296B', '-kernel', kernel,
    '-drive', 'file=%s,format=raw,if=virtio,snapshot=on' % disk,
    '-drive', 'file=%s,format=raw,if=virtio,snapshot=on' % kvm,
    '-chardev', 'socket,id=c,path=%s,server=on,wait=on' % sock, '-serial', 'chardev:c',
    '-append', 'rach2.root=ext2 rach2.tscreliable'], env=env)
log = open(os.path.join(work, 'console.txt'), 'wb')
for i in range(100):
    if os.path.exists(sock): break
    time.sleep(0.1)
s = socket.socket(socket.AF_UNIX); s.connect(sock); s.settimeout(1.0)
buf = b''
def wait(tok, limit):
    global buf
    t = time.time()
    while tok.encode() not in buf:
        if time.time() - t > limit or q.poll() is not None:
            raise SystemExit('drive: no %r after %d s' % (tok, limit))
        try: d = s.recv(65536)
        except socket.timeout: continue
        if not d: raise SystemExit('drive: console closed')
        buf += d; log.write(d); log.flush()
    i = buf.index(tok.encode()) + len(tok); out = buf[:i]; buf = buf[i:]; return out
def send(t): s.sendall(t.encode() + b'\r')
n = [0]
def run(cmd, limit=900):
    n[0] += 1; tok = 'DRV%dEND' % n[0]
    send('%s; echo DRV%d""END $?' % (cmd, n[0]))
    out = wait(tok, limit) + wait('\n', 30)
    sys.stdout.write(out.decode('ascii', 'replace')); sys.stdout.flush()
try:
    wait('rach2 login', 600); send('root'); wait('assword', 60); send(pw); wait('# ', 60)
    run('stty -echo 2>/dev/null; uname -a')
    run('mkdir -p /tmp/kvm /tmp/kvm-data && mount -t ext2 -o ro /dev/disk1 /tmp/kvm-data && cp -R /tmp/kvm-data/. /tmp/kvm && umount /tmp/kvm-data')
    T = '/sys/kernel/mm/transparent_hugepage/enabled'
    boot = 'cd /tmp/kvm && sh exits.sh sh kvm-boot.sh -k guests/debian/vmlinuz -i guests/debian/initrd'
    for mode in ('never', 'madvise', 'never', 'madvise'):
        run('echo %s > %s && cat %s && grep -E "thp_fault_alloc|thp_collapse_alloc |thp_split_pmd" /proc/vmstat' % (mode, T, T))
        run(boot)
        run('grep -E "thp_fault_alloc|thp_collapse_alloc |thp_split_pmd" /proc/vmstat; grep AnonHuge /proc/meminfo')
    send('poweroff')
    time.sleep(10)
finally:
    if q.poll() is None: q.kill()
    q.wait()
