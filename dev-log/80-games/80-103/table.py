import re, sys, collections
rows = collections.OrderedDict()
for f in sys.argv[1:]:
    txt = open(f).read()
    for m in re.finditer(r'^== (.*?)  ([\d.]+) s\n(?:.*\n)*?   (exits=.*)$', txt, re.M):
        line, sec, kv = m.group(1), m.group(2), m.group(3)
        if line.startswith('cat') or line == 'sleep 2':
            continue
        d = dict(x.split('=') for x in kv.split())
        rows.setdefault(line, []).append((f.split('-')[1], int(d['exits']), int(d['irq_window_exits']), int(d['halt_exits']), float(sec)))
for line, v in rows.items():
    print(line)
    for k in ('base', 'fix'):
        s = [x for x in v if x[0] == k]
        print('  %-4s exits %-28s window %-22s %s' % (k, ' '.join(str(x[1]) for x in s), ' '.join(str(x[2]) for x in s), ' '.join('%.2fs' % x[4] for x in s)))
