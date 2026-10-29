# Sends a sequence of presses to a running live session: tools\nav.ps1 "A:4 DOWN:1 RIGHT:1 A:5" [-Shot name]
# Each token is BUTTON[:seconds to wait after it] (default 2 s).
param([Parameter(Mandatory)][string]$Seq, [string]$Shot = '')
$L = 'N:\Projects\Monopoly\tools\live.ps1'
foreach ($tok in $Seq -split '\s+') {
  if (!$tok) { continue }
  $parts = $tok -split ':'
  & $L press $parts[0] | Out-Null
  Start-Sleep -Milliseconds ([int](1000 * $(if ($parts.Count -gt 1) { [double]$parts[1] } else { 2 })))
}
if ($Shot) { & $L shot $Shot }
