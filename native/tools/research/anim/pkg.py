import sys,struct,bisect,collections
sys.path.insert(0,r'E:\SecondSonPC-rebirth\native\tools\research')
import kcap
BASE='E:/shdps4/CUSA00004/art/cache/'
class Pkg:
    def __init__(s,name):
        s.name=name
        s.d=open(BASE+name+'.xpps','rb').read(); s.h=kcap.parse(s.d); s.hs=s.h['header_size']
        s.segs=kcap.segments(s.d,s.h); s.bl=kcap.blocks(s.d,s.h)
        s.links=[]; s.dic=[]; s.pyto=[]; s.knlx=[]
        for b in s.bl:
            if b['type']!=2: continue
            st=s.hs+b['offset']
            for tag,p,size in kcap.chunks(s.d,st,st+b['size']):
                if tag==kcap.TAG_KNLI: s.links+=kcap.knli(s.d,p)
                elif tag==kcap.TAG_DIC:
                    n=kcap.u32(s.d,p)
                    for i in range(n):
                        loc,z,hh=struct.unpack_from('<IIQ',s.d,p+8+i*16)
                        s.dic.append((loc,hh))
                elif tag==kcap.TAG_PYTO:
                    n=kcap.u32(s.d,p)
                    for i in range(n):
                        s.pyto.append(struct.unpack_from('<IBBBBQ',s.d,p+4+i*16))
        s.linkset={loc:k for k,loc in s.links}
        s.dic.sort()
        s.dicloc=[x[0] for x in s.dic]
        st=sorted((b['offset'],i) for i,b in enumerate(s.bl))
        s.boffs=[x[0] for x in st]; s.bidx=[x[1] for x in st]
    def u32(s,o): return struct.unpack_from('<I',s.d,s.hs+o)[0]
    def u64(s,o): return struct.unpack_from('<Q',s.d,s.hs+o)[0]
    def f32(s,o): return struct.unpack_from('<f',s.d,s.hs+o)[0]
    def ptr(s,o):
        return s.u32(o) if o in s.linkset else None
    def block_of(s,o):
        j=bisect.bisect_right(s.boffs,o)-1
        i=s.bidx[j]; return i,s.bl[i]
    def dic_before(s,o):
        j=bisect.bisect_right(s.dicloc,o)-1
        return s.dic[j] if j>=0 else None
    def hexd(s,o,n):
        for r in range(o,o+n,16): print('   %8x: %s'%(r,kcap.hexrow(s.d,s.hs+r)))
