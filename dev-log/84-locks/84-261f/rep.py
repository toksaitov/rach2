#!/usr/bin/env python3
# rep.py: boot an x86-64 Rach2 kernel and GNU root on the virgl profile by
# hand, run loop.sh N times as the test account and keep the console in
# WORK/console.log. The kernel, the root's raw image and the root password
# are the files kernel, gnu.disk and password beside this file; for the
# counts of 84-261f they were run 343804's own objects.
# Usage: rep.py WORK ACCEL N
import os, socket, subprocess, sys, time, signal
work, accel, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
here = os.path.dirname(os.path.abspath(__file__))
kernel = os.path.join(here, "kernel")
disk = os.path.join(here, "gnu.disk")
password = open(os.path.join(here, "password")).read().strip()
loop = open(os.path.join(here, "loop.sh")).read()
os.makedirs(work, exist_ok=True)
log = open(os.path.join(work, "console.log"), "ab", buffering=0)
d0 = os.path.join(work, "disk0.qcow2")
if os.path.exists(d0):
    os.unlink(d0)
subprocess.check_call(["qemu-img", "create", "-q", "-f", "qcow2", "-b", disk, "-F", "raw", d0])
sock = os.path.join(work, "c")
if os.path.exists(sock):
    os.unlink(sock)
env = dict(os.environ, GALLIUM_DRIVER="d3d12", MESA_D3D12_DEFAULT_ADAPTER_NAME="NVIDIA")
cpu = "max,rdrand=on,rdseed=on,xsave=on,avx=on,enforce=on" if accel == "tcg" else "host,invtsc=on"
args = ["qemu-system-x86_64", "-accel", accel, "-nodefaults",
        "-display", "egl-headless,rendernode=/dev/dri/renderD128", "-no-reboot",
        "-cpu", cpu, "-smp", "8", "-m", "8G",
        "-device", "virtio-vga-gl,edid=on,xres=1280,yres=800",
        "-kernel", kernel, "-device", "ich9-ahci,id=ahci",
        "-drive", "file=%s,format=qcow2,if=none,id=d0" % d0,
        "-device", "ide-hd,drive=d0,bus=ahci.0",
        "-chardev", "socket,id=con,path=%s,server=on,wait=on" % sock,
        "-serial", "chardev:con", "-monitor", "none",
        "-append", "rach2.root=ext2"]
qerr = open(os.path.join(work, "qemu.stderr"), "ab")
q = subprocess.Popen(args, env=env, stdout=qerr, stderr=qerr, start_new_session=True)

def stop(*_):
    try:
        os.killpg(q.pid, signal.SIGTERM)
        q.wait(30)
    except Exception:
        try:
            os.killpg(q.pid, signal.SIGKILL)
        except Exception:
            pass
    try:
        os.unlink(d0)
    except OSError:
        pass
    sys.exit(0)

signal.signal(signal.SIGTERM, stop)
for _ in range(100):
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(sock)
        break
    except OSError:
        time.sleep(0.2)
else:
    stop()
s.settimeout(1.0)
buf = b""

def pump():
    global buf
    try:
        d = s.recv(65536)
    except socket.timeout:
        return
    if not d:
        stop()
    log.write(d)
    buf = (buf + d)[-400000:]

def expect(token, limit):
    global buf
    end = time.time() + limit
    while time.time() < end:
        if token in buf:
            buf = buf[buf.index(token) + len(token):]
            return True
        pump()
    return False

def send(text):
    s.sendall(text.encode())

if not expect(b"login:", 900):
    log.write(b"\nrep: no login prompt\n"); stop()
send("root\n")
expect(b"Password:", 60)
send(password + "\n")
if not expect(b"# ", 120):
    log.write(b"\nrep: no prompt\n"); stop()
send("stty -echo; export PS1='# '\n")
time.sleep(1)
send("cat > /tmp/loop.sh << 'LOOPEOF'\n")
for line in loop.splitlines():
    send(line + "\n")
    time.sleep(0.03)
send("LOOPEOF\n")
time.sleep(1)
send("u=$(getent passwd 1000 | cut -d: -f1); chmod 755 /tmp/loop.sh; su $u -c 'sh /tmp/loop.sh %d /tmp/loop.out'; echo LOOP-BEGIN; cat /tmp/loop.out; echo REP-DONE\n" % n)
end = time.time() + 4 * 3600
while time.time() < end:
    pump()
    if b"REP-DONE\r\n" in buf or b"REP-DONE\n" in buf:
        break
time.sleep(2)
pump()
log.write(("\nrep: end at %d\n" % time.time()).encode())
stop()
