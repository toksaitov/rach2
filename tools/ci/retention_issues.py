"""Capture open-issue references and revalidate their set before deletion."""
import hashlib
import json
import subprocess

from retention_io import require


def open_issues(repository):
    require(isinstance(repository, str) and repository.count('/') == 1 and
            all(part and all(c.isalnum() or c in '._-' for c in part)
                for part in repository.split('/')), 'invalid issue repository')
    pages = json.loads(subprocess.check_output(['gh', 'api', '--paginate', '--slurp',
        'repos/' + repository + '/issues?state=open&per_page=100'], timeout=30))
    return [issue for page in pages for issue in page if 'pull_request' not in issue]


def issue_set(issues):
    values = sorted((issue['number'], issue['updated_at'], issue['comments']) for issue in issues)
    return hashlib.sha256(json.dumps(values, separators=(',', ':')).encode()).hexdigest()


def live(snapshot):
    require(issue_set(open_issues(snapshot['repository'])) == snapshot['issue_set_sha256'],
            'open issues changed after the protection snapshot')
