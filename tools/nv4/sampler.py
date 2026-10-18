# sampler.py PID SECONDS NMFILE -- host profiler for a running 86Box-Next: samples
# every thread's RIP and names it from NMFILE (nm build-static/src/86Box-Next.exe).
import ctypes, ctypes.wintypes as W, sys, time, bisect, collections, subprocess

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
psapi = ctypes.WinDLL('psapi', use_last_error=True)
for f in ('OpenProcess', 'OpenThread', 'CreateToolhelp32Snapshot'):
    getattr(k32, f).restype = ctypes.c_void_p
for f in ('CloseHandle', 'SuspendThread', 'ResumeThread'):
    getattr(k32, f).argtypes = [ctypes.c_void_p]
k32.SuspendThread.restype = W.DWORD
k32.GetThreadContext.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
k32.Thread32First.argtypes = k32.Thread32Next.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
psapi.EnumProcessModulesEx.argtypes = [ctypes.c_void_p, ctypes.c_void_p, W.DWORD, ctypes.c_void_p, W.DWORD]
psapi.GetModuleInformation.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, W.DWORD]
psapi.GetModuleBaseNameW.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_wchar_p, W.DWORD]
pid, secs, nmfile = int(sys.argv[1]), float(sys.argv[2]), sys.argv[3]

TH32CS_SNAPTHREAD = 4
class THREADENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', W.DWORD), ('cntUsage', W.DWORD), ('th32ThreadID', W.DWORD), ('th32OwnerProcessID', W.DWORD),
                ('tpBasePri', W.LONG), ('tpDeltaPri', W.LONG), ('dwFlags', W.DWORD)]

class CONTEXT(ctypes.Structure):
    _fields_ = [('P1Home', ctypes.c_uint64)] * 1 + [('P%dHome' % i, ctypes.c_uint64) for i in range(2, 7)] + [
        ('ContextFlags', W.DWORD), ('MxCsr', W.DWORD), ('SegCs', W.WORD), ('SegDs', W.WORD), ('SegEs', W.WORD),
        ('SegFs', W.WORD), ('SegGs', W.WORD), ('SegSs', W.WORD), ('EFlags', W.DWORD)] + [
        ('Dr%d' % i, ctypes.c_uint64) for i in (0, 1, 2, 3, 6, 7)] + [
        (r, ctypes.c_uint64) for r in ('Rax', 'Rcx', 'Rdx', 'Rbx', 'Rsp', 'Rbp', 'Rsi', 'Rdi', 'R8', 'R9', 'R10', 'R11',
                                       'R12', 'R13', 'R14', 'R15', 'Rip')] + [('rest', ctypes.c_byte * 1024)]

def threads():
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32(); te.dwSize = ctypes.sizeof(te)
    out = []
    ok = k32.Thread32First(snap, ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            out.append(te.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(te))
    k32.CloseHandle(snap)
    return out

# modules
hp = k32.OpenProcess(0x0410, False, pid)
mods = (ctypes.c_void_p * 1024)(); need = W.DWORD()
psapi.EnumProcessModulesEx(hp, mods, ctypes.sizeof(mods), ctypes.byref(need), 3)
class MI(ctypes.Structure):
    _fields_ = [('base', ctypes.c_void_p), ('size', W.DWORD), ('entry', ctypes.c_void_p)]
modlist = []
for m in mods[:need.value // 8]:
    mi = MI(); name = ctypes.create_unicode_buffer(260)
    psapi.GetModuleInformation(hp, ctypes.c_void_p(m), ctypes.byref(mi), ctypes.sizeof(mi))
    psapi.GetModuleBaseNameW(hp, ctypes.c_void_p(m), name, 260)
    modlist.append((mi.base or 0, mi.size, name.value))
exe = [m for m in modlist if m[2].lower().endswith('.exe')][0]

syms = []
for line in open(nmfile):
    p = line.split()
    if len(p) == 3 and p[1] in 'tT':
        syms.append((int(p[0], 16), p[2]))
syms.sort()
addrs = [s[0] for s in syms]
pref = 0x140000000

def sym(rip):
    if exe[0] <= rip < exe[0] + exe[1]:
        a = rip - exe[0] + pref
        i = bisect.bisect_right(addrs, a) - 1
        return syms[i][1] if i >= 0 else '?exe'
    for b, s, n in modlist:
        if b <= rip < b + s:
            return n
    return 'jit/%x' % (rip >> 20)

ctxbuf = ctypes.create_string_buffer(ctypes.sizeof(CONTEXT) + 64)
ctxaddr = (ctypes.addressof(ctxbuf) + 15) & ~15
ctx = CONTEXT.from_address(ctxaddr)
hits = collections.defaultdict(collections.Counter)
end = time.time() + secs
n = 0
while time.time() < end:
    for tid in threads():
        ht = k32.OpenThread(0x001A, False, tid)  # SUSPEND_RESUME | GET_CONTEXT | QUERY
        if not ht:
            continue
        if k32.SuspendThread(ht) != 0xffffffff:
            ctypes.memset(ctxbuf, 0, ctypes.sizeof(ctxbuf))
            ctx.ContextFlags = 0x100001
            if k32.GetThreadContext(ht, ctypes.c_void_p(ctxaddr)):
                hits[tid][sym(ctx.Rip)] += 1
            k32.ResumeThread(ht)
        k32.CloseHandle(ht)
    n += 1
    time.sleep(0.002)
print('%d rounds' % n)
for tid, c in sorted(hits.items(), key=lambda kv: -sum(v for k, v in kv[1].items() if not k.startswith(('ntdll', 'KERNELBASE', 'win32u')))):
    busy = sum(c.values())
    top = c.most_common(14)
    if top[0][0].startswith(('ntdll', 'win32u')) and top[0][1] > busy * 0.9:
        continue
    print('thread %d: %d samples' % (tid, busy))
    for k, v in top:
        print('   %5.1f%% %s' % (100.0 * v / busy, k))
