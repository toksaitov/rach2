"""Check accepted Actions jobs and listener logs before a physical grant."""
import json
from pathlib import Path
import re
import subprocess


def listener_active(text):
    active = None
    for line in text.splitlines():
        start = re.search(r'Running job: (.+)$', line)
        end = re.search(r'Job (.+) completed with result: ([A-Za-z]+)\s*$', line)
        if start:
            if active is not None:
                raise ValueError('listener accepted another job without a completion')
            active = start.group(1)
        elif end:
            if active is not None and active != end.group(1):
                raise ValueError('listener completed a different accepted job')
            active = None
    return active


def drain_snapshot(config, run_id, own_runner):
    repository = config['repository']
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
        raise ValueError('invalid Actions repository')
    pages = json.loads(subprocess.check_output(
        ['gh', 'api', '--paginate', '--slurp', f'repos/{repository}/actions/runners']))
    runners = {r['name']: r for page in pages for r in page['runners']}
    own_name = None
    if run_id:
        if not str(run_id).isdigit():
            raise ValueError('invalid coordinating workflow run')
        pages = json.loads(subprocess.check_output(
            ['gh', 'api', '--paginate', '--slurp', f'repos/{repository}/actions/runs/{run_id}/jobs']))
        matches = [j for p in pages for j in p['jobs'] if j['runner_name'] == own_runner and
                   j['status'] == 'in_progress']
        if len(matches) != 1:
            raise ValueError('coordinating accepted job is not unique')
        own_name = matches[0]['name']
    logs = {}
    for name, directory in config['listeners'].items():
        runner = runners.get(name)
        if not runner or runner['status'] != 'online' or (runner['busy'] and name != own_runner):
            raise ValueError('another accepted job or offline listener: ' + name)
        paths = sorted(Path(directory).glob('Runner_*.log'))
        if not paths:
            raise ValueError('listener has no retained diagnostic log: ' + name)
        # Replay all retained logs in filename order, including a job accepted
        # before the current listener began. Unknown unfinished state blocks.
        text = ''.join(p.read_text(encoding='utf-8', errors='strict') for p in paths)
        active = listener_active(text)
        if active is not None and (name != own_runner or active != own_name):
            raise ValueError('listener has an unfinished accepted job: ' + name)
        logs[name] = dict(files=[str(p) for p in paths], active=active,
                          sha256=__import__('hashlib').sha256(text.encode()).hexdigest())
    return dict(runners=runners, listeners=logs, coordinating_run=run_id,
                coordinating_runner=own_runner, coordinating_job=own_name)
