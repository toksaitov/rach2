# table.py OUT [N]: the N largest differences of each pair of debugfs `exits`
# readings in a vm.py run, the EOI writes of wait-diag.patch by what waited.
import sys,re
blocks=[];cur=None
for l in open(sys.argv[1]):
    if l.startswith('== cat'):
        cur={};blocks.append(cur);continue
    if l.startswith('=='): cur=None; continue
    if cur is None: continue
    f=l.split()
    if len(f)<3 or not f[2].isdigit(): continue
    cur[(f[0],f[1])]=list(map(int,f[2:]))
ctxn={0:'idle',1:'user',2:'kern'}
for i in range(1,len(blocks)):
    a,b=blocks[i-1],blocks[i]
    print('--- tar', i)
    rows=[]
    for k,v in b.items():
        d=[x-y for x,y in zip(v,a.get(k,[0]*len(v)))]
        if d[0]==0 or k[0] not in ('eoiwait','eoiunset','eoitaken','wrmsr','ipi','hlt','eoipv'): continue
        if k[0]=='wrmsr' and k[1] not in ('0x80b','0x830','0x838','0x832'): continue
        if k[0]=='eoiwait':
            n=int(k[1],16); vec=n&0xff; w=(n>>8)&0xff; c=(n>>16)&0xf; nest=n>>20
            rows.append((d[0],'wait end=%#x waiting=%#x ctx=%s%s'%(vec,w,ctxn[c],' nested' if nest else ''),d[1:]))
        else:
            rows.append((d[0],'%s %s'%k,d[1:]))
    for r in sorted(rows,reverse=True)[:int(sys.argv[2]) if len(sys.argv)>2 else 30]: print(r)
