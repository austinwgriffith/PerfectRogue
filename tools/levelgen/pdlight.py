# Lists the textures Perfect Dark's rooms draw facing down from their ceilings, by the
# mean vertex colour they are drawn with: light fittings are drawn at full brightness (255).
# For the level generator's lights. Usage: python3 tools/levelgen/pdlight.py data/pd.ntsc-final.z64
# (run from the game's folder; reads pdthemes.py next to it)
import sys,struct,collections,math
src=open(__import__('os').path.join(__import__('os').path.dirname(__file__),'pdthemes.py')).read()
exec(src[:src.index("stats=collections")])
be32=lambda b,o: struct.unpack('>I',b[o:o+4])[0]
st=collections.defaultdict(lambda:[0,0.0,0.0,set(),collections.Counter()])  # tex: tris, sum colour max, area sum, stages, w0
for name,(a,b) in sorted(files.items()):
    if not (name.startswith('bgdata/bg_') and name.endswith('.seg')): continue
    d=r[a:b]
    try:
        inf,s1,pc=be32(d,0),be32(d,4),be32(d,8); p=12+pc; end=12+s1
    except Exception: continue
    while p<end-5:
        if d[p]!=0x11 or d[p+1]!=0x73: p+=1; continue
        try: room,used=inflate(d[p:])
        except Exception: p+=1; continue
        p+=used
        if len(room)<0x30: continue
        base=be32(room,8)-0x18
        vtx=be32(room,0)-base; col=be32(room,4)-base
        gs=be32(room,0x18+8)-base
        if gs<0 or gs>=len(room) or col<0: continue
        tex=None; w0t=0; cur=[None]*16; colb=0
        k=gs
        while k+8<=len(room):
            w0,w1=be32(room,k),be32(room,k+4); op=w0>>24; k+=8
            if op==0xc0: tex=w1&0xfff; w0t=w0
            elif op==0x07: colb=w1&0xffffff
            elif op==0x04:
                n=((w0>>20)&0xf)+1; v0=(w0>>16)&0xf; off=(w1&0xffffff)
                for q in range(n):
                    o=vtx+off+12*q
                    if o+12<=len(room) and v0+q<16:
                        x,y,z=struct.unpack('>hhh',room[o:o+6]); ci=room[o+7]
                        co=col+colb+ci
                        c=room[co:co+4] if co+4<=len(room) else b'\0\0\0\0'
                        cur[v0+q]=(x,y,z,max(c[0],c[1],c[2]))
            elif op==0xb1 and tex is not None:
                for q in range(4):
                    ia=(w1>>(8*q))&0xf; ib=(w1>>(8*q+4))&0xf; ic=(w0>>(4*q))&0xf
                    if ia==ib==ic==0: continue
                    A,B,C=cur[ia],cur[ib],cur[ic]
                    if not(A and B and C): continue
                    u=[B[i]-A[i] for i in range(3)]; v=[C[i]-A[i] for i in range(3)]
                    nx=u[1]*v[2]-u[2]*v[1]; ny=u[2]*v[0]-u[0]*v[2]; nz=u[0]*v[1]-u[1]*v[0]
                    ln=math.sqrt(nx*nx+ny*ny+nz*nz)
                    if ln==0 or ny/ln>-0.7: continue   # ceiling-facing only (normal down)
                    s=st[tex]; s[0]+=1; s[1]+=(A[3]+B[3]+C[3])/3; s[2]+=ln/2; s[3].add(name[10:-4]); s[4][w0t]+=1
            elif op==0xb8: break
rows=[(v[1]/v[0],t,v) for t,v in st.items() if v[0]>=12]
rows.sort(reverse=True)
for m,t,v in rows[:30]:
    print('tex %#05x tris %5d meancol %5.1f meanarea %8.0f stages %2d %s w0 %s'%(t,v[0],m,v[2]/v[0],len(v[3]),sorted(v[3])[:5],hex(v[4].most_common(1)[0][0])))
