# summ.py: print, for each run of vm.py given, the guest's kick, steal and wake-move
# counters at each reading and each tar's time, rcat's reads and sleeps and KVM's exits.
import re,sys
for f in sys.argv[1:]:
    t=open(f).read()
    print(f)
    ex=[]
    for m in re.finditer(r'^ipi kick ([\d ]+)$',t,re.M): ex.append(('kick',m.group(1).split()[0]))
    for m in re.finditer(r'^ipi barrier ([\d ]+)$',t,re.M): ex.append(('steal',m.group(1).split()[0]))
    for m in re.finditer(r'^ipi stop ([\d ]+)$',t,re.M): ex.append(('wmove',m.group(1).split()[0]))
    print('  guest counters at each reading:', ' '.join('%s=%s'%e for e in ex))
    for blk in re.split(r'^== ',t,flags=re.M)[1:]:
        if not blk.startswith('tar'): continue
        tm=re.search(r'([\d.]+) s',blk).group(1)
        r=re.search(r'rcat: reads (\d+) bytes \d+ nvcsw (\d+)',blk)
        d=dict(re.findall(r'(\w+)=(\d+)',blk))
        print('  %5s s reads %6s sleeps %6s exits %7s halts %6s win %6s irq %6s unnamed %6s' % (tm, r.group(1), r.group(2), d['exits'], d['halt_exits'], d['irq_window_exits'], d['irq_exits'], d['unnamed']))
