"""Havok 2013.2 binary packfile reader (research)."""
import struct
def u32(b,o): return struct.unpack_from('<I',b,o)[0]
class Packfile:
    def __init__(s, d, base):
        s.d=d; s.b=base
        assert d[base:base+8]==bytes.fromhex('57e0e05710c0c010')
        s.ver=u32(d,base+0xc); s.rules=d[base+0x10:base+0x14]
        s.nsec=u32(d,base+0x14)
        s.contents=(u32(d,base+0x18),u32(d,base+0x1c)); s.contents_cls=(u32(d,base+0x20),u32(d,base+0x24))
        s.version=d[base+0x28:base+0x38].split(b'\0')[0].decode()
        s.flags=u32(d,base+0x38)
        pad=struct.unpack_from('<hh',d,base+0x3c)
        hdr=0x40 + (pad[1] if pad[0]!=-1 else 0)
        s.secs=[]
        o=base+hdr
        secsize=0x40 if s.ver>=11 else 0x30
        for i in range(s.nsec):
            tag=d[o:o+19].split(b'\0')[0].decode()
            vals=struct.unpack_from('<7I',d,o+20)
            s.secs.append({'tag':tag,'start':vals[0],'local':vals[1],'global':vals[2],'virtual':vals[3],'exports':vals[4],'imports':vals[5],'end':vals[6]})
            o+=secsize
    def sec_abs(s,i): return s.b+s.secs[i]['start']
    def classnames(s):
        sec=[x for x in s.secs if x['tag']=='__classnames__'][0]
        st=s.b+sec['start']; out={}; o=st
        while o < st+sec['local']:
            sig=u32(s.d,o); flag=s.d[o+4]
            if flag!=0x09: break
            name=s.d[o+5:s.d.index(b'\0',o+5)].decode()
            out[o+5-st]=name; o=o+5+len(name)+1
        return out
    def objects(s):
        cn=s.classnames()
        di=[i for i,x in enumerate(s.secs) if x['tag']=='__data__'][0]
        sec=s.secs[di]; st=s.b+sec['start']
        out=[]
        o=st+sec['virtual']
        while o < st+sec['exports']:
            src,cs,co=struct.unpack_from('<III',s.d,o)
            if src==0xffffffff: break
            out.append((src,cn.get(co,'?%x'%co)))
            o+=12
        return out
    def local_fixups(s, di):
        sec=s.secs[di]; st=s.b+sec['start']; out={}
        o=st+sec['local']
        while o < st+sec['global']:
            a,b=struct.unpack_from('<II',s.d,o)
            if a!=0xffffffff: out[a]=b
            o+=8
        return out
    def global_fixups(s, di):
        sec=s.secs[di]; st=s.b+sec['start']; out={}
        o=st+sec['global']
        while o < st+sec['virtual']:
            a,b,c=struct.unpack_from('<III',s.d,o)
            if a!=0xffffffff: out[a]=(b,c)
            o+=12
        return out
