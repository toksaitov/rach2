#!/usr/bin/env python3
"""Bounded fixtures for the pinned native complexity and manual judges."""
import argparse
import base64
import gzip
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

PAGE = r'''.Dd January 2, 2024
.Dt JUDGE 1
.Os Rach2
.Sh NAME
.Nm judge
.Nd exercise the native manual formatter
.Sh DESCRIPTION
Text with
.Em emphasis
and a literal backslash: \e.
'''


def prepare(root):
    root.mkdir(parents=True, exist_ok=True)
    (root / 'space path').mkdir(exist_ok=True)
    (root / 'space path/good.1').write_text(PAGE)
    (root / 'bad.1').write_text(PAGE.replace('January 2, 2024', 'not a date'))
    (root / 'iso.1').write_text(PAGE.replace('January 2, 2024', '2024-01-02'))
    (root / 'rcs.1').write_text(PAGE.replace('January 2, 2024', '$Mdocdate: January 2 2024 $'))
    (root / 'good.1.gz').write_bytes(gzip.compress(PAGE.encode(), mtime=0))
    (root / 'alias.1').write_text('.so iso.1\n')
    (root / 'decisions.c').write_text('''int decisions(int x) {
    if (x && x > 1 || x < 0) x++;
    for (; x < 3; x++) {}
    while (x < 4) x++;
    switch (x) { case 4: x++; break; default: break; }
    return x ? 1 : 0;
}
''')
    (root / 'comments.c').write_text('''int comments(void) {
    /* if for while switch && || ? */
    const char *s = "if for while switch && || ?";
    return s[0];
}
''')
    plan = [dict(id='version', tool='pmccabe', args=['-V']),
            dict(id='decisions', tool='pmccabe', args=['decisions.c']),
            dict(id='comments', tool='pmccabe', args=['comments.c'])]
    for name in ('space path/good.1', 'bad.1', 'iso.1', 'rcs.1'):
        plan.append(dict(id='lint-' + name, tool='mandoc', args=['-Tlint', '-Wwarning', name]))
    for mode in ('ascii', 'html'):
        for name in ('space path/good.1', 'iso.1', 'rcs.1', 'good.1.gz', 'alias.1'):
            plan.append(dict(id=mode + '-' + name, tool='mandoc', args=['-Ios=Rach2', '-T' + mode, name]))
        plan.append(dict(id=mode + '-pipe', tool='mandoc', args=['-Ios=Rach2', '-T' + mode],
                         input=base64.b64encode(PAGE.encode()).decode()))
        plan.append(dict(id=mode + '-multiple', tool='mandoc',
                         args=['-Ios=Rach2', '-T' + mode, 'space path/good.1', 'iso.1']))
    (root / 'plan.json').write_text(json.dumps(plan, indent=2) + '\n')


def run(prefix, root, output):
    results = []
    env = {**os.environ, 'LC_ALL': 'C', 'TZ': 'UTC'}
    for entry in json.loads((root / 'plan.json').read_text()):
        program = prefix / 'bin' / entry['tool']
        if program.with_suffix('.exe').is_file():
            program = program.with_suffix('.exe')
        result = subprocess.run([str(program), *entry['args']], cwd=root, env=env,
                                input=base64.b64decode(entry.get('input', '')),
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        results.append(dict(id=entry['id'], status=result.returncode,
                            out=base64.b64encode(result.stdout).decode(),
                            err=base64.b64encode(result.stderr).decode()))
    output.write_text(json.dumps(results, indent=2) + '\n')
    judge(results)


def judge(results):
    for item in results:
        name = item['id']
        out, err = (base64.b64decode(item[field]) for field in ('out', 'err'))
        wanted = 2 if name == 'lint-bad.1' else 0
        if item['status'] != wanted:
            raise ValueError(f'{name}: status {item["status"]}, expected {wanted}: {out!r} {err!r}')
        if name == 'version' and b'pmccabe 2.8' not in out + err:
            raise ValueError('the complexity parser is not pmccabe 2.8')
        if name in ('decisions', 'comments') and int(out.split()[0]) != (8 if name == 'decisions' else 1):
            raise ValueError(f'{name}: wrong complexity: {out!r}')
        if name.startswith(('ascii-', 'html-')) and (not out or b'manual formatter' not in out):
            raise ValueError(f'{name}: no manual output')
        print(f'{name}: status and output accepted')


def canonical(results):
    # The unmodified Windows pmccabe uses CRT text-mode newlines. Its table
    # parser treats those as whitespace; mandoc's output must match as bytes.
    result = []
    for item in results:
        copy = dict(item)
        if copy['id'] in ('version', 'decisions', 'comments'):
            for field in ('out', 'err'):
                text = base64.b64decode(copy[field]).replace(b'\r\n', b'\n')
                copy[field] = base64.b64encode(text).decode()
        result.append(copy)
    return result


def check(tool, prefix):
    if tool not in ('pmccabe', 'mandoc'):
        raise ValueError(f'unknown native judge: {tool}')
    scratch = ROOT / 'build/tests'
    scratch.mkdir(parents=True, exist_ok=True)
    output = prefix / (tool + '.check.json')
    with tempfile.TemporaryDirectory(dir=scratch, prefix='rach2-source-tools.') as name:
        root = Path(name)
        prepare(root)
        plan = [entry for entry in json.loads((root / 'plan.json').read_text()) if entry['tool'] == tool]
        (root / 'plan.json').write_text(json.dumps(plan))
        if (prefix / 'bin' / (tool + '.exe')).is_file():
            def native(path):
                if os.name == 'nt':
                    return str(path)
                return subprocess.check_output(['cygpath', '-w', str(path)], text=True).strip()
            windows = os.environ.get('SYSTEMROOT', os.environ.get('SystemRoot', 'C:/Windows'))
            powershell = windows.replace('\\', '/') + '/System32/WindowsPowerShell/v1.0/powershell.exe'
            subprocess.run([powershell, '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
                            '-File', native(ROOT / 'tests/system/source_tools_test.ps1'),
                            '-Prefix', native(prefix), '-Root', native(root), '-Output', native(output)],
                           check=True)
            judge(json.loads(output.read_text()))
        else:
            run(prefix, root, output)
    print(f'{tool}: source-tool fixture evidence: {output}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('prepare', 'run', 'compare', 'check'))
    parser.add_argument('paths', nargs='+', type=Path)
    args = parser.parse_args()
    if args.mode == 'check':
        check(str(args.paths[0]), args.paths[1].resolve())
    elif args.mode == 'prepare':
        prepare(args.paths[0].resolve())
    elif args.mode == 'run':
        run(*(path.resolve() for path in args.paths))
    else:
        first, second = (json.loads(path.read_text()) for path in args.paths)
        judge(first)
        judge(second)
        first, second = canonical(first), canonical(second)
        if first != second:
            differences = [a['id'] for a, b in zip(first, second) if a != b]
            raise ValueError(f'native judge results differ: {differences}')
        print(f'{len(first)} native judge results match; only pmccabe CRT newlines are normalized')


if __name__ == '__main__':
    raise SystemExit(main())
