# Parse rof_upl.bin (one frame of constant-buffer uploads + main camera records) and locate the camera view.
import struct, sys, numpy as np
d=open(sys.argv[1],'rb').read(); o=0
magic,nu,nr=struct.unpack_from('<III',d,o); o+=12; now,pc=struct.unpack_from('<qQ',d,o); o+=16
recs=[]
for i in range(nr):
    seq,t,pres=struct.unpack_from('<QqQ',d,o); m=np.frombuffer(d,dtype='<f4',count=7*16,offset=o+24).reshape(7,4,4); o+=24+448
    recs.append((seq,m))
ups=[]
for i in range(nu):
    buf,by,bb,how,off=struct.unpack_from('<QIIII',d,o); rva=struct.unpack_from('<4I',d,o+24); o+=40
    ups.append((buf,by,bb,how,off,rva))
nd=struct.unpack_from('<I',d,o)[0]; o+=4; data=d[o:o+nd]; o+=nd
print('uploads',nu,'records',nr,'data',nd)
from collections import Counter
print('sizes',Counter((u[1],u[2]) for u in ups).most_common(12))
recs.sort()
if recs:
    seq,m=recs[-1]; view=m[1]; print('newest record',seq,'world row0',m[0][0],'view rows\n',view)
    V=[r[1][1] for r in recs]
    # find view rows (first 3 rows, 12 floats) inside uploads of the per-view size
    for ui,(buf,by,bb,how,off,rva) in enumerate(ups):
        a=np.frombuffer(data,dtype='<f4',count=by//4,offset=off)
        for (sq,m) in recs[-6:]:
            v=m[1].reshape(-1)
            for tr,vv in (('row',v[:12]),('col',m[1].T.reshape(-1)[:12])):
                for i in range(0,len(a)-12+1):
                    if np.allclose(a[i:i+12],vv,rtol=1e-4,atol=1e-4):
                        print('upload %d size %d/%d how %d: view(%s) of rec %d at +0x%X  rva %s'%(ui,by,bb,how,tr,sq,i*4,[hex(x) for x in rva]))
