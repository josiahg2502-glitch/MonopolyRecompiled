# Assembles a runnable folder: tools\package.ps1 [-Out N:\Projects\Monopoly\dist\Monopoly]
# Game data is never included; the player copies their own disc files into game\.
param([string]$Out = 'N:\Projects\Monopoly\dist\Monopoly')

$bin = 'N:\Projects\Monopoly\port\out\build\win-amd64-release'
$net = 'N:\Projects\Monopoly\build\monopoly_net.exe'
$launcher = 'N:\Projects\Monopoly\build\MonopolyLauncher.exe'
foreach ($f in "$bin\monopoly.exe", $net, $launcher) { if (!(Test-Path $f)) { throw "missing $f - build first" } }

# Replace what we ship; keep the player's game files, settings and logs.
if (Test-Path $Out) { Get-ChildItem $Out -Exclude game, monopoly.toml, launcher.toml, logs | Remove-Item -Recurse -Force }
New-Item -ItemType Directory -Force "$Out\game" | Out-Null
Copy-Item "$bin\monopoly.exe", "$bin\*.dll", $net, $launcher $Out
Copy-Item "$bin\shader_cache" $Out -Recurse
New-Item -ItemType Directory -Force "$Out\server" | Out-Null
Copy-Item 'N:\Projects\Monopoly\server\lobby_directory.py' "$Out\server"

@'
Monopoly (Xbox 360) - PC port

SETUP
  Copy the files from your own Monopoly (Xbox 360) disc into the "game" folder:
  default.xex, data.big, movies.big, movies2.big, music.big.

PLAY
  Start MonopolyLauncher.exe. From there you can play on this PC, host an online game,
  join a friend's game, and change display settings. (The launcher can also point to
  another folder with your disc files.)

ONLINE (up to 4 players)
  The host runs the game; everyone else joins from their own launcher.
  Host:     Host online > Open lobby. Friends gather in the lobby (with chat), mark
            themselves ready, and you press Start game.
  Friends:  Join online, then pick a game from "Public games" or "On your network",
            or type the join code / address the host sent.
  Internet play needs TCP port 27360 open on the host's router. The launcher asks the
  router to open it (UPnP); if your router doesn't allow that, forward it yourself.
  In the game, choose "Play With: Multiple" controllers and the number of human players;
  each online player controls their own player. Players can join or rejoin at any time.
  Keyboard/controller: LB / RB (or Ctrl+PgUp / PgDn) switch launcher pages.

PUBLIC GAMES
  The Public games list comes from a lobby server everyone shares. Run
  server\lobby_directory.py (Python 3, nothing to install) on any always-on machine
  reachable from the internet, for example: python3 lobby_directory.py --port 27380
  Then enter its address (e.g. http://your-server:27380) in the launcher's Settings.

COMMAND LINE (optional)
  monopoly.exe [--fullscreen=false] [--net_host] [--net_port=27360] [--net_name=Alice]
  monopoly_net.exe --join <host address>[:port] --name Bob
  Settings saved by the launcher are in monopoly.toml (game) and launcher.toml.
'@ | Set-Content "$Out\README.txt" -Encoding utf8

$size = (Get-ChildItem $Out -Recurse -File | ? { $_.FullName -notlike "$Out\game\*" } | Measure-Object Length -Sum).Sum
"packaged $Out ($([int]($size / 1MB)) MB)"
