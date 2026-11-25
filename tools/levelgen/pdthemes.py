# Lists the textures Perfect Dark's own bg files put on floors, walls and ceilings,
# with their scale (s/t units per world unit) and command word. For the level generator.
# Usage: python3 tools/levelgen/pdtex.py data/pd.ntsc-final.z64
import struct,zlib,collections,math,sys
r=open(sys.argv[1],'rb').read()
def inflate(b):
    assert b[0]==0x11 and b[1]==0x73
    o=zlib.decompressobj(-15); out=o.decompress(b[5:]); return out, 5+len(b[5:])-len(o.unused_data)
ds,_=inflate(r[0x39850:])
T=0x28080
offs=[]; i=1
while True:
    v=struct.unpack('>I',ds[T+4*i:T+4*i+4])[0]
    if v==0: break
    offs.append(v); i+=1
nb=offs[-1]; names=[]; j=1
while True:
    v=struct.unpack('>I',r[nb+4*j:nb+4*j+4])[0]
    if v==0: break
    names.append(r[nb+v:nb+v+48].split(b'\0')[0].decode('latin1')); j+=1
files={names[k]:(offs[k],offs[k+1]) for k in range(len(names)) if k+1<len(offs)}
be32=lambda b,o: struct.unpack('>I',b[o:o+4])[0]
dens=collections.defaultdict(list); triples=collections.Counter(); tstage=collections.defaultdict(set)
stats=collections.defaultdict(lambda:[0,0.0,0,collections.Counter()])  # key (tex,kind): tris, density sum, n, w0s
for name,(a,b) in sorted(files.items()):
    if not (name.startswith('bgdata/bg_') and name.endswith('.seg')): continue
    d=r[a:b]
    try:
        inf,s1,pc=be32(d,0),be32(d,4),be32(d,8)
        p=12+pc; end=12+s1
    except Exception: continue
    while p<end-5:
        if d[p]!=0x11 or d[p+1]!=0x73: p+=1; continue
        try: room,used=inflate(d[p:])
        except Exception: p+=1; continue
        p+=used
        if len(room)<0x30: continue
        base=be32(room,8)-0x18
        vtx=be32(room,0)-base; nv=struct.unpack('>h',room[0x14:0x16])[0]
        if vtx<0 or vtx+12*nv>len(room): continue
        # find gdl start: scan leaves? just scan entire room for 8-aligned gdl ops after colours
        gs=be32(room,0x18+8)-base
        if gs<0 or gs>=len(room): continue
        tex=None; w0t=0; cur=[]
        rc=collections.defaultdict(lambda:collections.Counter())
        k=gs
        while k+8<=len(room):
            w0,w1=be32(room,k),be32(room,k+4); op=w0>>24; k+=8
            if op==0xc0: tex=w1&0xfff; w0t=w0
            elif op==0x04:
                n=((w0>>20)&0xf)+1; v0=(w0>>16)&0xf; off=(w1&0xffffff)
                cur=[None]*16
                for q in range(n):
                    o=vtx+off+12*q
                    if o+12<=len(room):
                        x,y,z=struct.unpack('>hhh',room[o:o+6]); s,t=struct.unpack('>hh',room[o+8:o+12])
                        if v0+q<16: cur[v0+q]=(x,y,z,s,t)
            elif op==0xb1 and tex is not None:
                for q in range(4):
                    ia=(w1>>(8*q))&0xf; ib=(w1>>(8*q+4))&0xf; ic=(w0>>(4*q))&0xf
                    if ia==ib==ic==0: continue
                    A,B,C=cur[ia],cur[ib],cur[ic]
                    if not(A and B and C): continue
                    u=[B[i]-A[i] for i in range(3)]; v=[C[i]-A[i] for i in range(3)]
                    nx=u[1]*v[2]-u[2]*v[1]; ny=u[2]*v[0]-u[0]*v[2]; nz=u[0]*v[1]-u[1]*v[0]
                    ln=math.sqrt(nx*nx+ny*ny+nz*nz)
                    if ln==0: continue
                    kind='floor' if ny/ln>0.7 else ('ceil' if ny/ln<-0.7 else 'wall')
                    dp=math.sqrt(sum(c*c for c in u)); du=math.hypot(B[3]-A[3],B[4]-A[4])
                    st=stats[(tex,kind)]; st[0]+=1; rc[kind][tex]+=1
                    dens[(tex,kind)].append(du/dp if dp>0 else 0)
                    if dp>0: st[1]+=du/dp; st[2]+=1
                    st[3][w0t]+=1
            elif op==0xb8: break
        if rc['floor'] and rc['wall'] and rc['ceil']:
            trip=tuple(rc[k].most_common(1)[0][0] for k in ('floor','wall','ceil'))
            triples[(trip)]+=1; tstage[trip].add(name)

import statistics
print('top room triples (floor,wall,ceil):rooms, stages')
for t,n in triples.most_common(40):
    f=[statistics.median(dens[(t[i],k)]) for i,k in enumerate(('floor','wall','ceil'))]
    print(' %s x%3d  scale %s  %s'%(tuple('%#05x'%x for x in t),n,tuple('%.1f'%x for x in f),sorted(x[10:-4] for x in tstage[t])[:6]))
