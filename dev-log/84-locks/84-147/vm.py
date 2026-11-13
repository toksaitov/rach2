#!/usr/bin/env python3
# Boot Rach2's kernel on the GNU root, or Debian's kernel with the GNU root in
# a chroot, as 80-92's crysis-load.py boots them, run the loads of a script
# file one line at a time and print KVM's counters of the VM over each, from
# QMP's query-stats, with each line's own output.
# Usage: vm.py rach2|linux KERNEL SCRIPT OUT
import json, os, pathlib, re, shlex, socket, subprocess as S, sys, time

R = pathlib.Path('/home/testing/rach2')
ref = R / 'build/cache/debian-ref'
system, kernel, script, outp = sys.argv[1], sys.argv[2], sys.argv[3], pathlib.Path(sys.argv[4])
w = pathlib.Path(__file__).resolve().parent
tag = outp.stem
qmp_path = w / f'qmp-{tag}.sock'
ser_path = w / f'ser-{tag}.sock'
out = open(outp, 'w')


def say(*a):
    print(*a, flush=True)
    print(*a, file=out, flush=True)


overlays = []
kinst = []


def overlay(name, base, fmt):
    p = w / f'{name}-{tag}.qcow2'
    if p.exists():
        p.unlink()
    S.run(['qemu-img', 'create', '-q', '-f', 'qcow2', '-F', fmt, '-b', str(base), str(p)], check=True)
    overlays.append(p)
    return p


s = socket.socket(); s.bind(('127.0.0.1', 0)); port = s.getsockname()[1]; s.close()
root = overlay('root', R / 'build/x86_64/images/gnu-test.disk', 'raw')
common = ['-name', 'task4863,debug-threads=on', '-accel', 'kvm', '-cpu', os.environ.get('VIEW_CPU', 'host,invtsc=on'), '-smp', '4', '-m', '4096', '-nodefaults', '-display', 'none', '-no-reboot']
tools = f'file={w}/tools.ext2,format=raw,if=none,id=tools,snapshot=on'
if system == 'linux':
    boot = overlay('debian', ref / 'prepared.qcow2', 'qcow2')
    args = ['qemu-system-x86_64'] + common + ['-machine', 'q35', '-drive', f'file={boot},if=none,id=boot,format=qcow2', '-device', 'virtio-blk-pci,drive=boot,bootindex=0', '-device', 'virtio-vga', '-device', 'virtio-rng-pci',
            '-drive', f'file={root},format=qcow2,if=none,id=root', '-device', 'ide-hd,drive=root,bus=ide.0',
            '-drive', tools, '-device', 'ide-hd,drive=tools,bus=ide.1',
            '-netdev', f'user,id=net,hostfwd=tcp:127.0.0.1:{port}-:22', '-device', 'virtio-net-pci,netdev=net']
else:
    args = ['qemu-system-x86_64'] + common + ['-kernel', kernel, '-append', ('rach2.exits rach2.root=ext2 rach2.ip=10.0.2.15/24 rach2.gateway=10.0.2.2 ' + os.environ.get('VIEW_APPEND', '')).strip(),
            '-device', 'ich9-ahci,id=ahci', '-device', 'virtio-vga',
            '-drive', f'file={root},format=qcow2,if=none,id=root', '-device', 'ide-hd,drive=root,bus=ahci.0',
            '-drive', tools, '-device', 'ide-hd,drive=tools,bus=ahci.1',
            '-netdev', f'user,id=net,hostfwd=tcp:127.0.0.1:{port}-:22', '-device', 'rtl8139,netdev=net']
for p in (qmp_path, ser_path):
    if p.exists():
        p.unlink()
args += ['-qmp', f'unix:{qmp_path},server=on,wait=off', '-chardev', f'socket,id=ser,path={ser_path},server=on,wait=off', '-serial', 'chardev:ser']
q = S.Popen(args, stdin=S.DEVNULL)
try:
    for _ in range(50):
        if qmp_path.exists() and ser_path.exists():
            break
        time.sleep(0.1)
    qs = socket.socket(socket.AF_UNIX); qs.connect(str(qmp_path)); qf = qs.makefile('rw'); qf.readline()

    def ask(c, **a):
        qf.write(json.dumps({'execute': c, 'arguments': a}) + '\n'); qf.flush()
        while True:
            r = json.loads(qf.readline())
            if 'return' in r or 'error' in r:
                return r
    ask('qmp_capabilities')

    def stats():
        r = ask('query-stats', target='vcpu')['return']
        tot = {}
        per = []
        for v in r:
            d = {}
            for st in v['stats']:
                if isinstance(st['value'], int):
                    tot[st['name']] = tot.get(st['name'], 0) + st['value']
                    d[st['name']] = st['value']
            per.append(d)
        return tot, per

    ss = socket.socket(socket.AF_UNIX); ss.connect(str(ser_path)); ss.settimeout(1)
    serlog = open(w / f'serial-{tag}.txt', 'wb')
    buf = b''

    def expect(pat, limit=300):
        nonlocal_buf = globals()
        t0 = time.monotonic()
        while time.monotonic() - t0 < limit:
            m = re.search(pat, nonlocal_buf['buf'])
            if m:
                rest = nonlocal_buf['buf'][m.end():]
                got = nonlocal_buf['buf'][:m.end()]
                nonlocal_buf['buf'] = rest
                return got
            if q.poll() is not None:
                raise RuntimeError('QEMU exited')
            if b'soft lockup' in nonlocal_buf['buf'] or b'panic: stopped' in nonlocal_buf['buf']:
                raise RuntimeError('guest failed')
            try:
                d = ss.recv(65536)
            except socket.timeout:
                continue
            serlog.write(d); serlog.flush()
            nonlocal_buf['buf'] += d
        raise RuntimeError('timed out waiting for %r; have %r' % (pat, nonlocal_buf['buf'][-300:]))

    if system == 'rach2':
        expect(rb'login: ')
        ss.sendall(b'root\r')
        expect(rb'Password: ?')
        ss.sendall((R / 'krffs/rach2_demo_password').read_text().splitlines()[0].encode() + b'\r')
        expect(rb'# ')
        ss.sendall(b'stty -echo; PS1=; export PS1\r')
        time.sleep(1)
        expect(rb'.*', 1)

        def run(c):
            ss.sendall(('%s; echo __END__$?\r' % c).encode())
            got = expect(rb'__END__(\d+)', 3600)
            return got.decode(errors='replace')
        run('mkdir -p /t; mount -t ext2 /dev/disk1 /t')
    else:
        import threading

        def drain():
            while True:
                try:
                    d = ss.recv(65536)
                except socket.timeout:
                    continue
                except OSError:
                    return
                if not d:
                    return
                serlog.write(d); serlog.flush()
        threading.Thread(target=drain, daemon=True).start()
        key = ref / 'id_ed25519'
        ssh = ['ssh', '-i', str(key), '-p', str(port), '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=15', '-o', 'StrictHostKeyChecking=no', '-o', 'UserKnownHostsFile=/dev/null', '-o', 'LogLevel=ERROR', 'gfx@127.0.0.1']
        for _ in range(120):
            if S.run(ssh + ['true'], stdin=S.DEVNULL, stdout=S.DEVNULL, stderr=S.DEVNULL).returncode == 0:
                break
            time.sleep(1)
        else:
            raise RuntimeError('no ssh')
        setup = """R=$(lsblk -nrpo NAME,TYPE | awk '$2 == "part" && $1 ~ /sd/ { print $1 }'); T=$(lsblk -bdnpo NAME,SIZE | awk '$2 == 67108864 { print $1 }'); sudo mkdir -p /gnu && sudo mount -t ext4 $R /gnu && sudo mkdir -p /gnu/dev /gnu/proc /gnu/sys /gnu/tmp /gnu/t && sudo mount --rbind /dev /gnu/dev && sudo mount -t proc proc /gnu/proc && sudo mount -t sysfs sys /gnu/sys && sudo mount -t ext2 $T /gnu/t && echo mounted"""
        r = S.run(ssh + [setup], stdin=S.DEVNULL, stdout=S.PIPE, stderr=S.STDOUT)
        say(r.stdout.decode())

        def run(c):
            r = S.run(ssh + ['sudo env -i PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin HOME=/root chroot /gnu /bin/sh -c ' + shlex.quote(c)], stdin=S.DEVNULL, stdout=S.PIPE, stderr=S.STDOUT)
            return r.stdout.decode(errors='replace') + '__END__%d' % r.returncode
    say('booted', system, kernel)
    time.sleep(float(os.environ.get('VIEW_SETTLE', '5')))
    # The histogram keys KVM's threads by the PID the tracing kernel sees,
    # which in WSL is not the distribution's, so QMP's thread ids do not
    # name them; each thread that wrote is printed, in PID order.
    kev = pathlib.Path('/sys/kernel/tracing/instances/task4863/events/kvm/kvm_msr')
    khist = 'hist:keys=common_pid,ecx:sort=common_pid:size=256'
    kfilt = ' if write == 1 && (ecx == 2059 || ecx == 1760) && comm ~ \"T63*\"'
    def ktrig(t):
        with open(kev / 'trigger', 'a') as f:
            f.write(t + '\n')
    ktrig(khist + kfilt)
    kinst.append(1)
    def keoi():
        per, dl = {}, {}
        h = (kev / 'hist').read_text()
        (w / ('khist-' + tag + '.txt')).open('a').write(h)
        for l in h.splitlines():
            m = re.search(r'common_pid:\s*(\d+), ecx:\s*(\d+) \} hitcount:\s*(\d+)', l)
            if m:
                (per if int(m.group(2)) == 2059 else dl)[int(m.group(1))] = int(m.group(3))
        return [per[k] for k in sorted(per)], [dl[k] for k in sorted(dl)]
    keys = ['exits', 'halt_exits', 'io_exits', 'mmio_exits', 'irq_exits', 'irq_window_exits', 'request_irq_exits', 'nmi_window_exits', 'insn_emulation', 'hypercalls', 'irq_injections', 'halt_successful_poll', 'halt_wakeup', 'req_event']
    for line in pathlib.Path(script).read_text().splitlines():
        if not line.strip() or line.startswith('#'):
            continue
        a, _ = stats()
        ktrig(khist + ':clear' + kfilt)
        t0 = time.monotonic()
        o = run(line)
        dt = time.monotonic() - t0
        b, _ = stats()
        ke = keoi()
        d = {k: b.get(k, 0) - a.get(k, 0) for k in b}
        named = sum(d.get(k, 0) for k in ('halt_exits', 'io_exits', 'mmio_exits', 'irq_exits', 'irq_window_exits', 'request_irq_exits', 'nmi_window_exits', 'hypercalls'))
        say('== %s  %.2f s' % (line, dt))
        say(o.strip())
        say('   ' + ' '.join('%s=%d' % (k, d.get(k, 0)) for k in keys) + ' unnamed=%d' % (d.get('exits', 0) - named))
        say('   host eoi writes %d %s; tsc deadline writes %d %s' % (sum(ke[0]), ' '.join(map(str, ke[0])), sum(ke[1]), ' '.join(map(str, ke[1]))))
    if system == 'rach2':
        ss.sendall(b'umount /t; poweroff -f\r')
    else:
        S.run(ssh + ['sudo poweroff'], stdin=S.DEVNULL, stdout=S.DEVNULL, stderr=S.DEVNULL)
    q.wait(timeout=60)
finally:
    if kinst:
        with open(kev / 'trigger', 'a') as f:
            f.write('!' + khist + kfilt + '\n')
    if q.poll() is None:
        q.terminate()
        q.wait()
    for p in overlays + [qmp_path, ser_path]:
        if p.exists():
            p.unlink()
