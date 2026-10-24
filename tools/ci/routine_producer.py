"""Fixed Linux operations run by the existing native remote responder."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def execute(argv, **kwargs):
    print('routine producer: ' + __import__('shlex').join(map(str, argv)), flush=True)
    return subprocess.run(list(map(str, argv)), check=True, **kwargs)


def write(path, value):
    with Path(path).open('x') as stream:
        json.dump(value, stream, indent=2)
        stream.write('\n')


def projection(config, output):
    text = Path(config['site']).read_text()
    lines = text.splitlines()
    if lines[0] != 'etude-site 2' or sum(l.startswith('endpoint ') for l in lines) != 1:
        raise ValueError('invalid installed producer policy')
    lines = ['endpoint linux' if l.startswith('endpoint ') else l for l in lines]
    Path(output).write_text('\n'.join(lines) + '\n')


def stage(args, config):
    p = config['producer']
    root = Path(p['tree_root'])
    root.mkdir(parents=True, exist_ok=True)
    tree = root / args.commit
    repository = Path(p['repository'])
    execute(['git', '-C', repository, 'bundle', 'verify', Path('candidate.bundle').absolute()])
    execute(['git', '-C', repository, 'fetch', str(Path('candidate.bundle').absolute()), args.commit])
    if not tree.exists():
        execute(['git', '-C', repository, 'worktree', 'add', '--detach', tree, args.commit])
    actual = subprocess.check_output(['git', '-C', tree, 'rev-parse', 'HEAD']).decode().strip()
    dirty = subprocess.check_output(['git', '-C', tree, 'status', '--porcelain', '--untracked-files=all'])
    if actual != args.commit or dirty:
        raise ValueError('producer worktree is not the exact clean candidate')
    out = Path(p['output_root']) / args.run
    out.mkdir(parents=True, exist_ok=False)
    site = str(Path('producer.site').absolute())
    if not Path(site).exists():
        raise ValueError('producer projection was not installed before admission')
    policy = dict(l.split(' ', 1) for l in Path(site).read_text().splitlines())
    os.environ.update(ETUDE_SITE=site, ETUDE_ROUTINE_COMMIT=args.commit)
    resource = p['preparation_resources']
    if args.purpose not in ('push', 'ci', 'release'):
        raise ValueError('invalid routine purpose')
    bound = {'push': 270, 'ci': 540, 'release': 13800}[args.purpose]
    if os.environ.get('ETUDE_ADMITTED') != site:
        raise ValueError('source staging and production need the enclosing real admission')
    command = ['/bin/sh', str(tree / 'tools/ci/native_producer.sh'), args.purpose,
               str(tree), str(out), args.arches, args.run + ':producer', site,
               p['etude'], p['accelerator'], *args.selection]
    execute(command)
    for name in ('producer.json', 'prepare.record', 'preparation.receipt.json'):
        shutil.copyfile(out / name, name)
    write('produced.json', dict(tree=str(tree), metadata=str(out / 'producer.json'), mode='produced'))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['produce', 'projection', 'freeze', 'restore', 'facts'])
    for name in ('config', 'commit', 'run', 'purpose', 'arches'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('selection', nargs='*')
    args = parser.parse_args()
    config = json.loads(Path(args.config).read_text())
    nonce = Path('nonce').read_text().strip()
    if config['mode'] in ('windows-import', 'wsl-native') and (
            len(nonce) != 64 or any(c not in '0123456789abcdef' for c in nonce)):
        raise ValueError('invalid owner capability')
    if nonce:
        os.environ['ETUDE_AUTHORITY_NONCE'] = nonce
    p = config['producer']
    if args.action == 'projection':
        projection(p, 'producer.site')
    elif args.action == 'produce':
        # The native phase owns source staging, admission and all build children.
        # stdin remains a separate owner-loss channel; the build consumes none.
        stage(args, config)
    else:
        operation = args.action
        if operation in ('freeze', 'facts'):
            from routine_jobs import drain_snapshot
            write('jobs-before.json', drain_snapshot(config['accepted_jobs'], config.get('github_run'),
                                                     config.get('github_runner')))
        execute([p['etude'], 'authority', operation, args.run, args.commit,
                 *([str(Path.cwd())] if operation == 'facts' else [])])
        if operation == 'freeze':
            write('jobs-after.json', drain_snapshot(config['accepted_jobs'], config.get('github_run'),
                                                    config.get('github_runner')))
            execute([p['etude'], 'authority', 'facts', args.run, args.commit, str(Path.cwd())])


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print('routine producer: ' + str(error), file=sys.stderr)
        raise SystemExit(1)
