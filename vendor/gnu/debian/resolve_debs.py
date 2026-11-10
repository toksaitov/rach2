#!/usr/bin/env python3
"""Resolve snapshot package dependencies, including versioned alternatives."""

import collections
import re
import subprocess
import sys


def accepts(version, relation):
    """Compare a package version with a Debian dependency relation."""
    if relation is None:
        return True
    op, wanted = relation
    return subprocess.run(['dpkg', '--compare-versions', version, op, wanted],
                          check=False).returncode == 0


def term(text):
    """Read one binary dependency, dropping an architecture qualifier."""
    match = re.fullmatch(r'\s*([a-z0-9+.-]+)(?::[a-z0-9-]+)?\s*'
                         r'(?:\((<<|<=|=|>=|>>)\s*([^()]+)\))?\s*', text)
    if not match:
        raise ValueError('invalid dependency: ' + text)
    name, op, version = match.groups()
    return name, (op, version.strip()) if op else None


def resolve(index, output, unfollowed, roots):
    """Write one deterministic closure, refusing unmet version constraints.

    Of a package that several suites list, the highest version is taken, as
    apt takes it; of equal versions, the later suite's entry.
    """
    packages = {}
    with open(index, encoding='utf-8') as stream:
        for line in stream:
            row = line.rstrip('\n').split('\t')
            if len(row) != 8 or not re.fullmatch('[0-9a-f]{64}', row[5]):
                raise ValueError('incomplete package index row')
            # A suite's older version, such as a security update that a
            # point release superseded, never replaces a newer one.
            had = packages.get(row[0])
            if had is None or not accepts(row[1], ('<<', had[1])):
                packages[row[0]] = row
    providers = collections.defaultdict(list)
    for name in sorted(packages):
        for provided in filter(None, packages[name][7].split(',')):
            virtual, relation = term(provided)
            providers[virtual].append((name, relation[1] if relation else None))

    def choose(clause):
        alternatives = [term(t) for t in clause.split('|')]
        for name, relation in alternatives:
            if name in unfollowed:
                return None
            if name in packages and accepts(packages[name][1], relation):
                return name
        # A provider already wanted satisfies a virtual package, as apt
        # takes an installed one; failing that, the first by name.
        found = [provider for name, relation in alternatives
                 for provider, version in providers[name]
                 if relation is None or (version and accepts(version, relation))]
        for provider in found:
            if provider in wanted or provider in seen:
                return provider
        if found:
            return found[0]
        raise ValueError('no package satisfies ' + clause)

    queue = collections.deque()
    wanted = set()
    for root in roots:
        name, sep, version = root.partition('=')
        if name not in packages:
            raise ValueError('no root package ' + name)
        if sep and packages[name][1] != version:
            raise ValueError(root + ': snapshot has ' + packages[name][1])
        queue.append(name)
        wanted.add(name)
    seen = set()
    while queue:
        name = queue.popleft()
        if name in seen:
            continue
        seen.add(name)
        for clause in filter(None, packages[name][6].split(',')):
            chosen = choose(clause)
            if chosen:
                queue.append(chosen)
    with open(output, 'w', encoding='utf-8') as stream:
        for name in sorted(seen):
            stream.write(' '.join(packages[name][:6]) + '\n')


if __name__ == '__main__':
    try:
        resolve(sys.argv[1], sys.argv[2], sys.argv[3].split(), sys.argv[4:])
    except (ValueError, OSError) as error:
        sys.exit('gnu: debian: ' + str(error))
