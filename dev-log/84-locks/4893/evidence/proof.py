import os, sys, subprocess, select, time
from pathlib import Path
# Boot the GNU root of task 4893 (84-167) through rach2.rootimage= on a loop device on
# an ext2 partition, as the Lenovo's trial does, with the probes and trip.sh's
# output on that partition's /data, the flusher set to write at once, and run
# trip.sh's first loop. Usage: qemu.py TAG KERNEL IMAGE_NAME
tag, kernel, image = sys.argv[1], sys.argv[2], sys.argv[3]
out = Path('build/runs/t4893-' + tag); out.mkdir(parents=True, exist_ok=True)
tmp = Path('build/tmp/4893-proof'); tmp.mkdir(parents=True, exist_ok=True)
cmd = ['qemu-system-x86_64', '-accel', 'kvm', '-cpu', 'host', '-smp', '4', '-m', '8192',
       '-kernel', kernel, '-append', 'rach2.rootimage=LABEL=trialdata:/trial4893/' + image + ' rach2.disk=ahci',
       '-drive', 'file=build/tmp/4893-proof/disk.img,format=raw,if=none,id=root',
       '-device', 'ich9-ahci,id=ahci', '-device', 'ide-hd,drive=root,bus=ahci.0',
       '-device', 'virtio-vga',
       '-display', 'none', '-serial', 'stdio', '-monitor', 'none', '-no-reboot', '-snapshot',
       '-netdev', 'user,id=net', '-device', 'e1000,netdev=net']
p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                     env=dict(os.environ, TMPDIR=str(tmp.resolve())))
log = open(out / 'console.txt', 'wb'); buf = b''; start = last = time.monotonic(); stage = 0
seen = {'panic': False, 'survived': False, 'trip': 0}
D = "/data/trial4893"
command = ("uname -a; cat /proc/cmdline; grep -e ' / ' -e ' /data ' /proc/mounts; ls -ld /sys/kernel/debug; "
           "echo 0 > /proc/sys/vm/dirty_expire_centisecs; echo 100 > /proc/sys/vm/dirty_writeback_centisecs; "
           "dd if=/dev/urandom of=/root/wbtest bs=4096 count=256; dd if=/dev/urandom of=" + D + "/wbtest bs=4096 count=256; "
           "sleep 10; echo WB_SURVIVED; "
           "sh " + D + "/probes/trip.sh -r llvmpipe -n 1 -t wined3d_cs " + D + "/out; echo TRIP_STATUS=$?; sync; "
           "ls -ld /sys/kernel/debug; grep -c debugfs /proc/mounts; grep -c '^thread' /sys/kernel/debug/bkl/calls; "
           "dmesg | grep -E 'panic|vm: fault|lockup|ext2.*error|sleeping device'; sync; echo ALL_DONE; poweroff -f\n")
try:
    while p.poll() is None:
        if time.monotonic() - start > 2700: raise RuntimeError("over 45 minutes")
        if not select.select([p.stdout], [], [], 1)[0]:
            if time.monotonic() - last > 60: raise RuntimeError("no console progress for 60 seconds")
            continue
        data = os.read(p.stdout.fileno(), 65536)
        if not data: break
        last = time.monotonic(); log.write(data); log.flush()
        sys.stdout.buffer.write(data); sys.stdout.buffer.flush(); buf = (buf + data)[-8192:]
        if any(b'panic:' in line and b'panic: timeout:' not in line for line in data.splitlines()):
            seen['panic'] = True
            raise RuntimeError('guest panic')
        if stage == 0 and b'rach2 login:' in buf:
            p.stdin.write(b'root\n'); p.stdin.flush(); stage = 1; buf = b''
        elif stage == 1 and b'Password:' in buf:
            p.stdin.write(Path('krffs/rach2_demo_password').read_bytes().strip() + b'\n'); p.stdin.flush(); stage = 2; buf = b''
        elif stage == 2 and b'# ' in buf:
            p.stdin.write(command.encode()); p.stdin.flush(); stage = 3; buf = b''
        if stage == 3 and b'\nWB_SURVIVED' in buf: seen['survived'] = True
        if stage == 3 and b'TRIP_STATUS=0\r\n' in buf: seen['trip'] += 1; buf = buf.replace(b'TRIP_STATUS=0\r\n', b'')
    p.wait(timeout=10)
finally:
    if p.poll() is None:
        p.terminate()
        try: p.wait(timeout=10)
        except subprocess.TimeoutExpired: p.kill(); p.wait()
    log.close()
    print('QEMU-RESULT', tag, seen)
