"""Register complete bundles and prune only unchanged ordinary evidence."""
import datetime
import hashlib
import json
import shutil
import subprocess
import time
import uuid

from preparation_paths import canonical_path, native_path
from retention_io import checked, checked_file, document, files, require, write
from retention_closure import cleanup_confirmed, closure, complete_history
from retention_issues import live


def stamp():
    return datetime.datetime.now(datetime.timezone.utc).isoformat().replace('+00:00', 'Z')


def seconds(value):
    require(value.endswith('Z') or value.endswith('+00:00'), 'retention timestamp is not UTC')
    return datetime.datetime.fromisoformat(value.replace('Z', '+00:00')).timestamp()


def records(root):
    result = {}
    for path in (root / 'registry').glob('*.json'):
        record = document(path)
        bundle = root / 'runs' / path.stem
        require(record.get('format') == 'etude-retained-bundle' and
                record.get('schema_version') == 1 and record.get('state') in
                ('allocated', 'active', 'sealed', 'protected'), 'unknown retained bundle record')
        require(record['path'] == native_path(bundle), 'registry bundle path differs')
        result[path.stem] = record
    return result


def record_for(root, bundle):
    bundle = canonical_path(bundle)
    require(bundle.parent == root / 'runs', 'OUT is not a registered persistent bundle')
    path = root / 'registry' / (bundle.name + '.json')
    record = document(path)
    require(record['path'] == native_path(bundle), 'registered bundle identity differs')
    return path, record, bundle


def protection_roots(policy, root, reference=None):
    roots = []
    for accepted in (root / 'accepted').glob('*.json'):
        roots.extend(document(accepted)['roots'])
    for ref in policy['baselines']:
        descriptor = document(checked(ref))
        require(descriptor.get('format') == 'etude-retention-protection' and
                descriptor.get('kind') == 'baseline' and descriptor['roots'],
                'baseline protection descriptor is incomplete')
        roots.extend(descriptor['roots'])
        roots.append(ref)
    issues_ref = policy['open_issues']
    issues = document(checked(issues_ref))
    require(issues.get('format') == 'etude-retention-open-issues' and
            issues.get('schema_version') == 1 and issues.get('complete') is True and
            not issues['unknown'] and
            0 <= time.time() - seconds(issues['captured_utc']) <= policy['issue_snapshot_max_age_seconds'],
            'open-issue protection snapshot is incomplete or stale')
    for issue in issues['issues']:
        require(type(issue['number']) is int and issue['number'] > 0 and
                issue['snapshot']['number'] == issue['number'] and
                issue['snapshot']['state'] == 'open' and
                hashlib.sha256(json.dumps(issue['snapshot'], sort_keys=True).encode()).hexdigest() ==
                issue['snapshot_sha256'], 'invalid open-issue snapshot entry')
        roots.extend(issue['evidence'])
    roots.append(issues_ref)
    if reference:
        ref = checked_file(canonical_path(reference))
        current = document(checked(ref))
        require(current.get('format') == 'etude-site-reference' and current['histories'],
                'selected-site reference is incomplete')
        roots.extend([ref, current['policy'], *current['histories']])
    return roots


def protection(policy, root, registered, reference=None):
    keep = {name for name, item in registered.items() if item['state'] != 'sealed'}
    external = {key: ref for item in registered.values() for key, ref in item.get('proofs', {}).items()}
    for ref in policy['anchors']:
        external[str(canonical_path(ref['path'], strict=False))] = ref
    reasons = []
    try:
        protected_proofs = closure(protection_roots(policy, root, reference))
        external.update(protected_proofs)
        for value in protected_proofs.values():
            path = canonical_path(value['path'])
            if root / 'runs' in path.parents:
                keep.add(path.relative_to(root / 'runs').parts[0])
        # Every retained bundle protects proofs in a different bundle. A
        # later ordinary sample must not lose its earlier dependencies.
        for name, item in registered.items():
            for value in item.get('proofs', {}).values():
                path = checked(value)
                if root / 'runs' in path.parents:
                    dependency = path.relative_to(root / 'runs').parts[0]
                    if dependency != name:
                        keep.add(dependency)
    except (OSError, ValueError, KeyError, TypeError) as error:
        keep.update(registered)
        reasons.append(str(error))
    return keep, external, reasons


def usage(root, registered, external):
    # Logical bytes are conservative for sparse files. Count every file under
    # the managed root, including unknown objects and executable inputs.
    total = sum(p.stat().st_size for p in root.rglob('*') if p.is_file())
    for ref in external.values():
        path = canonical_path(ref['path'], strict=False)
        if root not in path.parents:
            total += max(ref['size_bytes'], path.stat().st_size if path.exists() else 0)
    for item in registered.values():
        if item['state'] != 'sealed':
            bundle = canonical_path(item['path'])
            actual = sum(p.stat().st_size for p in bundle.rglob('*') if p.is_file())
            total += max(0, item['reservation_bytes'] - actual)
    return total


def reclaim(policy, root, reference=None, apply=False):
    registered = records(root)
    keep, external, reasons = protection(policy, root, registered, reference)
    total = usage(root, registered, external)
    removed = []
    original_total = total
    now = time.time()
    candidates = sorted(registered.items(), key=lambda pair: pair[1].get('sealed_utc', ''))
    for name, item in candidates:
        if name in keep:
            continue
        bundle = root / 'runs' / name
        age = now - seconds(item['sealed_utc'])
        if age < 86400 or (age < policy['max_age_days'] * 86400 and
                           total + policy['reserve_bytes'] <= policy['max_bytes']):
            continue
        try:
            actual = files(bundle)
            require(actual == item['files'], 'bundle changed after sealing')
            require(now - max([bundle.stat().st_mtime, item['last_used'],
                               *(p.stat().st_mtime for p in bundle.rglob('*'))]) >= 86400,
                    'bundle was used or changed within one day')
        except (OSError, ValueError) as error:
            reasons.append(str(error))
            continue
        removed.append(native_path(bundle))
        total -= sum(ref['size_bytes'] for ref in actual.values())
    if apply and removed:
        try:
            live(document(checked(policy['open_issues'])))
        except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
            keep.update(registered)
            reasons.append('no deletion: ' + str(error))
            removed, total = [], original_total
        for path in removed:
            bundle = canonical_path(path)
            shutil.rmtree(bundle)
            (root / 'registry' / (bundle.name + '.json')).unlink()
    return dict(bytes=total, limit_bytes=policy['max_bytes'], reserved_next_bytes=policy['reserve_bytes'],
                can_allocate=total + policy['reserve_bytes'] <= policy['max_bytes'],
                protected=sorted(keep), protection_warnings=reasons,
                removed=removed if apply else [], would_remove=removed if not apply else [])


def allocate(policy, root, reference=None):
    if reference:
        current = document(canonical_path(reference))
        require(current.get('format') == 'etude-site-reference' and current['histories'],
                'selected-site reference is incomplete')
        roots = [current['policy'], *current['histories']]
        proofs = closure(roots)
        retained = dict(format='etude-retention-accepted-reference', schema_version=1,
                        reference=current, roots=roots, proofs=proofs)
        identity = checked_file(canonical_path(reference))['sha256']
        target = root / 'accepted' / (identity + '.json')
        if not target.exists():
            write(target, retained)
    report = reclaim(policy, root, reference, apply=True)
    require(report['can_allocate'],
            'retained evidence ' + str(report['bytes']) + ' bytes plus reservation ' +
            str(policy['reserve_bytes']) + ' exceeds byte limit ' + str(policy['max_bytes']) +
            '; refresh protections or increase policy capacity; ' + '; '.join(report['protection_warnings']))
    name = 'run-' + uuid.uuid4().hex
    bundle = root / 'runs' / name
    bundle.mkdir()
    write(root / 'registry' / (name + '.json'), dict(format='etude-retained-bundle', schema_version=1,
          path=native_path(bundle), state='allocated', created_utc=stamp(), last_used=time.time(),
          reservation_bytes=policy['reserve_bytes']))
    return bundle


def begin(root, bundle, run):
    path, item, bundle = record_for(root, bundle)
    require(item['state'] == 'allocated' and not any(bundle.iterdir()), 'persistent OUT was already used')
    item.update(state='active', run_id=run, last_used=time.time())
    write(path, item)


def seal(policy, root, bundle):
    path, item, bundle = record_for(root, bundle)
    require(item['state'] == 'active', 'bundle is not active')
    try:
        cleanup_confirmed(bundle, item['run_id'])
        runs = complete_history(bundle)
        require(runs == [item['run_id']], 'terminal run identity differs from registered invocation')
        inventory = files(bundle)
        proofs = closure([ref for name, ref in inventory.items() if name.startswith('history/')])
        item['proofs'] = proofs
        anchors = {str(checked(ref)) for ref in policy['anchors']}
        for value in proofs.values():
            location = canonical_path(value['path'])
            require(root / 'runs' in location.parents or str(location) in anchors,
                    'proof is outside registered bundles and immutable anchors: ' + str(location))
        item.update(state='sealed', files=inventory, proofs=proofs, sealed_utc=stamp(),
                    last_used=time.time())
    except (OSError, ValueError, KeyError, TypeError) as error:
        item.update(state='protected', reason=str(error), last_used=time.time())
    write(path, item)
    return item
