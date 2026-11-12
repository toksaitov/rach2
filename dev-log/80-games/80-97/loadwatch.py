# Notify the host at Crysis's existing load markers, without polling the log.
import ctypes
import json
import os
import pathlib
import time

libc = ctypes.CDLL(None, use_errno=True)
fd = libc.inotify_init1(0)
if fd < 0:
    raise OSError(ctypes.get_errno(), 'inotify_init1')
try:
    directory = b'/data/crysis/game'
    if libc.inotify_add_watch(fd, directory, 0x182) < 0:
        raise OSError(ctypes.get_errno(), 'inotify_add_watch')
    path = pathlib.Path(os.fsdecode(directory)) / 'Game.log'
    offset = 0
    text = ''
    stage = 0
    print('READY', flush=True)
    while stage < 2:
        os.read(fd, 65536)
        try:
            with path.open('rb') as stream:
                stream.seek(offset)
                chunk = stream.read()
                offset += len(chunk)
        except FileNotFoundError:
            continue
        text = (text + chunk.decode(errors='replace'))[-65536:]
        markers = ('Successfully activated profile', '*LOADING')
        while stage < 2 and markers[stage] in text:
            interrupts = pathlib.Path('/proc/interrupts').read_text()
            total = 0
            found = False
            for row in interrupts.splitlines():
                if row.lstrip().startswith('LOC:') or (row.startswith('32:') and row.split()[-1] == 'timer'):
                    found = True
                    for field in row.split(':', 1)[1].split():
                        if not field.isdigit():
                            break
                        total += int(field)
            if not found:
                raise RuntimeError('no local timer counter')
            print(json.dumps({'stage': 'ab'[stage], 'guest_ns': time.monotonic_ns(),
                              'timer_interrupts': total}), flush=True)
            stage += 1
finally:
    os.close(fd)
