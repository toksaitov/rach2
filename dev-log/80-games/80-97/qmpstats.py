# Capture each load's own KVM vCPU counters through its QMP socket.
# Usage: qmpstats.py WORKDIR STOPFILE [TRACE_INSTANCE]
import json
import pathlib
import socket
import sys
import time

work, stop = map(pathlib.Path, sys.argv[1:3])
trace = pathlib.Path(sys.argv[3]) if len(sys.argv) > 3 else None
triggers = {'kvm_msr': 'hist:keys=common_pid.execname,ecx,write:sort=hitcount:size=2048',
            'kvm_exit': 'hist:keys=common_pid.execname,exit_reason:sort=hitcount:size=2048'}


def trigger(event, command):
    """Change only this observer's histogram in its own trace instance."""
    with (trace / 'events/kvm' / event / 'trigger').open('a') as stream:
        stream.write(command + '\n')


created = False
active = []
try:
    if trace:
        trace.mkdir()
        created = True
        for event, command in triggers.items():
            trigger(event, command)
            active.append((event, command))
    seen = set()
    while not stop.exists():
        for out in work.glob('out-*'):
            path = work / ('qmp-' + out.name[4:] + '.sock')
            if not path.exists():
                continue
            for side in 'ab':
                key = (out.name, side)
                if key in seen or not (out / ('host-' + side + '.txt')).exists():
                    continue
                try:
                    with socket.socket(socket.AF_UNIX) as sk:
                        sk.settimeout(3)
                        sk.connect(str(path))
                        with sk.makefile('rw') as stream:
                            stream.readline()
    
                            def ask(command, **arguments):
                                """Read the reply to one QMP query, passing events over."""
                                stream.write(json.dumps({'execute': command, 'arguments': arguments}) + '\n')
                                stream.flush()
                                while True:
                                    reply = json.loads(stream.readline())
                                    if 'return' in reply or 'error' in reply:
                                        return reply
    
                            ask('qmp_capabilities')
                            if trace:
                                if side == 'a':
                                    for event, command in triggers.items():
                                        trigger(event, command + ':clear')
                                else:
                                    (out / 'kvm-hist.txt').write_text(''.join(
                                        '== %s\n%s' % (event, (trace / 'events/kvm' / event / 'hist').read_text())
                                        for event in triggers))
                            reply = ask('query-stats', target='vcpu', providers=[{'provider': 'kvm'}])
                    (out / ('qmp-' + side + '.json')).write_text(json.dumps(reply, indent=2) + '\n')
                    seen.add(key)
                except (OSError, ValueError):
                    pass
        time.sleep(.02)
finally:
    if trace and created:
        for event, command in reversed(active):
            trigger(event, '!' + command)
        trace.rmdir()
