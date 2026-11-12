# Boot the GNU root under Rach2's kernel or under Debian's, restore Crysis's
# island checkpoint, time the level load with the kernel's counters around it,
# and power the guest off once the load ends.
# Usage: crysis-load.py rach2|linux TURN [KERNEL]
# VIEW_WARM=1 writes data-fresh.ext2 itself, which then becomes data-warm.ext2;
# VIEW_PROF=1 reads Rach2's bkl/calls and whole-kernel profile over the load,
# VIEW_PROFTID=1 with it, on a kernel profile-tid.py patched, profiles the
# game's main thread alone, or with VIEW_PROFNAME=NAME the first thread so named,
# VIEW_SAMPLE=N samples every processor's RIP N times over the load by QMP,
# VIEW_STRACE=1 runs the game under strace -f -c, or strace's options
# VIEW_STRACEO in place of -c -w, VIEW_TRACEP=NAMES counts
# the calls of every thread so named for VIEW_TRACET seconds of the load, game
# naming the main thread, with strace's options VIEW_TRACEO in place of -c -w,
# and VIEW_CPU gives QEMU's -cpu.
import collections, hashlib, json, os, pathlib, re, shlex, signal, socket, subprocess as S, sys, threading, time

w = pathlib.Path(__file__).resolve().parent
ref = pathlib.Path('/home/testing/rach2/build/cache/debian-ref')
system, turn = sys.argv[1:3]
kernel = pathlib.Path(sys.argv[3]) if len(sys.argv) > 3 else w / 'kernel-base'
extra = os.environ.get('VIEW_APPEND', '')
warm = os.environ.get('VIEW_WARM') == '1'
prof = os.environ.get('VIEW_PROF') == '1' and system == 'rach2'
nsample = int(os.environ.get('VIEW_SAMPLE', '0'))
tag = f'{system}-{turn}'
out = w / f'out-{tag}'
out.mkdir(exist_ok=True)
s = socket.socket(); s.bind(('127.0.0.1', 0)); port = s.getsockname()[1]; s.close()
if system == 'linux':
    key, user = ref / 'id_ed25519', 'gfx'
else:
    key, user = w / 'key', 'root'
    if not key.exists():
        key.write_bytes(pathlib.Path('/home/testing/rach2/build/worktrees/m3/build/runs/task2653/key').read_bytes())
    key.chmod(0o600)
ssh = ['ssh', '-i', str(key), '-p', str(port), '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=15', '-o', 'StrictHostKeyChecking=no', '-o', 'UserKnownHostsFile=/dev/null', '-o', 'LogLevel=ERROR', f'{user}@127.0.0.1']
clean = 'env -i PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin HOME=/root USER=root LOGNAME=root SHELL=/bin/sh '
q = None


def wrap(c):
    """Give the remote command that runs C as root in the GNU root."""
    if system == 'linux':
        return 'sudo ' + clean + 'chroot /gnu /bin/sh -c ' + shlex.quote(c)
    return c


def cmd(c, data=None, check=True, raw=False):
    r = S.run(ssh + [c if raw else wrap(c)], input=data, stdin=None if data is not None else S.DEVNULL, stdout=S.PIPE, stderr=S.STDOUT)
    if check and r.returncode != 0:
        raise RuntimeError('status %d from %s: %s' % (r.returncode, c, r.stdout.decode(errors='replace')))
    return r


def say(*a):
    print('crysis-load:', time.strftime('%H:%M:%S'), *a, flush=True)


def stop(signum, frame):
    raise SystemExit(128 + signum)


class Sampler(threading.Thread):
    """Sample every processor's RIP through QMP until told to stop."""

    def __init__(self):
        super().__init__(daemon=True)
        self.hits = collections.Counter()
        self.go = threading.Event()
        self.done = threading.Event()

    def run(self):
        sk = socket.socket(socket.AF_UNIX)
        sk.connect(str(w / f'qmp-{tag}.sock'))
        f = sk.makefile('rw')
        f.readline()

        def ask(c, **a):
            f.write(json.dumps({'execute': c, 'arguments': a}) + '\n')
            f.flush()
            while True:
                r = json.loads(f.readline())
                if 'return' in r or 'error' in r:
                    return r
        ask('qmp_capabilities')
        self.go.wait()
        n = 0
        while not self.done.is_set() and n < nsample:
            text = ask('human-monitor-command', **{'command-line': 'info registers -a'})['return']
            for block in text.split('CPU#')[1:]:
                m = re.search(r'RIP=([0-9a-f]+)', block)
                c = re.search(r'CS =([0-9a-f]+)', block)
                h = re.search(r'HLT=(\d)', block)
                if not m or not c:
                    continue
                if h and h.group(1) == '1':
                    self.hits['halt'] += 1
                elif int(c.group(1), 16) & 3:
                    self.hits['user'] += 1
                else:
                    self.hits[m.group(1)] += 1
            n += 1
            time.sleep(0.05)


for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
    signal.signal(sig, stop)
overlays = []
try:
    def overlay(name, base, fmt):
        p = w / f'{name}-{tag}.qcow2'
        if p.exists():
            p.unlink()
        S.run(['qemu-img', 'create', '-q', '-f', 'qcow2', '-F', fmt, '-b', str(base), str(p)], check=True)
        overlays.append(p)
        return p
    root = overlay('root', w / 'root-base.disk', 'raw')
    if warm:
        datadrv = f'file={w}/data-fresh.ext2,format=raw,if=none,id=data'
    else:
        datadrv = f'file={overlay("data", w / "data-warm.ext2", "raw")},format=qcow2,if=none,id=data'
    console = open(out / 'serial.txt', 'wb')
    common = ['-accel', 'kvm', '-cpu', os.environ.get('VIEW_CPU', 'host,invtsc=on'), '-smp', '4', '-m', '4096', '-nodefaults', '-display', 'none', '-no-reboot']
    if system == 'linux':
        boot = overlay('debian', ref / 'prepared.qcow2', 'qcow2')
        args = ['qemu-system-x86_64'] + common + ['-machine', 'q35', '-drive', f'file={boot},if=none,id=boot,format=qcow2', '-device', 'virtio-blk-pci,drive=boot,bootindex=0', '-device', 'virtio-vga', '-device', 'virtio-rng-pci',
                '-drive', f'file={root},format=qcow2,if=none,id=root', '-device', 'ide-hd,drive=root,bus=ide.0',
                '-drive', datadrv, '-device', 'ide-hd,drive=data,bus=ide.1',
                '-drive', f'file={w}/runtime.ext2,format=raw,if=none,id=runtime,snapshot=on', '-device', 'ide-hd,drive=runtime,bus=ide.2',
                '-netdev', f'user,id=net,hostfwd=tcp:127.0.0.1:{port}-:22', '-device', 'virtio-net-pci,netdev=net', '-serial', 'stdio']
    else:
        say('kernel', kernel, hashlib.sha256(kernel.read_bytes()).hexdigest())
        args = ['qemu-system-x86_64'] + common + ['-kernel', str(kernel), '-append', ('rach2.root=ext2 rach2.ip=10.0.2.15/24 rach2.gateway=10.0.2.2 ' + extra).strip(),
                '-device', 'ich9-ahci,id=ahci', '-device', 'virtio-vga',
                '-drive', f'file={root},format=qcow2,if=none,id=root', '-device', 'ide-hd,drive=root,bus=ahci.0',
                '-drive', datadrv, '-device', 'ide-hd,drive=data,bus=ahci.1',
                '-drive', f'file={w}/runtime.ext2,format=raw,if=none,id=runtime,snapshot=on', '-device', 'ide-hd,drive=runtime,bus=ahci.2',
                '-netdev', f'user,id=net,hostfwd=tcp:127.0.0.1:{port}-:22', '-device', 'rtl8139,netdev=net', '-serial', 'stdio']
    args += ['-qmp', f'unix:{w}/qmp-{tag}.sock,server=on,wait=off']
    q = S.Popen(args, stdout=console, stderr=S.STDOUT, stdin=S.DEVNULL)
    (out / 'qemu.txt').write_text(shlex.join(args) + '\n')
    say('booting', tag, 'port', port)
    for _ in range(240):
        if q.poll() is not None:
            raise RuntimeError('QEMU exited before SSH')
        if cmd('true', check=False, raw=True).returncode == 0:
            break
        time.sleep(1)
    else:
        raise RuntimeError('SSH never answered; read the serial output')
    say('SSH answered')
    if system == 'linux':
        say(cmd("""R=$(lsblk -nrpo NAME,TYPE | awk '$2 == "part" && $1 ~ /sd/ { print $1 }'); D=$(lsblk -bdnpo NAME,SIZE | awk '$2 == 15032385536 { print $1 }'); T=$(lsblk -bdnpo NAME,SIZE | awk '$2 == 268435456 { print $1 }'); echo root $R data $D runtime $T; sudo mkdir -p /gnu && sudo mount -t ext4 $R /gnu && sudo mkdir -p /gnu/dev /gnu/proc /gnu/sys /gnu/tmp && sudo mount --rbind /dev /gnu/dev && sudo mount -t proc proc /gnu/proc && sudo mount -t sysfs sys /gnu/sys && sudo mkdir -p /gnu/data /gnu/tmp/runtime && sudo mount -t ext2 $D /gnu/data && sudo mount -t ext2 -o ro $T /gnu/tmp/runtime && echo mounted; uname -a""", raw=True).stdout.decode())
    else:
        say(cmd('mkdir -p /data /tmp/runtime; mount -t ext2 /dev/disk1 /data; mount -t ext2 -o ro /dev/disk2 /tmp/runtime; cat /proc/cmdline; grep -e " /data " -e " / " /proc/mounts; uname -a').stdout.decode())
    cmd('tar xf - -C /opt/proton/files/lib/wine', (w / 'dinput-new.tar').read_bytes())
    say(cmd('sha256sum /opt/proton/files/lib/wine/x86_64-windows/dinput8.dll /data/crysis/saves/SaveGames/*.CRYSISJMSF; du -sk /data/crysis/compat /data/crysis/cache 2>&1; nproc; free -m', check=False).stdout.decode())
    for name in ('2653-key.py', '2653-screen.py'):
        cmd('cat > /tmp/' + name, (w / name).read_bytes())
    cmd('chown -R test:test /data/crysis; LD_LIBRARY_PATH=/tmp/runtime/xvfb/lib /tmp/runtime/xvfb/Xvfb :99 -ac -noreset -screen 0 640x480x24 -nolisten tcp > /tmp/xvfb.log 2>&1 < /dev/null &')
    for _ in range(30):
        if cmd('test -S /tmp/.X11-unix/X99', check=False).returncode == 0:
            break
        time.sleep(1)
    else:
        raise RuntimeError(cmd('cat /tmp/xvfb.log').stdout.decode())
    cmd('mkdir -p /tmp/t-runtime; chown test:test /tmp/t-runtime; chmod 700 /tmp/t-runtime; su -s /bin/sh test -c "XDG_RUNTIME_DIR=/tmp/t-runtime pulseaudio -n --daemonize --exit-idle-time=-1 --load=module-null-sink --load=\\"module-native-protocol-unix socket=/tmp/t-pulse auth-anonymous=1\\" --log-target=file:/tmp/t-pulse.log"')
    cmd('rm -f /data/crysis/game/Game.log')
    if os.environ.get('VIEW_PRE'):
        say('pre:', cmd(os.environ['VIEW_PRE'], check=False).stdout.decode())
    game = 'env PULSE_SERVER=unix:/tmp/t-pulse DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe LP_NUM_THREADS=4 PROTON_USE_XALIA=0 MESA_SHADER_CACHE_SHOW_STATS=true ' + os.environ.get('VIEW_ENV', 'WINEDEBUG=-all') + ' crysis.sh run -devmode +r_Width 640 +r_Height 480 +sys_spec 1 +g_skipIntro 1 +load default_Contact_crysis.CRYSISJMSF'
    if os.environ.get('VIEW_STRACE') == '1':
        game = 'strace -f %s -o /tmp/strace-c.txt ' % os.environ.get('VIEW_STRACEO', '-c -w') + game
    sampler = Sampler() if nsample else None
    if sampler:
        sampler.start()
    say('starting game:', game)
    stat = 'cat /proc/stat; cat /proc/vmstat 2>/dev/null; cat /proc/meminfo | head -8; grep -i huge /proc/meminfo; cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null; grep thp_fault /proc/vmstat 2>/dev/null; cat /proc/diskstats 2>/dev/null; for t in /proc/[0-9]*/task/[0-9]*; do read l < $t/stat 2>/dev/null && echo "task $t $l"; done'
    with open(out / 'game.out', 'wb') as log:
        child = S.Popen(ssh + [wrap('su -s /bin/sh test -c ' + shlex.quote(game))], stdout=log, stderr=S.STDOUT, stdin=S.DEVNULL)
        started = time.monotonic()
        phase = 0
        last = 0
        while child.poll() is None and time.monotonic() - started < float(os.environ.get('VIEW_LIMIT', '600')):
            time.sleep(1)
            g = cmd('cat /data/crysis/game/Game.log 2>/dev/null', check=False).stdout.decode(errors='replace')
            el = time.monotonic() - started
            if phase == 0 and 'Successfully activated profile' in g:
                phase = 1
                t1 = el
                pre = ''
                if prof:
                    pre = 'echo 1 > /sys/kernel/debug/bkl/calls; echo kernel > /sys/kernel/debug/bkl/profile; '
                    if os.environ.get('VIEW_PROFTID') == '1':
                        # The game's main thread, or with VIEW_PROFNAME the first thread of that name.
                        name = os.environ.get('VIEW_PROFNAME')
                        find = ('G=$(grep -l "^%s$" /proc/[0-9]*/task/[0-9]*/comm 2>/dev/null | head -1 | cut -d/ -f5); ' % name if name else
                                'G=$(for p in /proc/[0-9]*; do echo "$(ls $p/task 2>/dev/null | wc -l) ${p#/proc/}"; done | sort -n | tail -1 | cut -d" " -f2); ')
                        pre = (find + 'echo "profile thread $G"; '
                               'echo 1 > /sys/kernel/debug/bkl/calls; echo "kernel $G" > /sys/kernel/debug/bkl/profile; ')
                (out / 'stat-a.txt').write_bytes(cmd(pre + stat, check=False).stdout)
                if os.environ.get('VIEW_TRACEP'):
                    tr = ('G=$(for p in /proc/[0-9]*; do echo "$(ls $p/task 2>/dev/null | wc -l) ${p#/proc/}"; done | sort -n | tail -1 | cut -d" " -f2); '
                          'for n in %s; do if [ $n = game ]; then t="-p $G"; else t=$(grep -l "^$n\\$" /proc/[0-9]*/task/[0-9]*/comm 2>/dev/null | cut -d/ -f5 | sed "s/^/-p /" | tr "\\n" " "); fi; '
                          '[ -n "$t" ] && (timeout -s INT %s strace %s $t -o /tmp/tp-$n.txt > /tmp/tp-$n.err 2>&1 &); echo "$n $t"; done' % (os.environ['VIEW_TRACEP'], os.environ.get('VIEW_TRACET', '6'), os.environ.get('VIEW_TRACEO', '-c -w')))
                    say('tracing', cmd(tr, check=False).stdout.decode())
                if sampler:
                    sampler.go.set()
                say('load began at %.0f s' % el)
            if phase == 1 and '*LOADING' in g:
                phase = 2
                if sampler:
                    sampler.done.set()
                (out / 'stat-b.txt').write_bytes(cmd(stat, check=False).stdout)
                if prof:
                    (out / 'bkl-calls.txt').write_bytes(cmd('cat /sys/kernel/debug/bkl/calls', check=False).stdout)
                    (out / 'bkl-profile.txt').write_bytes(cmd('cat /sys/kernel/debug/bkl/profile', check=False).stdout)
                say('load ended at %.0f s, %.0f s after the profile:' % (el, el - t1), [l for l in g.splitlines() if '*LOADING' in l])
                time.sleep(3)
                break
            if el - last >= 15:
                last = el
                say('%.0f s:' % el, g.strip().splitlines()[-1:] if g.strip() else '-')
        if sampler and phase >= 1:
            sampler.done.set()
            sampler.join(timeout=30)
            (out / 'rip.txt').write_text(''.join('%d %s\n' % (n, k) for k, n in sampler.hits.most_common()))
        if child.poll() is None:
            say('closing the game')
            cmd('python3 /tmp/2653-key.py --close', check=False)
            try:
                child.wait(timeout=60)
            except S.TimeoutExpired:
                cmd('su -s /bin/sh test -c "WINEPREFIX=/data/crysis/compat/pfx /opt/proton/files/bin/wineserver -k"', check=False)
                try:
                    child.wait(timeout=30)
                except S.TimeoutExpired:
                    child.terminate()
                    child.wait()
                say('the game did not close by itself')
        say('game exit', child.returncode)
    (out / 'Game.log').write_bytes(cmd('cat /data/crysis/game/Game.log', check=False).stdout)
    if os.environ.get('VIEW_STRACE') == '1':
        (out / 'strace-c.txt').write_bytes(cmd('cat /tmp/strace-c.txt', check=False).stdout)
    if os.environ.get('VIEW_TRACEP'):
        (out / 'tracep.txt').write_bytes(cmd('for f in /tmp/tp-*; do echo "== $f"; cat $f; done', check=False).stdout)
    if system == 'rach2':
        (out / 'dmesg.txt').write_bytes(cmd('dmesg', check=False).stdout)
finally:
    if q is not None and q.poll() is None:
        try:
            cmd('killall pulseaudio; killall Xvfb; su -s /bin/sh test -c "WINEPREFIX=/data/crysis/compat/pfx /opt/proton/files/bin/wineserver -k"; sleep 2; sync; umount /tmp/runtime; umount /data', check=False)
            cmd('sudo poweroff' if system == 'linux' else 'poweroff -f', check=False, raw=True)
            q.wait(timeout=90)
            say('guest powered off')
        except Exception:
            q.terminate()
            q.wait()
            say('QEMU terminated')
    for p in overlays:
        if p.exists():
            p.unlink()
    say('ended', tag)
