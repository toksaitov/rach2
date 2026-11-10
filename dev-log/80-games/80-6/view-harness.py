# Boot the GNU root under Rach2's kernel or under Debian's, restore Crysis's
# island checkpoint there, and keep the guest up for screenshots until the
# stop file appears.  Usage: 2653-view.py rach2|linux TURN [KERNEL]
import hashlib, os, pathlib, shlex, signal, socket, subprocess as S, sys, time

w = pathlib.Path(__file__).resolve().parent
o = pathlib.Path('/home/testing/rach2/build/worktrees/codex/build/runs/task2653')
ref = pathlib.Path('/home/testing/rach2/build/cache/debian-ref')
system, turn = sys.argv[1:3]
kernel = pathlib.Path(sys.argv[3]) if len(sys.argv) > 3 else o / 'kernel'
extra = os.environ.get('VIEW_APPEND', '')
gameargs = os.environ.get('VIEW_GAME', '+load default_Contact_crysis.CRYSISJMSF')
tag = f'{system}-{turn}'
s = socket.socket(); s.bind(('127.0.0.1', 0)); port = s.getsockname()[1]; s.close()
if system == 'linux':
    key, user = ref / 'id_ed25519', 'gfx'
else:
    key, user = w / 'key', 'root'
    if not key.exists():
        key.write_bytes((o / 'key').read_bytes())
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
    print('crysis-view:', *a, flush=True)


def stop(signum, frame):
    raise SystemExit(128 + signum)


for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
    signal.signal(sig, stop)
stopfile = w / f'stop-{tag}'
overlays = []
try:
    def overlay(name, base, fmt):
        p = w / f'{name}-{tag}.qcow2'
        if p.exists():
            p.unlink()
        S.run(['qemu-img', 'create', '-q', '-f', 'qcow2', '-F', fmt, '-b', str(base), str(p)], check=True)
        overlays.append(p)
        return p
    root = overlay('root', o / 'root-base.disk', 'raw')
    data = overlay('data', o / 'data.ext2', 'raw')
    console = open(w / f'serial-{tag}.txt', 'wb')
    common = ['-accel', 'kvm', '-cpu', 'host,invtsc=on', '-smp', '4', '-m', '4096', '-nodefaults', '-display', 'none', '-no-reboot']
    if system == 'linux':
        boot = overlay('debian', ref / 'prepared.qcow2', 'qcow2')
        args = ['qemu-system-x86_64'] + common + ['-machine', 'q35', '-drive', f'file={boot},if=none,id=boot,format=qcow2', '-device', 'virtio-blk-pci,drive=boot,bootindex=0', '-device', 'virtio-vga', '-device', 'virtio-rng-pci',
                '-drive', f'file={root},format=qcow2,if=none,id=root', '-device', 'ide-hd,drive=root,bus=ide.0',
                '-drive', f'file={data},format=qcow2,if=none,id=data', '-device', 'ide-hd,drive=data,bus=ide.1',
                '-drive', f'file={o}/runtime.ext2,format=raw,if=none,id=runtime,snapshot=on', '-device', 'ide-hd,drive=runtime,bus=ide.2',
                '-netdev', f'user,id=net,hostfwd=tcp:127.0.0.1:{port}-:22', '-device', 'virtio-net-pci,netdev=net', '-serial', 'stdio']
    else:
        say('kernel', hashlib.sha256(kernel.read_bytes()).hexdigest())
        args = ['qemu-system-x86_64'] + common + ['-kernel', str(kernel), '-append', ('rach2.root=ext2 rach2.ip=10.0.2.15/24 rach2.gateway=10.0.2.2 ' + extra).strip(),
                '-device', 'ich9-ahci,id=ahci', '-device', 'virtio-vga',
                '-drive', f'file={root},format=qcow2,if=none,id=root', '-device', 'ide-hd,drive=root,bus=ahci.0',
                '-drive', f'file={data},format=qcow2,if=none,id=data', '-device', 'ide-hd,drive=data,bus=ahci.1',
                '-drive', f'file={o}/runtime.ext2,format=raw,if=none,id=runtime,snapshot=on', '-device', 'ide-hd,drive=runtime,bus=ahci.2',
                '-netdev', f'user,id=net,hostfwd=tcp:127.0.0.1:{port}-:22', '-device', 'rtl8139,netdev=net', '-serial', 'stdio']
    args += ['-qmp', f'unix:{w}/qmp-{tag}.sock,server=on,wait=off']
    q = S.Popen(args, stdout=console, stderr=S.STDOUT, stdin=S.DEVNULL)
    (w / f'qemu-{tag}.txt').write_text(shlex.join(args) + '\n')
    (w / f'ssh-{tag}.txt').write_text(shlex.join(ssh) + '\n')
    say('booting', tag, 'port', port)
    for _ in range(180):
        if q.poll() is not None:
            raise RuntimeError('QEMU exited before SSH')
        if cmd('true', check=False, raw=True).returncode == 0:
            break
        time.sleep(1)
    else:
        raise RuntimeError('SSH never answered; read the serial output')
    say('SSH answered')
    if system == 'linux':
        say(cmd("""lsblk -o NAME,SIZE,TYPE; uname -a; R=$(lsblk -nrpo NAME,TYPE | awk '$2 == "part" && $1 ~ /sd/ { print $1 }'); D=$(lsblk -bdnpo NAME,SIZE | awk '$2 == 15032385536 { print $1 }'); T=$(lsblk -bdnpo NAME,SIZE | awk '$2 == 268435456 { print $1 }'); echo root $R data $D runtime $T; sudo mkdir -p /gnu && { sudo mount -t ext4 $R /gnu || { sudo dmesg | tail -8; sudo blkid; false; }; } && ls /gnu && sudo mkdir -p /gnu/dev /gnu/proc /gnu/sys /gnu/tmp && sudo mount --rbind /dev /gnu/dev && sudo mount -t proc proc /gnu/proc && sudo mount -t sysfs sys /gnu/sys && sudo mkdir -p /gnu/data /gnu/tmp/runtime && sudo mount -t ext2 $D /gnu/data && sudo mount -t ext2 -o ro $T /gnu/tmp/runtime && echo mounted""", raw=True).stdout.decode())
        say(cmd('grep -e " /data " -e " / " -e runtime /proc/mounts; uname -a').stdout.decode())
    else:
        say(cmd('mkdir -p /data /tmp/runtime; mount -t ext2 /dev/disk1 /data; mount -t ext2 -o ro /dev/disk2 /tmp/runtime; cat /proc/cmdline; grep -e " /data " -e " / " /proc/mounts; uname -a').stdout.decode())
    for src, dst in [('crysis.sh', '/usr/bin/crysis.sh'), ('rach2-game.sh', '/usr/bin/rach2-game'), ('game.sha256', '/usr/share/rach2/crysis/game.sha256'), ('crysis-installers.sha256', '/usr/share/rach2/crysis/crysis-installers.sha256')]:
        cmd('mkdir -p /usr/share/rach2/crysis; cat > ' + shlex.quote(dst), (o / src).read_bytes())
    cmd('chmod 755 /usr/bin/crysis.sh /usr/bin/rach2-game')
    cmd('tar xzf - -C /', (o / 'games-layer.tar.gz').read_bytes())
    cmd('tar xf - -C /opt/proton/files/lib/wine', (o / 'dinput-new.tar').read_bytes())
    say(cmd('ldconfig -X; sha256sum /opt/proton/files/lib/wine/x86_64-windows/dinput8.dll /data/crysis/saves/SaveGames/*; ls -la /data/crysis/saves/SaveGames', check=False).stdout.decode())
    if turn.startswith('probe'):
        probe = os.environ.get('VIEW_PROBE', 'maxrss')
        cmd(f'cat > /tmp/{probe}.c', (w / f'{probe}.c').read_bytes())
        r = cmd(f'gcc -O2 -o /tmp/{probe} /tmp/{probe}.c && /tmp/{probe} /{probe}.scratch', check=False)
        say('probe status', r.returncode)
        print(r.stdout.decode(errors='replace'), flush=True)
        cmd('sync; umount /tmp/runtime; umount /data', check=False)
        raise SystemExit(r.returncode)
    for name in ('2653-key.py', '2653-screen.py'):
        cmd('cat > /tmp/' + name, (o / name).read_bytes())
    say(cmd('chown -R test:test /data/crysis; LD_LIBRARY_PATH=/tmp/runtime/xvfb/lib /tmp/runtime/xvfb/Xvfb :99 -ac -noreset -screen 0 640x480x24 -nolisten tcp > /tmp/xvfb.log 2>&1 < /dev/null &').stdout.decode())
    for _ in range(30):
        if cmd('test -S /tmp/.X11-unix/X99', check=False).returncode == 0:
            break
        time.sleep(1)
    else:
        raise RuntimeError(cmd('cat /tmp/xvfb.log').stdout.decode())
    say(cmd('mkdir -p /tmp/task2653-runtime; chown test:test /tmp/task2653-runtime; chmod 700 /tmp/task2653-runtime; su -s /bin/sh test -c "XDG_RUNTIME_DIR=/tmp/task2653-runtime pulseaudio -n --daemonize --exit-idle-time=-1 --load=module-null-sink --load=\\"module-native-protocol-unix socket=/tmp/task2653-pulse auth-anonymous=1\\" --log-target=file:/tmp/task2653-pulse.log"').stdout.decode())
    game = 'env PULSE_SERVER=unix:/tmp/task2653-pulse DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe LP_NUM_THREADS=4 PROTON_USE_XALIA=0 MESA_SHADER_CACHE_SHOW_STATS=true ' + os.environ.get('VIEW_ENV', 'WINEDEBUG=-all') + ' crysis.sh run -devmode +r_Width 640 +r_Height 480 +sys_spec 1 +g_skipIntro 1 ' + gameargs
    say('starting game:', game)
    with open(w / f'game-{tag}.out', 'wb') as log:
        child = S.Popen(ssh + [wrap('su -s /bin/sh test -c ' + shlex.quote(game))], stdout=log, stderr=S.STDOUT, stdin=S.DEVNULL)
        started = time.monotonic()
        while child.poll() is None and not stopfile.exists():
            time.sleep(15)
            progress = cmd("wc -c /data/crysis/game/Game.log 2>/dev/null; tail -n 2 /data/crysis/game/Game.log 2>/dev/null", check=False).stdout
            say(int(time.monotonic() - started), 's:', progress.decode(errors='replace').replace('\n', ' | '))
        if child.poll() is None:
            say('stop requested')
            cmd('python3 /tmp/2653-key.py --close', check=False)
            try:
                child.wait(timeout=60)
            except S.TimeoutExpired:
                cmd('su -s /bin/sh test -c "WINEPREFIX=/data/crysis/compat/pfx /opt/proton/files/bin/wineserver -k"', check=False)
                child.terminate()
                child.wait()
                say('the game did not close by itself; stopped attempt')
        say('game exit', child.returncode)
    (w / f'Game-{tag}.log').write_bytes(cmd('cat /data/crysis/game/Game.log', check=False).stdout)
    (w / f'evidence-{tag}.tar.gz').write_bytes(cmd('tar czf - -C /data/crysis logs saves', check=False).stdout)
finally:
    if q is not None and q.poll() is None:
        try:
            cmd('killall pulseaudio; killall Xvfb; su -s /bin/sh test -c "WINEPREFIX=/data/crysis/compat/pfx /opt/proton/files/bin/wineserver -k"; sync; umount /tmp/runtime; umount /data', check=False)
            cmd('sudo poweroff' if system == 'linux' else 'poweroff -f', check=False, raw=True)
            q.wait(timeout=60)
            say('guest powered off')
        except Exception:
            q.terminate()
            q.wait()
            say('QEMU terminated')
    for p in overlays:
        if p.exists():
            p.unlink()
    if stopfile.exists():
        stopfile.unlink()
    say('ended', tag)
