import re,sys
t=open(sys.argv[1]).read()
blocks=re.split(r'\n== ',t)
snaps=[];labels=[]
prev=None
for b in blocks:
    first=b.splitlines()[0]
    if b.startswith('cat /sys/kernel/debug/exits'):
        d={}
        for l in b.splitlines():
            m=re.match(r'(rdmsr|wrmsr|in|out|hlt|cpuid|ipi|lost) ?(\S*) (\d+)( \d+)+$',l)
            if m: d[m.group(1)+' '+m.group(2)]=[int(x) for x in l.split()[2:]]
        snaps.append((prev,d))
    else:
        prev=first
    m=re.search(r'exits=.*',b)
    if m and not b.startswith('cat'): print('%-45s'%first[:45], m.group(0).replace('request_irq_exits=0 ','').replace('nmi_window_exits=0 ','')[:260])
for i in range(1,len(snaps)):
    a,b=snaps[i-1][1],snaps[i][1]
    print('== guest counts over', snaps[i][0])
    rows=[(k,[b[k][j]-a.get(k,[0]*len(b[k]))[j] for j in range(len(b[k]))]) for k in b]
    rows=[r for r in rows if r[1][0]>=int(sys.argv[2]) if len(sys.argv)>2]
    rows.sort(key=lambda r:-r[1][0])
    for k,v in rows: print('  %-22s %8d  %s'%(k,v[0],' '.join('%7d'%x for x in v[1:])))
