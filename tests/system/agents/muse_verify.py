#!/usr/bin/env python3
"""Verify the Muse row's repository and Model API requests on the host."""

import json
import os
import re
import signal
import time
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile


def watch(record, output):
    """Record host elapsed time as each request appears, until SIGTERM."""
    running = True

    def stop(_signum, _frame):
        """End the observer after it reads the last complete record."""
        nonlocal running
        running = False

    signal.signal(signal.SIGTERM, stop)
    began = time.monotonic()
    seen = 0
    with open(output, 'w', encoding='ascii') as out:
        while True:
            lines = Path(record).read_text().splitlines() if Path(record).exists() else []
            for line in lines[seen:]:
                try:
                    entry = json.loads(line)
                except ValueError:
                    break
                out.write(f'{entry["n"]} {time.monotonic() - began:.3f}\n')
                out.flush()
                seen += 1
            if not running:
                return
            time.sleep(0.1)


def prepare_script(source, destination):
    """Add the TUI's second model-list fetch and unavailable config service."""
    script = json.loads(Path(source).read_text())
    catalog = script['steps'][0]
    script['steps'].insert(1, catalog.copy())
    script['steps'].insert(2, {
        'protocol': 'plain', 'unordered': True,
        'request': {'path': '/muse-code/config', 'query': 'channel=public'},
        'reply': {'status': 404},
    })
    Path(destination).write_text(json.dumps(script))


def verify(archive, record, version):
    """Require the four tool results, edited file and single clean commit."""
    with tempfile.TemporaryDirectory(prefix='rach2-muse-verify.') as work:
        with tarfile.open(archive) as source:
            source.extractall(work, filter='data')
        mode = (Path(work) / 'mode').read_text().strip()
        assert mode in ('exec', 'tmux'), f'unknown mode {mode}'
        entries = [json.loads(line) for line in Path(record).read_text().splitlines()]
        expected = [('GET', '/muse-code/models')]
        if mode == 'tmux':
            expected += [('GET', '/muse-code/models'), ('GET', '/muse-code/config')]
        expected += [('POST', '/v1/responses')] * 5
        assert len(entries) == len(expected), f'{len(entries)} requests, want {len(expected)}'
        assert {entry.get('step') for entry in entries} == set(range(1, len(expected) + 1)), 'the played steps'
        for entry in entries:
            method, path = expected[entry['step'] - 1]
            assert (entry.get('method'), entry.get('path')) == (method, path), entry
            headers = {key.lower(): value for key, value in entry['headers']}
            assert headers.get('authorization') == 'Bearer meta-fakeapi-dummy', 'the dummy key'
            assert headers.get('user-agent', '').startswith(f'muse-build/{version.split("-")[0]} '), 'the user agent'
        recipe = Path(__file__).resolve().parents[3] / 'vendor/gnu/agents/packages/muse.sh'
        digest = re.search(r'sha256:([a-f0-9]{64})', recipe.read_text()).group(1)
        for name in ('binary.before', 'binary.after'):
            assert (Path(work) / name).read_text().split()[0] == digest, f'{name}: modified program'
        frames = [int(line) for line in (Path(work) / 'memory').read_text().splitlines()]
        assert frames, 'no memory sample'
        seconds = int((Path(work) / 'seconds').read_text())
        print(f'muse-verify: measurement: session {seconds} s; minimum sampled free frames {min(frames)} '
              f'({min(frames) * 4} KiB); sampled fall from start {max(0, frames[0] - min(frames)) * 4} KiB')
        tree = Path(work) / 'repo'
        env = dict(os.environ, HOME=work, GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL='/dev/null')

        def git(*args):
            """Read the returned repository without using the host's settings."""
            return subprocess.check_output(['git', '-C', str(tree), *args], env=env, text=True).strip()

        fixture = Path(__file__).resolve().parents[1] / 'fakeapi/repo'
        for name in ('check.sh', 'notes.txt'):
            assert (tree / name).read_bytes() == (fixture / name).read_bytes(), f'{name} changed'
        assert (tree / 'greeting.txt').read_bytes() == b'hello, rach2\n', 'the edited greeting'
        assert git('log', '--format=%s|%an <%ae>') == (
            'Greet Rach2|Muse Test <muse@fakeapi.test>\nThe fixture|Muse Test <muse@fakeapi.test>'
        ), 'the history or commit author'
        assert git('diff', '--name-only', 'HEAD~1', 'HEAD') == 'greeting.txt', 'the committed paths'
        assert git('show', 'HEAD:greeting.txt') == 'hello, rach2', 'the committed greeting'
        assert git('show', 'HEAD~1:greeting.txt') == 'hello, world', 'the original greeting'
        assert git('ls-tree', '--name-only', 'HEAD') == 'check.sh\ngreeting.txt\nnotes.txt', 'the tree'
        assert git('status', '--porcelain', '--untracked-files=all') == '', 'the work tree is dirty'
    print(f'muse-verify: pass: {len(entries)} requests and the edited file committed in a clean repository')


if __name__ == '__main__':
    try:
        if sys.argv[1] == '--script':
            prepare_script(*sys.argv[2:])
        elif sys.argv[1] == '--watch':
            watch(*sys.argv[2:])
        else:
            verify(*sys.argv[1:])
    except (AssertionError, OSError, ValueError, tarfile.TarError, subprocess.CalledProcessError) as error:
        sys.exit(f'muse-verify: fail: {error}')
