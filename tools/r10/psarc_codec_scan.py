"""R10 AJM (docs/R10_AJM.md 1.2): which audio codecs the game's PSARC archives hold.

For every entry of each archive (TOC, zlib blocks) counts the RIFF "fmt " codec tags (0xFFFF = Wwise Vorbis) by
channels and rate, and the entries that carry the ATRAC9 GUID.
  python -I tools/r10/psarc_codec_scan.py E:/shdps4/CUSA00004/art/cache/psarc/english_a.psarc ...
"""
import struct,zlib,re,collections,sys,os
def scan(path):
    f=open(path,'rb')
    h=f.read(32)
    magic,ver,comp,toclen,esz,ne,bsz,flags=struct.unpack('>4sI4sIIIII',h)
    toc=f.read(toclen-32)
    ents=[]
    for i in range(ne):
        e=toc[i*esz:(i+1)*esz]
        bi=struct.unpack('>I',e[16:20])[0]
        us=int.from_bytes(e[20:25],'big'); off=int.from_bytes(e[25:30],'big')
        ents.append((bi,us,off))
    nb=1
    while (1<<(8*nb)) < bsz: nb+=1
    bl=toc[ne*esz:]
    sizes=[int.from_bytes(bl[i:i+nb],'big') for i in range(0,len(bl)-nb+1,nb)]
    cnt=collections.Counter(); at9=0
    for (bi,us,off) in ents[1:]:
        f.seek(off); out=bytearray(); b=bi
        while len(out)<us:
            sz=sizes[b] or bsz
            raw=f.read(sz)
            if raw[:1]==b'\x78':
                try: raw=zlib.decompress(raw)
                except Exception: pass
            out+=raw; b+=1
            if len(out)>=8<<20 and b'RIFF' not in out[:65536]: break
        cnt["m:"+out[8:12].decode("latin1")]+=1
        for m in re.finditer(rb'fmt ',out[:262144]):
            p=m.start()+8
            if p+8>len(out): continue
            fmt=struct.unpack_from('<HHI',out,p)
            cnt[(hex(fmt[0]),fmt[1],fmt[2])]+=1
        if b'\xD2\x42\xE1\x47\xBA\x36\x8D\x4D' in out: at9+=1
    print(os.path.basename(path), 'entries',ne,'fmt',dict(cnt.most_common(14)),'at9guid_entries',at9, flush=True)
for p in sys.argv[1:]: scan(p)
