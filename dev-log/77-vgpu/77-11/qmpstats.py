# qmpstats.py SOCKET OUT [PID]: every half second until QEMU goes, append to
# OUT one JSON line with the host's time, KVM's statistics of each virtual
# processor from QEMU's QMP socket SOCKET (query-stats), and, with PID, the
# name, user, system and guest ticks of each of QEMU's threads.
import glob
import json
import socket
import sys
import time


def connect(path):
    """The QMP socket at path, once QEMU has made it, or exit."""
    for _ in range(600):
        try:
            s = socket.socket(socket.AF_UNIX)
            s.connect(path)
            return s.makefile('rw')
        except OSError:
            time.sleep(0.5)
    sys.exit('qmpstats: no socket: %s' % path)


def command(f, name, arguments=None):
    """Send one command and return its answer, skipping events."""
    m = {'execute': name}
    if arguments:
        m['arguments'] = arguments
    f.write(json.dumps(m) + '\n')
    f.flush()
    while True:
        line = f.readline()
        if not line:
            raise OSError('qmpstats: QEMU closed the socket')
        r = json.loads(line)
        if 'return' in r or 'error' in r:
            return r


def threads(pid):
    """Each of QEMU's threads: its name, utime, stime and guest_time, in ticks."""
    th = {}
    for fn in glob.glob('/proc/%s/task/*/stat' % pid):
        try:
            with open(fn) as f:
                line = f.read()
        except OSError:
            continue
        name = line[line.index('(') + 1:line.rindex(')')]
        v = line[line.rindex(')') + 2:].split()
        th[fn.split('/')[4]] = [name, int(v[11]), int(v[12]), int(v[40])]
    return th


def main():
    f = connect(sys.argv[1])
    json.loads(f.readline())
    command(f, 'qmp_capabilities')
    with open(sys.argv[2], 'a') as out:
        try:
            while True:
                r = command(f, 'query-stats', {'target': 'vcpu'})
                row = {'t': time.time(), 'r': r.get('return')}
                if len(sys.argv) > 3:
                    row['th'] = threads(sys.argv[3])
                out.write(json.dumps(row) + '\n')
                out.flush()
                time.sleep(0.5)
        except (OSError, ValueError):
            pass


main()
