import sys
from match import *
from capstone.x86 import *
which=sys.argv[1]; lo=int(sys.argv[2],16); hi=int(sys.argv[3],16)
I=Img2('/home/claude/rofvr/work/so4_dump.exe',0x140000000) if which=='so4' else Img2('/home/claude/rofvr/work/rof_dump.exe',0x7ff723fe0000)
seclo,sechi=(0x1000,0x90c000) if which=='so4' else (0x1000,0x9e2000)
import collections; hits=collections.defaultdict(list)
for b,e in I.funcs:
    if not (seclo<=b<sechi): continue
    for ins in I.md.disasm(I.d[b:e],I.base+b):
        if ins.id==0: continue
        for op in ins.operands:
            if op.type==X86_OP_MEM and op.mem.base!=X86_REG_RIP and lo<=op.mem.disp<hi:
                hits[b].append((ins.address-I.base,ins.mnemonic,ins.op_str))
for b,l in hits.items():
    print('func %x: %d refs'%(b,len(l)), ' '.join('%x'%a for a,_,_ in l[:6]))
