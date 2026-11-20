# wsdetail.ps1 - where a process's private working set goes.
#
#   .\tools\wsdetail.ps1 -Id <pid> [-Top 15]
#
# Walks every committed region with VirtualQueryEx, asks QueryWorkingSetEx which
# pages are resident and not shared, and groups them:
#   module.dll   copy-on-write data pages of that DLL/EXE
#   private      VirtualAlloc / heap memory (driver heaps, our heap, stacks...)
#   mapped:<f>   private pages of file mappings and sections
# "Private working set" is what Task Manager shows in its Memory column.
param([Parameter(Mandatory)][int]$Id, [int]$Top = 15)

Add-Type @'
using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Text;
public static class Ws {
  [StructLayout(LayoutKind.Sequential)] struct MBI {
    public IntPtr Base, AllocBase; public uint AllocProtect; public ushort Part; public IntPtr Size;
    public uint State, Protect, Type; }
  [StructLayout(LayoutKind.Sequential)] struct WSX { public IntPtr Va; public IntPtr Attr; }
  [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint a, bool i, int pid);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll")] static extern IntPtr VirtualQueryEx(IntPtr h, IntPtr a, out MBI m, IntPtr n);
  [DllImport("psapi.dll")] static extern bool QueryWorkingSetEx(IntPtr h, [In, Out] WSX[] w, int cb);
  [DllImport("psapi.dll", CharSet = CharSet.Unicode)] static extern uint GetMappedFileNameW(IntPtr h, IntPtr a, StringBuilder s, uint n);
  public static Dictionary<string, long> Run(int pid) {
    var r = new Dictionary<string, long>();
    IntPtr h = OpenProcess(0x0410, false, pid);
    if (h == IntPtr.Zero) throw new Exception("OpenProcess failed");
    long a = 0; MBI m; int sz = Marshal.SizeOf(typeof(MBI));
    while (VirtualQueryEx(h, (IntPtr)a, out m, (IntPtr)sz) != IntPtr.Zero) {
      long size = (long)m.Size;
      if (m.State == 0x1000) {
        string key = "private";
        if (m.Type != 0x20000) {
          var sb = new StringBuilder(512);
          string f = GetMappedFileNameW(h, m.Base, sb, 512) > 0 ? sb.ToString() : "(section)";
          f = f.Substring(f.LastIndexOf('\\') + 1);
          key = m.Type == 0x1000000 ? f : "mapped:" + f;
        }
        int pages = (int)(size / 4096);
        var w = new WSX[pages];
        for (int i = 0; i < pages; i++) w[i].Va = (IntPtr)((long)m.Base + i * 4096L);
        if (QueryWorkingSetEx(h, w, pages * Marshal.SizeOf(typeof(WSX)))) {
          long kb = 0;
          foreach (var x in w) { long at = (long)x.Attr; if ((at & 1) != 0 && (at & 0x8000) == 0) kb += 4; }
          if (kb > 0) { long v; r.TryGetValue(key, out v); r[key] = v + kb; }
        }
      }
      a = (long)m.Base + size;
      if (a >= 0x7FFFFFFF0000L) break;
    }
    CloseHandle(h);
    return r;
  }
}
'@

$r = [Ws]::Run($Id)
$total = ($r.Values | Measure-Object -Sum).Sum
'{0,-40} {1,8:N0} KB' -f 'TOTAL private working set', $total
$r.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First $Top |
    ForEach-Object { '{0,-40} {1,8:N0} KB' -f $_.Key, $_.Value }
