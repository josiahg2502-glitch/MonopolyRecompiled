# Builds the precompiled shader cache that ships with the port (port/assets/shader_cache).
#
# Starts the game with its cache pointed at the assets folder; every shader and pipeline the game
# uses while you play is translated and appended there. Play through the menus, a board game
# (buy, trade, auction, jail), minigames and the Richest edition, then close the game and rebuild:
# the build copies the cache next to the exe and the app loads all of it at startup.
#
# usage: tools\warm_shader_cache.ps1 [-Extra @('--input_script=...')]
param([string[]]$Extra = @())

$bin = 'N:\Projects\Monopoly\port\out\build\win-amd64-release'
$cache = 'N:\Projects\Monopoly\port\assets\shader_cache'
$before = (Get-ChildItem $cache -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum

$gameArgs = @('--game_data_root="N:\ROMS\Xbox 360\Monopoly"', '--fullscreen=false', "--cache_root=$cache") + $Extra
Start-Process "$bin\monopoly.exe" -WorkingDirectory $bin -ArgumentList $gameArgs -Wait

$after = (Get-ChildItem $cache -Recurse -File | Measure-Object Length -Sum).Sum
"shader cache: $before -> $after bytes. Rebuild to ship it."
