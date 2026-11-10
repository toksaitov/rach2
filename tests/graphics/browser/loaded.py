"""Reserve a four-processor comparison and three eight-processor load guests."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

from run import stop


def admit(here, fields):
    """Claim all 28 processors before any guest starts, inside the launcher."""
    site = os.environ.get('ETUDE_SITE', '/etc/rach2/etude-site')
    if os.environ.get('ETUDE_ADMITTED') != site:
        launcher = fields['launcher']
        os.execv(launcher, [launcher, 'legacy', sys.executable,
                           str(Path(__file__).resolve())] + sys.argv[1:])
    claim = os.environ.get('BROWSER_LOAD_CLAIM')
    if claim:
        return claim
    claim = 'browser-load:%d:%d' % (os.getpid(), time.time_ns())
    etude = here.parents[2] / 'build/tools/etude/etude'
    args = [str(etude), 'admit', '-c', '-d', fields['locks'], '-w', fields['wait'],
            fields['ledger'], claim, subprocess.check_output(['id', '-un'], text=True).strip(),
            'shared', '28000,6442450944,1', fields['capacity'], fields['seconds'],
            '/usr/bin/env', 'BROWSER_LOAD_CLAIM=' + claim, sys.executable,
            str(Path(__file__).resolve())] + sys.argv[1:]
    os.execv(etude, args)


def main():
    """Watch the load guests, retain their records, and stop and reap all children."""
    out = Path(sys.argv[1]).resolve()
    assert sys.argv[2] == '--' and len(sys.argv) > 3, 'loaded.py OUT -- COMMAND ARG...'
    out.mkdir(parents=True, exist_ok=True)
    here = Path(__file__).resolve().parent
    ref = here.parent / 'debian-ref/debian-ref.sh'
    site = Path(os.environ.get('ETUDE_SITE', '/etc/rach2/etude-site'))
    fields = dict(line.split(' ', 1) for line in site.read_text().splitlines() if ' ' in line)
    ledger = Path(fields['ledger']) / 'claims'
    claim = admit(here, fields)
    children, logs = [], []
    command = None
    for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
        signal.signal(sig, lambda *_: sys.exit(1))
    try:
        for i in range(3):
            log = (out / ('load-%d.log' % i)).open('w')
            logs.append(log)
            args = ['sh', str(ref), 'run', '-p', 'virtio-vga', '-c', '8', '-m', '1024',
                    '-g', 'conformance', '-i', str(here / 'load.py'),
                    'python3 -u /tmp/load.py']
            (out / ('load-%d.argv.json' % i)).write_text(json.dumps(args) + '\n')
            children.append(subprocess.Popen(args, stdout=log, stderr=log,
                                              env=dict(os.environ, DEBIAN_REF_CLAIM=claim),
                                              start_new_session=True))
        while True:
            assert all(c.poll() is None for c in children), 'load guest exited; see load logs'
            if all('browser-load: ready:' in (out / ('load-%d.log' % i)).read_text()
                   for i in range(3)):
                break
            time.sleep(1)  # Poll guest readiness, with no measurement verdict.
        with (out / 'host-record').open('w') as record:
            subprocess.run(['sh', str(here.parent / 'runners/common.sh'), 'host'],
                           stdout=record, check=True)
        print('browser-load: ready: three bound guests of eight processors', flush=True)
        command = subprocess.Popen(sys.argv[3:], start_new_session=True,
                                   env=dict(os.environ, DEBIAN_REF_CLAIM=claim))
        while command.poll() is None:
            assert all(c.poll() is None for c in children), 'load guest exited; see load logs'
            time.sleep(1)  # Poll the command and its load guests until the command exits.
        assert command.returncode == 0, 'comparison command failed'
    finally:
        stop(command)
        for child in children:
            try:
                os.killpg(child.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        for child in children:
            child.wait()
        for log in logs:
            log.close()
        if ledger.exists():
            text = ledger.read_text()
            (out / 'ledger-after').write_text(text)
            assert not any(line.startswith('claim debian-ref:') and
                           any(':%d:' % child.pid in line for child in children)
                           for line in text.splitlines()), 'load claim remains in the ledger'


if __name__ == '__main__':
    main()
