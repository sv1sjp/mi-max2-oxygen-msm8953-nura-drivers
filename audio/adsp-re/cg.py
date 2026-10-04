# SPDX-License-Identifier: GPL-2.0-or-later
import re,sys,bisect,pickle,os
if not os.path.exists('cg.pkl'):
    lines=open('a04.txt').read().splitlines()
    addr=[];txt=[]
    for L in lines:
        m=re.match(r'^([0-9a-f]{8}):',L)
        if m: addr.append(int(m.group(1),16)); txt.append(L)
    targets=set()
    for t in txt:
        for m in re.finditer(r'(?:call|jump) 0x([0-9a-f]+)',t):
            if 'call' in m.group(0): targets.add(int(m.group(1),16))
    starts=sorted(targets)
    pickle.dump((addr,txt,starts),open('cg.pkl','wb'))
addr,txt,starts=pickle.load(open('cg.pkl','rb'))
def func_of(a):
    i=bisect.bisect_right(starts,a)-1; return starts[i] if i>=0 else None
def body(f):
    i=bisect.bisect_left(addr,f); j=bisect.bisect_right(starts,f)
    end=starts[j] if j<len(starts) else f+0x1000
    out=[]
    while i<len(addr) and addr[i]<end: out.append(txt[i]); i+=1
    return out
def callees(f):
    s=[];b=body(f)
    for t in b:
        for m in re.finditer(r'(call|jump) 0x([0-9a-f]+)',t):
            a=int(m.group(2),16)
            if m.group(1)=='call' or func_of(a)!=f: s.append(a)
    return s,b
def notes(b): return [t.split(';',1)[1].strip()[:80] for t in b if '    ; ' in t]
if __name__=='__main__':
    root=int(sys.argv[1],16); depth=int(sys.argv[2])
    seen=set(); q=[(root,0)]
    while q:
        f,d=q.pop(0)
        if f in seen or d>depth: continue
        seen.add(f); c,b=callees(f)
        n=notes(b)
        print('  '*d+hex(f),len(b),'|'.join(n)[:200])
        for x in dict.fromkeys(c): q.append((x,d+1))
