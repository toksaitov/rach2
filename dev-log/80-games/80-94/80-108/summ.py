# summ.py: print each tar of a vm.py run, its time, rcat's reads, sleeps and
# reads by size, KVM's exits by kind, and the guest's kicks over each
# reading of debugfs's exits (Rach2) or the change in function-call
# interrupts (Linux).
import re,sys
for f in sys.argv[1:]:
    t=open(f).read()
    print(f)
    marks=[]
    for blk in re.split(r'^== ',t,flags=re.M)[1:]:
        if blk.startswith('tar'):
            tm=re.search(r'([\d.]+) s',blk).group(1)
            d=dict(re.findall(r'(\w+)=(\d+)',blk))
            r=re.search(r'rcat: reads (\d+) bytes \d+ nvcsw (\d+) nivcsw \d+ by 10k: (.*)',blk)
            what='rcat' if r else 'zero'
            line='  %-4s %5s s exits %7s halts %6s win %6s unnamed %6s' % (what, tm, d['exits'], d['halt_exits'], d['irq_window_exits'], d['unnamed'])
            if r: line+='  reads %6s sleeps %6s by10k %s' % (r.group(1), r.group(2), r.group(3))
            print(line)
        m=re.search(r'^ipi kick (\d+)',blk,re.M)
        if m: marks.append(('kick',int(m.group(1)))); print('  kick total %s' % m.group(1))
        m=re.search(r'^CAL:\s+([\d\s]+?)\s+Function',blk,re.M)
        if m: print('  CAL total %d' % sum(int(x) for x in m.group(1).split()))
