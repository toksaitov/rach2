"""Check accepted Actions jobs and current-boot listener state before a grant."""
from datetime import datetime, timezone
import hashlib
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
            if active != end.group(1):
                raise ValueError('listener completed a different accepted job')
            active = None
    return active


def boot_snapshot():
    identity = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    if not re.fullmatch(r'[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}', identity):
        raise ValueError('Linux listener boot identity is unavailable')
    lines = re.findall(r'^btime ([1-9][0-9]*)$', Path('/proc/stat').read_text(), re.MULTILINE)
    if len(lines) != 1:
        raise ValueError('Linux listener boot timestamp is unavailable')
    started = int(lines[0])
    if started > int(datetime.now(timezone.utc).timestamp()):
        raise ValueError('Linux listener boot timestamp is in the future')
    return dict(boot_id=identity, boot_unix_seconds=started, btime='btime ' + lines[0])


def listener_boot_state(text, boot):
    current, historical = [], []
    saw_current = False
    for line in text.splitlines():
        event = 'Running job:' in line or ('Job ' in line and 'completed with result:' in line)
        stamp = re.match(r'^\[([0-9-]+ [0-9:]+Z) ', line)
        if not stamp:
            if event:
                raise ValueError('listener job event has no boot-comparable timestamp')
            continue
        try:
            at = datetime.strptime(stamp[1], '%Y-%m-%d %H:%M:%SZ').replace(tzinfo=timezone.utc)
        except ValueError:
            raise ValueError('listener has an invalid UTC timestamp') from None
        if at > datetime.now(timezone.utc):
            raise ValueError('listener timestamp is in the future')
        if at.timestamp() < boot['boot_unix_seconds']:
            if event:
                historical.append(line)
        else:
            saw_current = True
            if event:
                if not re.search(r'Running job: .+$|Job .+ completed with result: [A-Za-z]+\s*$', line):
                    raise ValueError('listener has a malformed current-boot job event')
                current.append(line)
    if not saw_current:
        raise ValueError('listener has no diagnostic evidence from this boot')
    return dict(active=listener_active('\n'.join(current)),
                current_events=current, superseded_by_boot=historical)


def api(repository, suffix):
    return json.loads(subprocess.check_output(
        ['gh', 'api', '--paginate', '--slurp', f'repos/{repository}/actions/{suffix}']))


def server_snapshot(repository, run_id, own_runner):
    runners = {r['name']: r for page in api(repository, 'runners') for r in page['runners']}
    workflows = {}
    for status in ('in_progress', 'queued', 'waiting', 'pending', 'requested'):
        for page in api(repository, 'runs?status=' + status):
            for run in page['workflow_runs']:
                workflows[str(run['id'])] = run
    if any(identity != run_id for identity in workflows):
        raise ValueError('another current workflow has not completed')
    own_name, jobs = None, []
    if run_id:
        if run_id not in workflows or workflows[run_id]['status'] != 'in_progress':
            raise ValueError('coordinating workflow is not currently in progress')
        jobs = [j for page in api(repository, f'runs/{run_id}/jobs') for j in page['jobs']]
        matches = [j for j in jobs if j['runner_name'] == own_runner and j['status'] == 'in_progress']
        if len(matches) != 1:
            raise ValueError('coordinating accepted job is not unique')
        own = matches[0]
        if any(j['id'] != own['id'] and j['status'] != 'completed' and j.get('runner_id', 0)
               for j in jobs):
            raise ValueError('another job in the coordinating workflow has been accepted')
        own_name = own['name']
        if not isinstance(own_name, str) or not own_name:
            raise ValueError('coordinating accepted job has no name')
    return dict(runners=runners, workflows=workflows, jobs=jobs, coordinating_job=own_name)


def server_identity(snapshot):
    return dict(runners={name: (r['id'], r['status'], r['busy'])
                         for name, r in snapshot['runners'].items()},
                workflows={identity: r['status'] for identity, r in snapshot['workflows'].items()},
                jobs={j['id']: (j['status'], j.get('runner_id'), j['runner_name'], j['name'])
                      for j in snapshot['jobs']})


def drain_snapshot(config, run_id, own_runner):
    repository = config['repository']
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
        raise ValueError('invalid Actions repository')
    run_id = str(run_id) if run_id else None
    if run_id and not run_id.isdigit():
        raise ValueError('invalid coordinating workflow run')
    boot = boot_snapshot()
    before = server_snapshot(repository, run_id, own_runner)
    logs = {}
    for name, directory in config['listeners'].items():
        runner = before['runners'].get(name)
        if (not runner or runner['status'] != 'online' or type(runner['busy']) is not bool or
                (runner['busy'] and (not run_id or name != own_runner or
                                     not before['coordinating_job']))):
            raise ValueError('another accepted job or offline listener: ' + name)
        paths = sorted(Path(directory).glob('Runner_*.log'))
        if not paths:
            raise ValueError('listener has no retained diagnostic log: ' + name)
        contents = [p.read_bytes() for p in paths]
        text = b''.join(contents).decode('utf-8', errors='strict')
        state = listener_boot_state(text, boot)
        if state['active'] is not None and (name != own_runner or
                                           state['active'] != before['coordinating_job']):
            raise ValueError('listener has an unfinished accepted job: ' + name)
        files = [dict(path=str(p), sha256=hashlib.sha256(raw).hexdigest(), size_bytes=len(raw))
                 for p, raw in zip(paths, contents)]
        logs[name] = dict(**state, files=files, sha256=hashlib.sha256(text.encode()).hexdigest())
    after = server_snapshot(repository, run_id, own_runner)
    if boot_snapshot() != boot or server_identity(before) != server_identity(after):
        raise ValueError('listener boot or server job state changed during drain check')
    return dict(boot=boot, server_before=before, server_after=after, listeners=logs,
                coordinating_run=run_id, coordinating_runner=own_runner,
                coordinating_job=before['coordinating_job'])
