/*
 * cpuspeed.c -- time a fixed amount of CPU and memory work in guest time.
 *
 *   cpuspeed
 *
 * Runs an integer loop and a pass over a 4 MB buffer (cache misses),
 * times each with GetTickCount and RDTSC, and appends the results to
 * C:\CPUSPEED.TXT. For telling a guest CPU speed change from a
 * graphics one. Built without the C runtime (build.sh).
 */
#include <windows.h>

static HANDLE out;
static char   buf[256];
static DWORD  mem[1024 * 1024];

static void
put(const char *s)
{
    DWORD n;

    WriteFile(out, s, lstrlen(s), &n, NULL);
}

static unsigned __int64
tsc(void)
{
    unsigned int lo, hi;

    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned __int64) hi << 32) | lo;
}

static void
report(const char *what, DWORD ms, unsigned __int64 cycles, DWORD sum)
{
    DWORD kc  = (DWORD) (cycles >> 10); /* no 64-bit division without the C runtime */
    DWORD mhz = ms ? MulDiv(kc, 1024, ms * 1000) : 0;

    wsprintf(buf, "%s: %lu ms, %lu Kicycles (%lu MHz) [%lx]\r\n", what, ms, kc, mhz, sum);
    put(buf);
}

void
entry(void)
{
    DWORD            t0, sum = 1;
    unsigned __int64 c0;
    volatile DWORD  *m = mem;

    out = CreateFile("C:\\CPUSPEED.TXT", GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    SetFilePointer(out, 0, NULL, FILE_END);

    t0 = GetTickCount();
    c0 = tsc();
    for (DWORD i = 0; i < 100000000; i++)
        sum = sum * 1664525 + 1013904223;
    report("int loop", GetTickCount() - t0, tsc() - c0, sum);

    for (DWORD i = 0; i < 1024 * 1024; i++)
        m[i] = i;
    t0 = GetTickCount();
    c0 = tsc();
    for (int pass = 0; pass < 8; pass++)
        for (DWORD i = 0; i < 1024 * 1024; i += 8)
            sum += m[i];
    report("mem pass", GetTickCount() - t0, tsc() - c0, sum);
    CloseHandle(out);
    ExitProcess(0);
}
