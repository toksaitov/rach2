#!/usr/bin/env python3
"""Trace one exit or tmpfs unlink under a continuous silent audio stream."""
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time


def interrupted(signum, frame):
    """Leave through the cleanup on an interrupt or termination request."""
    raise KeyboardInterrupt


def snapshot(out, name):
    """Keep the audio event and recent long holds on the same TSC clock."""
    for source, suffix in (("/proc/audio", "audio"), ("/proc/uptime", "uptime"),
                           ("/proc/stat", "stat"), ("/proc/interrupts", "irqs"),
                           ("/sys/kernel/debug/bkl/long", "long")):
        (out / (name + "." + suffix)).write_bytes(Path(source).read_bytes())


def main():
    """Run the named load once; keep OUT and remove scratch and children."""
    if len(sys.argv) != 4 or sys.argv[1] not in ("exit", "unlink"):
        raise SystemExit("trigger: usage: trigger.py exit|unlink MIB OUT")
    mode, mib, out = sys.argv[1], int(sys.argv[2]), Path(sys.argv[3])
    if mib not in (16, 512, 1024, 2048) or os.geteuid() != 0:
        raise SystemExit("trigger: run as root with 16, 512, 1024 or 2048 MiB")
    info = {line.split(':')[0]: int(line.split()[1])
            for line in Path('/proc/meminfo').read_text().splitlines() if ':' in line}
    if info.get('MemAvailable', info['MemFree']) < (2 * mib + 512) * 1024:
        raise SystemExit("trigger: insufficient free memory for this load")
    tmp = Path(os.environ['TMPDIR']).resolve()
    mounts = [line.split() for line in Path('/proc/mounts').read_text().splitlines()]
    if not any(row[1] == str(tmp) and row[2] == 'tmpfs' for row in mounts):
        raise SystemExit("trigger: TMPDIR must name a tmpfs mount")
    space = os.statvfs(tmp)
    if space.f_bavail * space.f_frsize < (mib + 64) * 1024 * 1024:
        raise SystemExit("trigger: insufficient tmpfs space")
    scratch = None
    audio = child = None
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGHUP, interrupted)
    try:
        out.mkdir(parents=True, exist_ok=False)
        print("trigger: pid:", os.getpid(), "evidence:", out, flush=True)
        scratch = Path(tempfile.mkdtemp(prefix='rach2-hda-', dir=tmp))
        with open('/dev/zero', 'rb') as zero, open(out / 'pacat.log', 'wb') as log:
            audio = subprocess.Popen([
                'su', '-s', '/bin/sh', 'desktop', '-c',
                'exec env XDG_RUNTIME_DIR=/run/user/1001 '
                'PULSE_SERVER=unix:/run/user/1001/pulse/native '
                'pacat --playback --raw --format=s16le --rate=48000 --channels=2'
            ], stdin=zero, stdout=log, stderr=log, start_new_session=True)
        print('trigger: waiting for a playback period', flush=True)
        first_period = None
        while True:
            if audio.poll() is not None:
                raise RuntimeError('silent stream ended before a playback period')
            report = Path('/proc/audio').read_text()
            if 'hda ready 1 playing 1' in report:
                queue = next(line.split() for line in report.splitlines() if line.startswith('queue '))
                period = int(queue[queue.index('periods') + 1])
                if first_period is not None and period != first_period:
                    break
                first_period = period
            time.sleep(0.05)
        snapshot(out, 'before-load')
        print('trigger: preparing', mode, mib, 'MiB', flush=True)
        if mode == 'exit':
            code = '''import mmap, os, sys
n = int(sys.argv[1]) * 1024 * 1024
m = mmap.mmap(-1, n, flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS)
for i in range(0, n, 4096):
    m[i] = 1
    if i and i % (128 * 1024 * 1024) == 0:
        print("trigger: touched", i // (1024 * 1024), "MiB", file=sys.stderr, flush=True)
os.write(1, b"ready\\n")
os.read(0, 1)
os._exit(0)
'''
            child = subprocess.Popen([sys.executable, '-c', code, str(mib)],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE)
            if child.stdout.readline() != b'ready\n':
                raise RuntimeError('allocation child failed')
        else:
            block = bytes(1024 * 1024)
            with open(scratch / 'data', 'wb') as f:
                for i in range(mib):
                    f.write(block)
                    if (i + 1) % 128 == 0:
                        print('trigger: wrote', i + 1, 'MiB', flush=True)
        snapshot(out, 'before-free')
        Path('/sys/kernel/debug/bkl/long').write_text('0\n')
        print('trigger: freeing', mode, mib, 'MiB', flush=True)
        if child is not None:
            child.communicate(b'x')
            if child.returncode != 0:
                raise RuntimeError('allocation child failed at exit')
        else:
            (scratch / 'data').unlink()
        snapshot(out, 'after-free')
        time.sleep(0.1)
        snapshot(out, 'after-settle')
        if audio.poll() is not None:
            raise RuntimeError('silent stream ended during the load')
        print('trigger: result: load completed; read the retained counters', flush=True)
    finally:
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.signal(signal.SIGHUP, signal.SIG_IGN)
        for proc in (child, audio):
            if proc is not None:
                try:
                    if proc is audio:
                        os.killpg(proc.pid, signal.SIGTERM)
                    elif proc.poll() is None:
                        proc.terminate()
                except ProcessLookupError:
                    pass
                proc.wait()
        if scratch is not None:
            shutil.rmtree(scratch)


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        raise SystemExit(130)
    except (OSError, RuntimeError, ValueError) as exc:
        raise SystemExit('trigger: ' + str(exc))
