import struct,sys
# LE (VxD) parser: objects, and internal fixups -> {file offset of source: target file offset}
def load(path):
    d=open(path,'rb').read()
    le=struct.unpack('<I',d[0x3c:0x40])[0]
    h=lambda o,f: struct.unpack(f,d[le+o:le+o+struct.calcsize(f)])[0]
    pagesize=h(0x28,'<I'); objtab=le+h(0x40,'<I'); nobj=h(0x44,'<I')
    pagemap=le+h(0x48,'<I'); fpt=le+h(0x68,'<I'); frt=le+h(0x6c,'<I')
    datapages=h(0x80,'<I'); npages=h(0x14,'<I'); lastpage=h(0x2c,'<I')
    objs=[]
    for i in range(nobj):
        o=objtab+i*24
        vsize,base,flags,pmi,pmn,_=struct.unpack('<IIIIII',d[o:o+24])
        objs.append((vsize,base,flags,pmi,pmn))
    def pagefile(p): # 1-based page -> file offset
        return datapages+(p-1)*pagesize
    # map (obj, offset) -> file offset
    def objoff2file(oi,off):
        vsize,base,flags,pmi,pmn=objs[oi]
        p=off//pagesize
        if p>=pmn: return None
        return pagefile(pmi+p)+off%pagesize
    fix={}
    for oi,(vsize,base,flags,pmi,pmn) in enumerate(objs):
        for pi in range(pmn):
            page=pmi+pi
            a=struct.unpack('<I',d[fpt+(page-1)*4:fpt+page*4])[0]
            b=struct.unpack('<I',d[fpt+page*4:fpt+page*4+4])[0]
            p=frt+a
            while p<frt+b:
                src=d[p]; flg=d[p+1]; p+=2
                cnt=1
                if src&0x20: cnt=d[p]; p+=1
                else:
                    so=struct.unpack('<h',d[p:p+2])[0]; p+=2
                if flg&3==0:
                    tobj=d[p] if not flg&0x40 else struct.unpack('<H',d[p:p+2])[0]; p+= 1 if not flg&0x40 else 2
                    if (src&0xf)!=2:
                        toff=struct.unpack('<I',d[p:p+4])[0] if flg&0x10 else struct.unpack('<H',d[p:p+2])[0]
                        p+= 4 if flg&0x10 else 2
                    else: toff=0
                    if src&0x20:
                        for k in range(cnt):
                            so=struct.unpack('<h',d[p:p+2])[0]; p+=2
                            fix[(oi,pi*pagesize+so)]=(tobj-1,toff)
                    else:
                        fix[(oi,pi*pagesize+so)]=(tobj-1,toff)
                else:
                    break
    return d,objs,objoff2file,fix,pagesize,pagefile
