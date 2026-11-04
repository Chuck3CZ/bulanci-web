# threadcpu-win: per-thread CPU time of a Chrome run's processes (Windows), for web/tools/bench.mjs.
#   pwsh -NoProfile -File web/tools/threadcpu-win.ps1
# A small server on stdin/stdout, so a snapshot costs milliseconds, not a PowerShell start:
#   in:  "snap <text>"   every chrome.exe whose command line contains <text> (the run's --user-data-dir)
#   out: one line per thread, then "END <epoch ms of the snapshot>":
#        pid thread_id cpu_ns name type cycle_ns
#   cpu_ns is kernel + user time (GetThreadTimes: clock-tick accounting, fine over seconds), cycle_ns the
#   thread's cycle count (QueryThreadCycleTime, exact) at the TSC rate measured at start.  type is the
#   process's --type= (renderer, gpu-process, utility, ...) or "browser".  Names are Chrome's own thread
#   descriptions (CrRendererMain, DedicatedWorker thread, CrGpuMain, ...), spaces as underscores.
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
public static class ThreadCpu {
  [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenThread(uint access, bool inherit, uint id);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll")] static extern bool GetThreadTimes(IntPtr h, out long c, out long e, out long k, out long u);
  [DllImport("kernel32.dll")] static extern bool QueryThreadCycleTime(IntPtr h, out ulong cycles);
  [DllImport("kernel32.dll")] static extern IntPtr GetCurrentThread();
  [DllImport("kernel32.dll")] static extern int GetThreadDescription(IntPtr h, out IntPtr desc);
  [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr p);
  const uint QUERY_LIMITED = 0x0800;
  public static double TscPerNs = 1.0;
  public static void Calibrate() {
    ulong c0, c1; var sw = Stopwatch.StartNew();
    QueryThreadCycleTime(GetCurrentThread(), out c0);
    while (sw.ElapsedMilliseconds < 200) { }
    QueryThreadCycleTime(GetCurrentThread(), out c1);
    TscPerNs = (c1 - c0) / (sw.Elapsed.TotalMilliseconds * 1e6);
  }
  public static string Snap(int[] pids, string[] types) {
    var sb = new StringBuilder();
    for (int i = 0; i < pids.Length; i++) {
      Process p;
      try { p = Process.GetProcessById(pids[i]); } catch { continue; }
      foreach (ProcessThread t in p.Threads) {
        IntPtr h = OpenThread(QUERY_LIMITED, false, (uint)t.Id);
        if (h == IntPtr.Zero) continue;
        long c, e, k, u; ulong cy = 0; string name = "-";
        if (!GetThreadTimes(h, out c, out e, out k, out u)) { CloseHandle(h); continue; }
        QueryThreadCycleTime(h, out cy);
        IntPtr d;
        if (GetThreadDescription(h, out d) >= 0 && d != IntPtr.Zero) {
          string s = Marshal.PtrToStringUni(d); LocalFree(d);
          if (!string.IsNullOrEmpty(s)) name = s.Replace(' ', '_');
        }
        CloseHandle(h);
        sb.Append(pids[i]).Append(' ').Append(t.Id).Append(' ').Append((k + u) * 100).Append(' ').Append(name)
          .Append(' ').Append(types[i]).Append(' ').Append((long)(cy / TscPerNs)).Append('\n');
      }
    }
    return sb.ToString();
  }
}
'@
[ThreadCpu]::Calibrate()
$cache = @{}
while ($null -ne ($line = [Console]::In.ReadLine())) {
  if (-not $line.StartsWith('snap ')) { continue }
  $key = $line.Substring(5)
  if (-not $cache.ContainsKey($key)) {
    $procs = @(Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" | Where-Object { $_.CommandLine -and $_.CommandLine.Contains($key) })
    $types = @($procs | ForEach-Object { if ($_.CommandLine -match '--type=([\w-]+)') { $Matches[1] } else { 'browser' } })
    if ($procs.Count) { $cache[$key] = @{ pids = [int[]]@($procs | ForEach-Object { [int]$_.ProcessId }); types = [string[]]$types } }
  }
  $t = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
  if ($cache.ContainsKey($key)) { [Console]::Out.Write([ThreadCpu]::Snap($cache[$key].pids, $cache[$key].types)) }
  [Console]::Out.WriteLine("END $t")
  [Console]::Out.Flush()
}
