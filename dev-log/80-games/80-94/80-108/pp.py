# pp.py: print the probe's counters over each pair of readings of a vm.py run.
import re,sys
t=open(sys.argv[1]).read()
reads=[]
for blk in re.split(r'^== ',t,flags=re.M)[1:]:
    if not blk.startswith('cat /sys/kernel/debug/exits'): continue
    d={}
    for m in re.finditer(r'^pp (\d+) (\d+)',blk,re.M): d[int(m.group(1))]=int(m.group(2))
    m=re.search(r'^ipi kick (\d+)',blk,re.M); d['kick']=int(m.group(1))
    m=re.search(r'^hlt 0 (\d+)',blk,re.M); d['hlt']=int(m.group(1))
    reads.append(d)
names={0:'writes',1:'w_empty',2:'w_wake',3:'reads',4:'r_slept',5:'r_kib',6:'r_samecpu',7:'rcat_locksleep',8:'tar_locksleep'}
for a,b in zip(reads,reads[1:]):
    dd={k:b[k]-a.get(k,0) for k in b}
    print(' '.join('%s=%d'%(names[k],dd[k]) for k in sorted(names)), 'kick=%d hlt=%d'%(dd['kick'],dd['hlt']))
    for base,nm in ((16,"wake->run"),(32,"w period"),(48,"r gap"),(64,"wakingwrite"),(80,"in wake"),(96,"asleep"),(112,"kick")):
        print('  %-10s'%nm, ' '.join('%d:%d'%(8+i,dd[base+i]) for i in range(16) if dd[base+i]))
