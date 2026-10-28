# Functions that test a byte field "& 0x11 == 1" and later OR 0x10 into the same field (DF linear-depth "done" pattern)
from match import *
from capstone.x86 import *
R=Img2('/home/claude/rofvr/work/rof_dump.exe',0x7ff723fe0000)
hits=[]
for b,e in R.funcs:
    if not (0x1000<=b<0x9e2000) or e-b>20000: continue
    ins=[i for i in R.md.disasm(R.d[b:e],R.base+b) if i.id]
    ors={}; ands=[]
    for k,i in enumerate(ins):
        if i.mnemonic=='or' and len(i.operands)==2 and i.operands[0].type==X86_OP_MEM and i.operands[0].size==1 and i.operands[1].type==X86_OP_IMM and i.operands[1].imm==0x10:
            ors[i.operands[0].mem.disp]=i.address-R.base
        if i.mnemonic=='and' and len(i.operands)==2 and i.operands[1].type==X86_OP_IMM and i.operands[1].imm==0x11:
            ands.append(i.address-R.base)
    if ors and ands: hits.append((b,ands[:2],{hex(k):hex(v) for k,v in ors.items()}))
for h in hits: print(hex(h[0]),[hex(x) for x in h[1]],h[2])
