import struct, re, json
from lib import *
I=Img()
d=I.d
tds={}
for m in re.finditer(rb'\.\?A[VU][^\0]{2,200}@@\0',d):
    tds[m.start()-0x10]=m.group(0)[:-1].decode('latin1')
# COLs: signature 1, pTypeDescriptor rva at +0x0c, pSelf at +0x14
cols={}
rd,rsz=I.sec('.xdata')
for sname in ('.xdata','.arch','.data1'):
    va,sz=I.sec(sname)
    for o in range(va,va+sz-0x18,4):
        if d[o:o+4]==b'\x01\0\0\0':
            td=I.u32(o+0xc); slf=I.u32(o+0x14)
            if slf==o and td in tds: cols[o]=(tds[td],I.u32(o+4))
print('COLs',len(cols))
# vtables: qword == BASE+col, vtable starts right after
vt={}
colset={BASE+c:c for c in cols}
for sname in ('.xdata','.arch'):
    va,sz=I.sec(sname)
    for o in range(va,va+sz-8,8):
        v=I.q(o)
        if v in colset:
            c=colset[v]; vt[o+8]=(cols[c][0],cols[c][1])
print('vtables',len(vt))
json.dump({hex(k+BASE):[v[0],v[1]] for k,v in sorted(vt.items())},open('work/vtables.json','w'),indent=0)
