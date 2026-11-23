"""Boot Rach2's os-real RAM image in QEMU and run barecall on its console.

Usage: serial.py KERNEL IMAGE BARECALL PASSWORD_FILE LOG [ARG...]

Logs in as root on the serial console, writes BARECALL to /tmp through
base64, and runs it on itself with ARG, or runs the shell command
BARECALL_CMD from the environment instead; BARECALL_QEMU adds QEMU
arguments. The console goes to LOG, and the guest is powered off once
barecall's last line has come. Each wait for the console has a limit, and
a wait that passes it ends the run with an error.
"""

import base64
import os
import socket
import subprocess
import sys
import tempfile
import time


def main():
    """Run one boot as the module's comment says."""
    kernel, image, program, password_file, log = sys.argv[1:6]
    args = " ".join(sys.argv[6:])
    with open(password_file) as f:
        password = f.read().strip()
    with open(program, "rb") as f:
        encoded = base64.encodebytes(f.read()).decode()
    scratch = tempfile.mkdtemp(dir=os.environ.get("TMPDIR"))
    path = os.path.join(scratch, "console")
    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-accel", "tcg,thread=multi", "-cpu", "max",
        "-smp", "4", "-m", "3G", "-machine", "q35", "-kernel", kernel,
        "-initrd", image, "-display", "none", "-vga", "none",
        "-monitor", "none", "-no-reboot", "-nic", "none",
        "-chardev", "socket,id=s0,path=%s,server=on,wait=on" % path,
        "-serial", "chardev:s0"] + os.environ.get("BARECALL_QEMU", "").split())
    out = open(log, "wb")
    try:
        for _ in range(100):
            if os.path.exists(path):
                break
            time.sleep(0.1)
        con = socket.socket(socket.AF_UNIX)
        con.connect(path)
        seen = bytearray()

        def until(token, limit=90, fail=None):
            """Copy the console to LOG until `token` comes, and drop what
            precedes it; stop on `fail`, a closed console or the limit."""
            print("serial: wait: %r" % token, file=sys.stderr, flush=True)
            end = time.time() + limit
            con.settimeout(5)
            while token.encode() not in seen:
                if time.time() > end or qemu.poll() is not None:
                    raise SystemExit("serial: no %r" % token)
                try:
                    data = con.recv(4096)
                except socket.timeout:
                    continue
                if not data:
                    raise SystemExit("serial: console closed")
                seen.extend(data)
                out.write(data)
                out.flush()
                if fail and fail.encode() in seen:
                    raise SystemExit("serial: %r" % fail)
            del seen[:seen.index(token.encode()) + len(token)]

        def send(line):
            """Type `line` and a return, eight bytes at a time."""
            data = line.encode() + b"\r"
            con.settimeout(60)
            for i in range(0, len(data), 8):
                con.sendall(data[i:i + 8])
                time.sleep(0.01)

        until("login: ", 300)
        send("root")
        until("assword: ")
        send(password)
        until("~ # ")
        send("PS1='# '")
        until("\n# ")
        send("base64 -d > /tmp/barecall <<'EOF'")
        for line in encoded.splitlines():
            send(line)
        send("EOF")
        until("\n# ")
        send("chmod 755 /tmp/barecall && uname -a")
        until("\n# ")
        send(os.environ.get("BARECALL_CMD", "/tmp/barecall /tmp/barecall " + args))
        until("barecall: openclose", 1800, "failed")
        until("\n# ")
        send("poweroff -f")
        qemu.wait(60)
    finally:
        if qemu.poll() is None:
            qemu.kill()
            qemu.wait()
        out.close()
        if os.path.exists(path):
            os.unlink(path)
        os.rmdir(scratch)


main()
