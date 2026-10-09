import capstone,struct,sys
f=sys.argv[1]; va=int(sys.argv[2],16); n=int(sys.argv[3],16)
d=open(f,'rb').read()
pe=struct.unpack('<I',d[0x3c:0x40])[0]
nsec=struct.unpack('<H',d[pe+6:pe+8])[0]; opt=struct.unpack('<H',d[pe+20:pe+22])[0]
base=struct.unpack('<I',d[pe+24+28:pe+24+32])[0]
for i in range(nsec):
    o=pe+24+opt+40*i; vs,sva,rs,ro=struct.unpack('<IIII',d[o+8:o+24])
    if base+sva<=va<base+sva+rs: off=va-base-sva+ro
md=capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
for i in md.disasm(d[off:off+n], va):
    print("%08x  %-24s %s %s"%(i.address, i.bytes.hex(), i.mnemonic, i.op_str))
