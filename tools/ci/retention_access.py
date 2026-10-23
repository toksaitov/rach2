"""Keep registered source evidence in place while routine readers use it."""
import contextlib
import os
import time

from preparation_paths import canonical_path
from retention_io import checked_file, registry, require, write
from retention_registry import record_for
from retention_closure import closure


@contextlib.contextmanager
def reading(paths, bundle=False):
    policy = os.environ.get('ETUDE_RETENTION')
    if not policy:
        yield
        return
    with registry(policy, os.environ['ETUDE_SITE']) as (_, root):
        values = [dict(path=str(canonical_path(p))) for p in paths] if bundle else \
            closure([checked_file(canonical_path(p)) for p in paths]).values()
        touched = set()
        for value in values:
            path = canonical_path(value['path'])
            if root / 'runs' not in path.parents:
                require(not bundle, 'evidence source is not a registered persistent bundle')
                continue
            name = path.relative_to(root / 'runs').parts[0]
            if name not in touched:
                target, item, _ = record_for(root, root / 'runs' / name)
                item['last_used'] = time.time()
                write(target, item)
                touched.add(name)
        yield
