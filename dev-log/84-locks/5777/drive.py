#!/usr/bin/env python3
# drive.py: boot the GNU test disk on the virgl profile under KVM, as the
# machine.virgl cases boot it, log in as root on the serial console, run a
# guest script given on the command line, print the console to stdout and
# stop QEMU once the guest prints its end marker or powers off.
#
#   drive.py KERNEL DISK SCRIPT [CPUS [MIB]]
#
# The guest's root password is the tree's krffs/rach2_demo_password, and the
# overlay and console socket go in a directory under the tree's build/tmp.
import os, signal, socket, subprocess, sys, tempfile, time
signal.signal(signal.SIGTERM, lambda *a: sys.exit("drive: terminated"))

kernel, disk, script = sys.argv[1], sys.argv[2], sys.argv[3]
cpus = sys.argv[4] if len(sys.argv) > 4 else "8"
mib = sys.argv[5] if len(sys.argv) > 5 else "8192"
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..")
password = open(os.path.join(root, "krffs", "rach2_demo_password")).read().strip()
os.makedirs(os.path.join(root, "build", "tmp"), exist_ok=True)
work = tempfile.mkdtemp(prefix="rach2-t5777.", dir=os.path.join(root, "build", "tmp"))
overlay = os.path.join(work, "overlay.qcow2")
sock = os.path.join(work, "console")
subprocess.check_call(["qemu-img", "create", "-q", "-f", "qcow2", "-F", "raw", "-b", os.path.abspath(disk), overlay])
env = dict(os.environ, GALLIUM_DRIVER="d3d12", MESA_D3D12_DEFAULT_ADAPTER_NAME="NVIDIA")
argv = ["qemu-system-x86_64", "-accel", "kvm", "-nodefaults",
        "-display", "egl-headless,rendernode=/dev/dri/renderD128", "-no-reboot",
        "-cpu", "host,invtsc=on", "-smp", cpus, "-m", mib + "M",
        "-vga", "none", "-device", "virtio-vga-gl,edid=on,xres=1280,yres=800",
        "-kernel", kernel, "-append", "rach2.root=ext2",
        "-drive", "file=%s,if=none,id=d0,format=qcow2,cache=unsafe" % overlay,
        "-device", "virtio-blk-pci,drive=d0",
        "-chardev", "socket,id=c0,path=%s,server=on,wait=on" % sock,
        "-serial", "chardev:c0"]
q = subprocess.Popen(argv, env=env)
s = None
try:
    for _ in range(200):
        if os.path.exists(sock):
            break
        time.sleep(0.1)
    s = socket.socket(socket.AF_UNIX)
    s.connect(sock)
    s.settimeout(1.0)
    buf = b""

    def pump():
        global buf
        try:
            d = s.recv(65536)
        except socket.timeout:
            return True
        if not d:
            return False
        sys.stdout.buffer.write(d)
        sys.stdout.flush()
        buf += d
        return True

    def expect(token, limit=600):
        global buf
        end = time.time() + limit
        while time.time() < end:
            i = buf.find(token)
            if i >= 0:
                buf = buf[i + len(token):]
                return True
            if not pump():
                return False
        return False

    if not expect(b"login: ", 900):
        sys.exit("drive: no login prompt")
    s.sendall(b"root\n")
    expect(b"assword: ", 60)
    s.sendall(password.encode() + b"\n")
    if not expect(b"# ", 120):
        sys.exit("drive: no shell")
    s.sendall(b"stty -echo; export PS1='# '\n")
    time.sleep(1)
    with open(script, "rb") as f:
        body = f.read()
    s.sendall(b"cat > /tmp/job.sh <<'T5777EOF'\n" + body + b"T5777EOF\n")
    s.sendall(b"sh /tmp/job.sh; echo T5777-END $?\n")
    while pump():
        if b"T5777-END" in buf and buf.rstrip().endswith(b"#"):
            break
        if b"T5777-END" in buf[-200:] and b"\n" in buf[buf.rfind(b"T5777-END"):]:
            break
        if q.poll() is not None:
            break
        if len(buf) > 1 << 20:
            buf = buf[-4096:]
finally:
    if s is not None:
        s.close()
    if q.poll() is None:
        q.terminate()
        try:
            q.wait(30)
        except subprocess.TimeoutExpired:
            q.kill()
            q.wait()
    for f in (overlay, sock):
        try:
            os.unlink(f)
        except OSError:
            pass
    os.rmdir(work)
