# Capture each load's own KVM vCPU counters through its QMP socket.
# Usage: qmpstats.py WORKDIR STOPFILE
import json
import pathlib
import socket
import sys
import time

work, stop = map(pathlib.Path, sys.argv[1:])
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
                        reply = ask('query-stats', target='vcpu', providers=[{'provider': 'kvm'}])
                (out / ('qmp-' + side + '.json')).write_text(json.dumps(reply, indent=2) + '\n')
                seen.add(key)
            except (OSError, ValueError):
                pass
    time.sleep(.02)
