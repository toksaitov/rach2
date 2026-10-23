#!/usr/bin/env python3
"""Apply selected-site retention only to explicitly registered run bundles."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'etude'))
from preparation_paths import canonical_path, filesystem_path, native_path
from retention_io import checked_file, document, registry, require, write
from retention_registry import allocate, begin, reclaim, record_for, seal, stamp
from retention_closure import closure
from retention_issues import issue_set, live, open_issues


def initialize(args):
    policy = document(canonical_path(args.policy))
    require(policy.get('format') == 'etude-retention-policy' and policy.get('schema_version') == 1,
            'not a version-one retention policy')
    require(checked_file(canonical_path(args.site))['sha256'] == policy['site_sha256'],
            'retention policy belongs to a different site')
    require(isinstance(policy['root'], str) and not any(c in policy['root'] for c in '\r\n\0'),
            'retention root has invalid characters')
    root = filesystem_path(policy['root'])
    require(root.is_absolute() and not root.exists(), 'retention root must be a new absolute directory')
    for parent in root.parents:
        require(not parent.is_symlink(), 'retention root has a symlink parent')
        require(not (parent / '.git').exists(), 'retention root must be outside disposable checkouts')
    root.mkdir(parents=True)
    for name in ('registry', 'runs', 'accepted'):
        (root / name).mkdir()
    write(root / '.etude-retention-root.json', dict(format='etude-retention-root', schema_version=1,
          root=native_path(root), site_sha256=policy['site_sha256']))
    print(native_path(root))


def issue_snapshot(args):
    mapping = document(canonical_path(args.mapping))
    current = open_issues(args.repository)
    issues, unknown = [], []
    signature = issue_set(current)
    for issue in current:
        number = str(issue['number'])
        if number not in mapping:
            unknown.append(issue['number'])
            continue
        roots = mapping[number]
        comments = json.loads(subprocess.check_output(['gh', 'api', '--paginate', '--slurp',
            'repos/' + args.repository + '/issues/' + number + '/comments?per_page=100'], timeout=30))
        issue['retained_comments'] = [comment for page in comments for comment in page]
        closure(roots)  # Pin reachable proof bytes, not just their filenames.
        issues.append(dict(number=issue['number'], snapshot_sha256=hashlib.sha256(
            json.dumps(issue, sort_keys=True).encode()).hexdigest(), evidence=roots, snapshot=issue))
    output = filesystem_path(args.output)
    require(not output.exists(), 'issue snapshot output already exists')
    value = dict(format='etude-retention-open-issues', schema_version=1,
                 repository=args.repository, captured_utc=stamp(), complete=True,
                 issue_set_sha256=signature, issues=issues, unknown=unknown)
    live(value)
    write(output, value)
    print(json.dumps(checked_file(output)))


def action(args):
    if args.action == 'init':
        return initialize(args)
    if args.action == 'issues':
        return issue_snapshot(args)
    with registry(args.policy, args.site) as (policy, root):
        if args.action == 'allocate':
            bundle = allocate(policy, root, args.reference)
            if args.github_env:
                with filesystem_path(args.github_env).open('a') as stream:
                    stream.write('ETUDE_RESULTS=' + native_path(bundle) + '\n')
            print(native_path(bundle))
        elif args.action == 'begin':
            begin(root, args.output, args.run)
        elif args.action == 'seal':
            item = seal(policy, root, args.output)
            print(json.dumps(dict(state=item['state'], reason=item.get('reason'))))
        elif args.action == 'touch':
            path, item, bundle = record_for(root, args.output)
            item['last_used'] = time.time()
            write(path, item)
        else:
            print(json.dumps(reclaim(policy, root, args.reference, args.apply), indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--policy', type=filesystem_path)
    parser.add_argument('--site', type=filesystem_path)
    sub = parser.add_subparsers(dest='action', required=True)
    sub.add_parser('init')
    for name in ('allocate', 'prune'):
        command = sub.add_parser(name)
        command.add_argument('--reference', type=filesystem_path)
        if name == 'allocate':
            command.add_argument('--github-env', type=filesystem_path)
        else:
            command.add_argument('--apply', action='store_true', help='otherwise report without deleting')
    for name in ('begin', 'seal', 'touch'):
        command = sub.add_parser(name)
        command.add_argument('output', type=filesystem_path)
        if name == 'begin':
            command.add_argument('run')
    command = sub.add_parser('issues')
    command.add_argument('--repository', required=True)
    command.add_argument('--mapping', required=True)
    command.add_argument('--output', required=True)
    args = parser.parse_args()
    require(args.action == 'issues' or (args.policy is not None and args.site is not None),
            '--policy and --site are required')
    action(args)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print('ci: retention: ' + str(error), file=sys.stderr)
        sys.exit(2)
