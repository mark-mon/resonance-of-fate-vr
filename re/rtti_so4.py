import struct, re, json
from match import *
I=Img2('/home/claude/rofvr/work/so4_dump.exe',0x140000000); d=I.d
def sec(n): return [(v,z) for nn,v,z in I.secs if nn.startswith(n)][0]
u32=lambda o: struct.unpack_from('<I',d,o)[0]; q=lambda o: struct.unpack_from('<Q',d,o)[0]
tds={}
for m in re.finditer(rb'\.\?A[VU][^\0]{2,200}@@\0',d): tds[m.start()-0x10]=m.group(0)[:-1].decode('latin1')
cols={}
for sname in ('.xdata','.arch','.data1'):
    va,sz=sec(sname)
    for o in range(va,va+sz-0x18,4):
        if d[o:o+4]==b'\x01\0\0\0':
            td=u32(o+0xc); slf=u32(o+0x14)
            if slf==o and td in tds: cols[o]=tds[td]
colset={0x140000000+c:c for c in cols}; vt={}
for sname in ('.xdata','.arch'):
    va,sz=sec(sname)
    for o in range(va,va+sz-8,8):
        v=q(o)
        if v in colset: vt[o+8]=cols[colset[v]]
json.dump({hex(k):v for k,v in vt.items()},open('/home/claude/rofvr/work/so4_vtables.json','w'))
print(len(vt))
for a in (0x90F038,0x9296C8,0x92B768,0x914EF8,0x9106F8,0x90DF20): print(hex(a),vt.get(a))
