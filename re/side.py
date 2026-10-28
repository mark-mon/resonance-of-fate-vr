import sys
from match import *
S=Img2('/home/claude/rofvr/work/so4_dump.exe',0x140000000)
R=Img2('/home/claude/rofvr/work/rof_dump.exe',0x7ff723fe0000)
def show(I,a,n):
    f=I.func_of(a); b=f[0] if f else a; e=f[1] if f else a+0x200
    out=[]
    for ins in I.md.disasm(I.d[b:e],I.base+b):
        out.append('%6x: %-6s %s'%(ins.address-I.base,ins.mnemonic,ins.op_str))
        if len(out)>=n: break
    return out
a=int(sys.argv[1],16); b=int(sys.argv[2],16); n=int(sys.argv[3]) if len(sys.argv)>3 else 60
L=show(S,a,n); Rr=show(R,b,n)
for i in range(max(len(L),len(Rr))):
    l=L[i] if i<len(L) else ''; r=Rr[i] if i<len(Rr) else ''
    print('%-70s | %s'%(l[:70],r[:80]))
