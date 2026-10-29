# Repeats tools/find_missing_funcs.py (merging into config/monopoly.toml) + codegen until nothing new turns up.
$port = 'N:\Projects\Monopoly\port'
$image = 'C:\Users\josia\AppData\Local\Temp\claude\C--Users-josia--claude-spares-spare-8bbi8D\532973a4-802e-4c95-a5b2-e6509f8f8fe5\scratchpad\image.bin'
for ($pass = 1; $pass -le 8; $pass++) {
  python -I N:\Projects\Monopoly\tools\find_missing_funcs.py $image "$port\generated\default\monopoly_init.cpp" "$port\config\monopoly.toml"
  $changed = $LASTEXITCODE -eq 2
  Push-Location $port
  cmd /c "N:\Projects\Monopoly\tools\env.cmd rexglue codegen monopoly_manifest.toml" > N:\Projects\Monopoly\docs\codegen.log 2>&1
  Pop-Location
  Select-String -Path N:\Projects\Monopoly\docs\codegen.log -Pattern 'Unresolved|Failed|error|Overlapping' | % { "  $($_.Line)" }
  if (!$changed) { "pass ${pass}: converged"; break }
}
