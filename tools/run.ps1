# Boots the port, saves window captures at the given times (seconds since launch) and the log, then closes it.
# usage: tools\run.ps1 [-At 10,20,30] [-Name run] [-Extra @('--some_cvar=1')]
param([int[]]$At = @(30), [string]$Name = 'run', [string[]]$Extra = @(), [double]$Scale = 0.5)

$bin = 'N:\Projects\Monopoly\port\out\build\win-amd64-release'
$docs = 'N:\Projects\Monopoly\docs'
$log = "$docs\$Name.log"
Remove-Item $log, "$docs\$Name-*.png" -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force N:\Projects\Monopoly\userdata | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class Win { [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  public struct RECT { public int L, T, R, B; } }
"@ -ErrorAction SilentlyContinue

$args_ = @('--game_data_root="N:\ROMS\Xbox 360\Monopoly"', '--user_data_root=N:\Projects\Monopoly\userdata',
           '--gpu_plugin=xenos', "--log_file=$log") + $Extra
$p = Start-Process "$bin\monopoly.exe" -ArgumentList $args_ -WorkingDirectory $bin -PassThru
$start = Get-Date
foreach ($t in ($At | Sort-Object)) {
  $wait = $t - ((Get-Date) - $start).TotalSeconds
  if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]($wait * 1000)) }
  $p.Refresh()
  if ($p.HasExited) { "exited early with code $($p.ExitCode)"; return }
  $r = New-Object Win+RECT
  [Win]::GetClientRect($p.MainWindowHandle, [ref]$r) | Out-Null
  if ($r.R -le 0) { continue }
  $bmp = New-Object System.Drawing.Bitmap $r.R, $r.B
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $dc = $g.GetHdc()
  [Win]::PrintWindow($p.MainWindowHandle, $dc, 3) | Out-Null  # client area, full render content
  $g.ReleaseHdc($dc)
  $small = New-Object System.Drawing.Bitmap $bmp, ([int]($r.R * $Scale)), ([int]($r.B * $Scale))
  $small.Save("$docs\$Name-$t.png")
  "captured $docs\$Name-$t.png"
}
"alive: cpu=$([int]$p.CPU)s mem=$([int]($p.WS / 1MB))MB"
Stop-Process $p -Force
