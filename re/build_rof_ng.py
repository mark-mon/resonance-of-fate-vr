import pickle, collections
from match import *
R=Img2('/home/claude/rofvr/work/rof_dump.exe',0x7ff723fe0000)
db={}; inv=collections.defaultdict(list)
for b,e in R.funcs:
    if e-b<12: continue
    t=R.toks(b,e,3000); g=ngrams(t)
    db[b]=(len(t),g)
    for h in g: inv[h].append(b)
pickle.dump((db,dict(inv)),open('/home/claude/rofvr/work/rof_ng.pkl','wb'))
print(len(db))
