param([string]$Exe, [string]$ArgLine, [string]$Dir, [string]$CmdFile)
# Starts a program minimized WITHOUT activating it (SW_SHOWMINNOACTIVE), so it
# never takes the keyboard focus from whatever the user is doing.
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class NoAct {
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
  public struct SI { public int cb; public string r, d, t; public int x, y, w, h, cx, cy, fa, fl; public short show, r2; public IntPtr r3, i, o, e; }
  [StructLayout(LayoutKind.Sequential)]
  public struct PI { public IntPtr hp, ht; public int pid, tid; }
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  public static extern bool CreateProcess(string app, string cmd, IntPtr pa, IntPtr ta, bool inh, uint fl, IntPtr env, string dir, ref SI si, out PI pi);
  public static int Start(string exe, string cmd, string dir) {
    SI si = new SI(); si.cb = Marshal.SizeOf(si); si.fl = 1; si.show = 7;
    PI pi; if (!CreateProcess(exe, cmd, IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, dir, ref si, out pi)) return -Marshal.GetLastWin32Error();
    return pi.pid; }
}
'@
$env:BOX86NEXT_DEBUG_CMD = $CmdFile
[NoAct]::Start($Exe, "`"$Exe`" $ArgLine", $Dir)
