# Interactive test session: tools\live.ps1 start | press <buttons> [hold_ms] | shot <name> | stop
# The game reads presses appended to the live script (see port/src/script_input.h).
param([Parameter(Mandatory)][string]$Cmd, [string]$Arg = '', [int]$Hold = 150, [string[]]$Extra = @())

$bin = 'N:\Projects\Monopoly\port\out\build\win-amd64-release'
$docs = 'N:\Projects\Monopoly\docs'
$script = "$docs\live-input.txt"
$pidFile = "$docs\live.pid"

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class LiveWin { [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  public struct RECT { public int L, T, R, B; } }
"@ -ErrorAction SilentlyContinue

switch ($Cmd) {
  'start' {
    Set-Content $script "# live session`n" -NoNewline -Encoding ascii
    $gameArgs = @('--game_data_root="N:\ROMS\Xbox 360\Monopoly"', '--user_data_root=N:\Projects\Monopoly\userdata',
                  '--fullscreen=false', "--input_script=$script", "--log_file=$docs\live.log") + $Extra
    $p = Start-Process "$bin\monopoly.exe" -WorkingDirectory $bin -PassThru -ArgumentList $gameArgs
    $p.Id | Set-Content $pidFile
    "started pid $($p.Id)"
  }
  'press' { Add-Content $script "+ $Arg $Hold" -Encoding ascii; "pressed $Arg" }
  'shot' {
    $p = Get-Process -Id (Get-Content $pidFile) -ErrorAction Stop
    $r = New-Object LiveWin+RECT
    [LiveWin]::GetClientRect($p.MainWindowHandle, [ref]$r) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap $r.R, $r.B
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $dc = $g.GetHdc()
    [LiveWin]::PrintWindow($p.MainWindowHandle, $dc, 3) | Out-Null
    $g.ReleaseHdc($dc)
    $name = if ($Arg) { $Arg } else { 'live' }
    (New-Object System.Drawing.Bitmap $bmp, ([int]($r.R / 2)), ([int]($r.B / 2))).Save("$docs\$name.png")
    "$docs\$name.png"
  }
  'stop' {
    # Close normally first: shader/pipeline storage is buffered and only fully written on exit.
    $p = Get-Process -Id (Get-Content $pidFile) -ErrorAction SilentlyContinue
    if ($p -and $p.CloseMainWindow() -and $p.WaitForExit(15000)) { 'closed' } else { Stop-Process -Id (Get-Content $pidFile) -Force -ErrorAction SilentlyContinue; 'killed' }
    Remove-Item $pidFile
  }
}
