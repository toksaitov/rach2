#!/bin/sh
# Require actual counter evidence, while allowing sub-tick phase durations.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
exec python3 - "$repo/tests/system/checkers/grow_measure.awk" <<'PY'
import pathlib
import subprocess
import sys
import tempfile

work = pathlib.Path(tempfile.mkdtemp(prefix='rach2-grow-measure.'))
phases = ['table', 'check-readonly', 'check-repair', 'resize']
commands = [100, 105, 105, 112, 112, 123, 123, 140]
flushes = [20, 21, 21, 21, 21, 23, 23, 26]
lines = ['grow-witness: whole before disk-commands 98 disk-flushes 20']
for i in range(8):
    phase, edge = phases[i // 2], 'after' if i % 2 else 'before'
    lines.append(f'growroot: measure: {phase} {edge} monotonic-us 1000000 '
                 f'disk-commands {commands[i]} disk-flushes {flushes[i]}')
    if i >= 2:
        lines.append(f'grow-witness: {phase} {edge} disk-commands {commands[i]} '
                     f'disk-flushes {flushes[i]}')
lines.append('grow-witness: whole after disk-commands 140 disk-flushes 26')
valid = '\n'.join(lines) + '\n'
zeros = []
for line in lines:
    words = line.split()
    words = ['0' if word.isdecimal() else word for word in words]
    zeros.append(' '.join(words))
cases = [('valid-sub-tick', valid, 0),
         ('all-zero', '\n'.join(zeros) + '\n', 1),
         ('wrong-phase', valid.replace('monotonic-us 1000000 disk-commands 112',
                                       'monotonic-us 1000000 disk-commands 113'), 1),
         ('missing-witness', '\n'.join(lines[1:]) + '\n', 1),
         ('duplicate-witness', valid + lines[-1] + '\n', 1),
         ('wrong-order', valid.replace('check-readonly before', 'check-repair before'), 1)]
for name, text, status in cases:
    path = work / name
    path.write_text(text)
    with (work / (name + '.log')).open('wb') as out:
        result = subprocess.run(['awk', '-f', sys.argv[1], str(path)], stdout=out,
                                stderr=subprocess.STDOUT, timeout=5)
    if result.returncode != status:
        sys.exit('grow-measure-test: failed: ' + name + '; logs: ' + str(work))
print('grow-measure-test: 6 checks passed; logs:', work)
PY
