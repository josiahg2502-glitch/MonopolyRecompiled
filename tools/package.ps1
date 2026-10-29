# Assembles a runnable folder: tools\package.ps1 [-Out N:\Projects\Monopoly\dist\Monopoly] [-Zip]
# Game data is never included; the player installs it from their own disc image in the launcher.
# -Zip also writes dist\MonopolyRecompiled.zip for a release (without game files, settings or logs).
param([string]$Out = 'N:\Projects\Monopoly\dist\Monopoly', [switch]$Zip)

$root = 'N:\Projects\Monopoly'
$bin = "$root\port\out\build\win-amd64-release"
$net = "$root\build\monopoly_net.exe"
$launcher = "$root\build\MonopolyLauncher.exe"
foreach ($f in "$bin\monopoly.exe", $net, $launcher) { if (!(Test-Path $f)) { throw "missing $f - build first" } }

# Replace what we ship; keep the player's game files, settings and logs.
if (Test-Path $Out) { Get-ChildItem $Out -Exclude game, monopoly.toml, launcher.toml, logs | Remove-Item -Recurse -Force }
New-Item -ItemType Directory -Force "$Out\game", "$Out\server" | Out-Null
Copy-Item "$bin\monopoly.exe", "$bin\*.dll", $net, $launcher, "$root\LICENSE", "$root\THIRD_PARTY_NOTICES.txt" $Out
Copy-Item "$bin\shader_cache" $Out -Recurse
Copy-Item "$root\server\lobby_directory.py" "$Out\server"

@'
Monopoly Recompiled - Monopoly (Xbox 360) for PC
https://github.com/josiahg2502-glitch/MonopolyRecompiled

SETUP
  Open MonopolyLauncher.exe and press "Install from disc image..." to pick your own
  Monopoly (Xbox 360, USA/Europe) .iso. The game files (about 1.9 GB) are copied into the
  "game" folder once. Or choose a folder that already has default.xex and the .big files.

PLAY
  MonopolyLauncher.exe: play on this PC, host or join an online game, change settings.
  Plug in a controller per player and choose "Play With: Multiple" in the game.
  With a controller, LB / RB (or Ctrl+PgUp / PgDn on a keyboard) switch launcher pages.

ONLINE (up to 4 players)
  The host runs the game; everyone else joins from their own launcher (no game files needed).
  Host:     Host online > Open lobby. Friends gather in the lobby (with chat), mark
            themselves ready, and you press Start game.
  Friends:  Join online, then pick a game from "Public games" or "On your network",
            or type the join code / address the host sent.
  Internet play needs TCP port 27360 open on the host's router. The launcher asks the
  router to open it (UPnP); if your router doesn't allow that, forward it yourself.

PUBLIC GAMES
  The Public games list comes from a lobby server everyone shares. Run
  server\lobby_directory.py (Python 3, nothing to install) on any always-on machine
  reachable from the internet, for example: python3 lobby_directory.py --port 27380
  Then enter its address (e.g. http://your-server:27380) in the launcher's Settings.

COMMAND LINE (optional)
  monopoly.exe [--fullscreen=false] [--net_host] [--net_port=27360] [--net_name=Alice]
  monopoly_net.exe --join <host address>[:port] --name Bob
  Settings saved by the launcher are in monopoly.toml (game) and launcher.toml.

LEGAL
  This port's code is GPL-3.0 (LICENSE); bundled libraries are listed in
  THIRD_PARTY_NOTICES.txt. A fan project, not affiliated with Hasbro or Electronic Arts.
  MONOPOLY is a trademark of Hasbro. No game data is included.
'@ | Set-Content "$Out\README.txt" -Encoding utf8

$size = (Get-ChildItem $Out -Recurse -File | ? { $_.FullName -notlike "$Out\game\*" } | Measure-Object Length -Sum).Sum
"packaged $Out ($([int]($size / 1MB)) MB)"

if ($Zip) {
  $stage = "$root\dist\stage\MonopolyRecompiled"
  Remove-Item "$root\dist\stage" -Recurse -Force -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force $stage | Out-Null
  Get-ChildItem $Out -Exclude game, logs, monopoly.toml, launcher.toml | Copy-Item -Destination $stage -Recurse
  $zipPath = "$root\dist\MonopolyRecompiled.zip"
  Remove-Item $zipPath -ErrorAction SilentlyContinue
  Compress-Archive -Path $stage -DestinationPath $zipPath -CompressionLevel Optimal
  Remove-Item "$root\dist\stage" -Recurse -Force
  "zipped $zipPath ($([int]((Get-Item $zipPath).Length / 1MB)) MB)"
}
