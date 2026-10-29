# Assembles a runnable folder: tools\package.ps1 [-Out N:\Projects\Monopoly\dist\Monopoly]
# Game data is never included; the player copies their own disc files into game\.
param([string]$Out = 'N:\Projects\Monopoly\dist\Monopoly')

$bin = 'N:\Projects\Monopoly\port\out\build\win-amd64-release'
$net = 'N:\Projects\Monopoly\build\monopoly_net.exe'
foreach ($f in "$bin\monopoly.exe", $net) { if (!(Test-Path $f)) { throw "missing $f - build first" } }

Remove-Item $Out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$Out\game" | Out-Null
Copy-Item "$bin\monopoly.exe", "$bin\*.dll", $net $Out
Copy-Item "$bin\shader_cache" $Out -Recurse

@'
Monopoly (Xbox 360) - PC port

SETUP
  Copy the files from your own Monopoly (Xbox 360) disc into the "game" folder:
  default.xex, data.big, movies.big, movies2.big, music.big.

PLAY
  monopoly.exe                      start in fullscreen (--fullscreen=false for a window)

ONLINE (up to 4 players)
  The host runs the game; everyone else runs the small monopoly_net.exe client.
  Host:     monopoly.exe --net_host [--net_port=27360] [--net_name=Alice]
            Forward TCP port 27360 on your router for players outside your network.
  Players:  monopoly_net.exe --join <host address>[:port] --name Bob
            Use a controller (or arrows/Enter/Backspace/Space/Esc on the keyboard).
  In the game, choose "Play With: Multiple" controllers and the number of human players;
  each online player controls their own player. Players can join or rejoin at any time.
'@ | Set-Content "$Out\README.txt" -Encoding utf8

$size = (Get-ChildItem $Out -Recurse -File | Measure-Object Length -Sum).Sum
"packaged $Out ($([int]($size / 1MB)) MB)"
