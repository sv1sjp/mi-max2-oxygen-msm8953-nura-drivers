# SPDX-License-Identifier: GPL-2.0-or-later
import re,struct,sys
segs={3:0xf0102000,4:0xf015f000,5:0xf0821000,6:0xf0d35000,7:0xf0d36000,8:0xf0000000,9:0xf0014000,10:0x2db00000,11:0x2dbf7000,12:0x2dcd3000,13:0x2da00000}
data={i:open('adsp.b%02d'%i,'rb').read() for i in segs}
def rd(a,n=4):
    for i,b in segs.items():
        if b<=a and a+n<=b+len(data[i]): return data[i][a-b:a-b+n]
def cstr(a):
    for i,b in segs.items():
        if b<=a<b+len(data[i]):
            d=data[i];o=a-b;e=d.find(b'\0',o,o+200)
            if e>o+3:
                t=d[o:e]
                if all(32<=c<127 or c in (9,10) for c in t): return t.decode()
def desc(a):
    t=cstr(a)
    if t: return '"%s"'%t[:90]
    r=rd(a,12)
    if r:
        ln,mask,fmt=struct.unpack('<3I',r)
        if True:
            t=cstr(fmt)
            if t: return 'MSG[%d] "%s"'%(ln&0xffff,t[:110])
    return None
def fix(m):
    v=int(m.group(2),16)
    if m.group(1)=='-': v=(-v)&0xffffffff
    return '##0x%08x'%v
out=open(sys.argv[2],'w')
for L in open(sys.argv[1]):
    L=re.sub(r'##(-?)0x([0-9a-f]+)',fix,L.rstrip('\n'))
    L=re.sub(r' <_binary_adsp_b\d+_\w+[+-]0x[0-9a-f]+>','',L)
    notes=[desc(int(x,16)) for x in re.findall(r'##0x([0-9a-f]{8})',L)]
    notes=[n for n in notes if n]
    if notes: L+='    ; '+' | '.join(notes)
    out.write(L+'\n')
