# Combat Simulator maps by the numbers, for the level generator's layout:
# rooms, portals, loops in the room graph, floor levels and stacking.
# Usage (from the game folder): python3 source/tools/levelgen/pdmpmaps.py data/pd.ntsc-final.z64
import struct,zlib,collections,math,sys,os
src=open(os.path.join(os.path.dirname(__file__),'pdthemes.py')).read()
exec(src[:src.index("stats=collections")])
be32=lambda b,o: struct.unpack('>I',b[o:o+4])[0]
be16=lambda b,o: struct.unpack('>H',b[o:o+2])[0]
bef=lambda b,o: struct.unpack('>f',b[o:o+4])[0]
MAPS=[('Ravine','arec'),('Complex','ref'),('G5','cryp'),('Temple','jun'),('Pipes','crad'),('Skedar','oat'),('Base','mp1'),
      ('Area52','mp3'),('Warehouse','mp4'),('Carpark','mp5'),('Ruins','mp9'),('Sewers','mp10'),('Felicity','mp11'),
      ('Fortress','mp12'),('Villa','mp13'),('Grid','mp15')]
def analyse(nm):
    a,b=files['bgdata/bg_%s.seg'%nm]; d=r[a:b]
    inf,s1,pc=be32(d,0),be32(d,4),be32(d,8)
    prim,_=inflate(d[12:])
    seg=0x0f000000
    tab=be32(prim,4)-seg; ptl=be32(prim,8)-seg
    # rooms: table rows until the pointer stops rising
    pos=[None]; i=1
    while True:
        p=be32(prim,tab+20*i)
        if p==0 or (i>1 and p<=be32(prim,tab+20*(i-1))): break
        pos.append((bef(prim,tab+20*i+4),bef(prim,tab+20*i+8),bef(prim,tab+20*i+12))); i+=1
    nrooms=len(pos)-2  # the last row is the end pointer
    # room geometry, rooms in order after the primary block
    p=12+pc; end=12+s1; ext=[]
    while p<end-5 and len(ext)<nrooms:
        if d[p]!=0x11 or d[p+1]!=0x73: p+=1; continue
        try: room,used=inflate(d[p:])
        except Exception: p+=1; continue
        p+=used
        base=be32(room,8)-0x18 if len(room)>=0x18 else 0
        vtx=be32(room,0)-base; col=be32(room,4)-base
        n=(col-vtx)//12 if col>vtx else 0
        ys=[];xs=[];zs=[]
        for k in range(max(0,n)):
            o=vtx+12*k
            if o+6>len(room): break
            x,y,z=struct.unpack('>hhh',room[o:o+6]); xs.append(x); ys.append(y); zs.append(z)
        c=pos[len(ext)+1]
        if ys: ext.append((min(xs)+c[0],max(xs)+c[0],min(ys)+c[1],max(ys)+c[1],min(zs)+c[2],max(zs)+c[2]))
        else: ext.append(None)
    # portals
    edges=set(); k=0
    while True:
        o=ptl+8*k
        if o+8>len(prim) or be16(prim,o)==0: break
        r1,r2=be16(prim,o+2),be16(prim,o+4)
        if r1!=r2 and 0<r1<=nrooms and 0<r2<=nrooms: edges.add((min(r1,r2),max(r1,r2)))
        k+=1
    # graph
    adj=collections.defaultdict(set)
    for u,v in edges: adj[u].add(v); adj[v].add(u)
    seen=set(); comps=0
    for v in range(1,nrooms+1):
        if v in seen: continue
        comps+=1; st=[v]
        while st:
            x=st.pop()
            if x in seen: continue
            seen.add(x); st.extend(adj[x])
    cyc=len(edges)-nrooms+comps
    deg=collections.Counter(len(adj[v]) for v in range(1,nrooms+1))
    rooms=[e for e in ext if e]
    floors=sorted(e[2] for e in rooms)
    span=floors[-1]-floors[0] if floors else 0
    levels=collections.Counter(round(f/100) for f in floors)   # floors in 100-unit bands
    w=max(e[1] for e in rooms)-min(e[0] for e in rooms); dpt=max(e[5] for e in rooms)-min(e[4] for e in rooms)
    # stacking: pairs of rooms that overlap in plan, one wholly above the other
    stack=0
    for i1 in range(len(rooms)):
        A=rooms[i1]
        for i2 in range(i1+1,len(rooms)):
            B=rooms[i2]
            ox=min(A[1],B[1])-max(A[0],B[0]); oz=min(A[5],B[5])-max(A[4],B[4])
            if ox>100 and oz>100 and (A[2]>=B[3]-20 or B[2]>=A[3]-20): stack+=1
    big=sum(1 for e in rooms if (e[1]-e[0])*(e[5]-e[4])>800*800)
    return dict(rooms=nrooms,portals=len(edges),loops=cyc,comps=comps,span=int(span),bands=len(levels),
        top3=[(k*100,v) for k,v in levels.most_common(3)],size=(int(w),int(dpt)),stacked=stack,deg1=deg[1],deg2=deg[2],deg3p=sum(v for k,v in deg.items() if k>=3),big=big)
for name,nm in MAPS:
    try: s=analyse(nm)
    except Exception as e: print(name,'error',e); continue
    print('%-9s rooms %3d portals %3d loops %3d  size %5dx%-5d floor span %4d in %2d bands %s  stacked pairs %3d  dead-ends %3d  2-way %3d  3+way %3d  big %d'%(
        name,s['rooms'],s['portals'],s['loops'],s['size'][0],s['size'][1],s['span'],s['bands'],s['top3'],s['stacked'],s['deg1'],s['deg2'],s['deg3p'],s['big']))
