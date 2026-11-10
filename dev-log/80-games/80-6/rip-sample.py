# Sample every processor's RIP through QMP.  Usage: 2653-sample.py TAG COUNT SECONDS
import collections, json, pathlib, re, socket, sys, time

w = pathlib.Path(__file__).resolve().parent
tag, count, gap = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
s = socket.socket(socket.AF_UNIX)
s.connect(str(w / f'qmp-{tag}.sock'))
f = s.makefile('rw')
f.readline()


def ask(c, **a):
    f.write(json.dumps({'execute': c, 'arguments': a}) + '\n')
    f.flush()
    while True:
        r = json.loads(f.readline())
        if 'return' in r or 'error' in r:
            return r


ask('qmp_capabilities')
hits = collections.Counter()
for _ in range(count):
    text = ask('human-monitor-command', **{'command-line': 'info registers -a'})['return']
    for rip, cs in re.findall(r'RIP=([0-9a-f]+).*?CS =([0-9a-f]+)', text, re.S):
        hits[rip if int(cs, 16) & 3 == 0 else 'user'] += 1
    time.sleep(gap)
for rip, n in hits.most_common():
    print(n, rip)
