import sys
from match import *
from capstone.x86 import *
def funcs_with(I, disps, sec_lo=None, sec_hi=None, maxfun=4000):
    res=[]
    for b,e in I.funcs:
        if e-b>maxfun*8: continue
        if sec_lo and not (sec_lo<=b<sec_hi): continue
        found=set()
        for ins in I.md.disasm(I.d[b:e],I.base+b):
            if ins.id==0: continue
            for op in ins.operands:
                if op.type==X86_OP_MEM and op.mem.base!=X86_REG_RIP and op.mem.disp in disps: found.add(op.mem.disp)
        if found==set(disps): res.append((b,e))
    return res
if __name__=='__main__':
    which=sys.argv[1]; disps=[int(x,16) for x in sys.argv[2:]]
    I=Img2('/home/claude/rofvr/work/so4_dump.exe',0x140000000) if which=='so4' else Img2('/home/claude/rofvr/work/rof_dump.exe',0x7ff723fe0000)
    lo,hi=(0x1000,0x90c000) if which=='so4' else (0x1000,0x9e2000)
    for b,e in funcs_with(I,disps,lo,hi): print(hex(b),hex(e))
