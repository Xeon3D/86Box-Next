import capstone,sys,re
f=sys.argv[1]; pat=sys.argv[2]
d=open(f,'rb').read()
base=0xb00b0000; code=d[0x400:0x400+228864]
md=capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.skipdata=True
ins=list(md.disasm(code, base+0x1000))
r=re.compile(pat)
ctx=int(sys.argv[3]) if len(sys.argv)>3 else 0
for k,i in enumerate(ins):
    if r.search(i.mnemonic+' '+i.op_str):
        for x in ins[max(0,k-ctx):k+ctx+1]:
            print("%s%08x  %s %s"%('>' if x is i else ' ',x.address,x.mnemonic,x.op_str))
        if ctx: print('--')
