# Monopoly (Xbox 360) PC port

Static recompilation of Monopoly (USA, Europe), 2008, EA, title ID 4541086F, built on the ReXGlue SDK, with online play for up to 4 players.

## Status (2026-10-29)

- **Playable.** Tested: Monopoly Edition (Classic board, 4 players, dice, buying, auctions, AI turns), Richest Edition (Developer mode, World board), the Go to Jail minigame, saves, and the VP6 FMVs. It runs at 60 fps. See `docs/milestone-board.png`.
- **Online works end to end.** The game hosts with `--net_host`. A remote player using `monopoly_net.exe` (or `net_probe`) sees the game at about 28 fps with audio and controls their own player. In a test, a remote P2 set up their player, rolled, won the order roll, and started an auction over the network. See `docs/milestone-online.png`.
- **Shaders are precompiled:** 55 shaders and 67 pipelines ship in `shader_cache/`, and all of them are created at startup.
- Distributable folder: `tools\package.ps1` writes `dist\Monopoly` (43 MB, no game data). The player copies their disc files into `game\`.

## Layout

| Path | What |
|---|---|
| `N:\ROMS\Xbox 360\Monopoly\` | game files (default.xex + *.big). The source zip is in Downloads. |
| `rexglue-sdk/` | ReXGlue v0.10.0 plus our fixes (`patches/rexglue-codegen-fixes.patch`). Installed to `rexglue-sdk/out/install/win-amd64`. `out/build` (409 MB) is kept for incremental SDK rebuilds. |
| `port/` | the game: manifest, `config/monopoly.toml`, `src/` (app, netplay bridge, script input), `assets/shader_cache` (shipped storage), `generated/` (codegen output) |
| `src/netplay/`, `tests/` | online transport (Host/Client, H.264 via Media Foundation), the self-test, the `net_probe` headless remote player, and the `monopoly_net` join client (top-level CMake project, `build/`) |
| `tools/` | `env.cmd` (VS 2026 Build Tools + LLVM 23), `xiso.py`, `big.py`, `find_missing_funcs.py`/`fix_missing_funcs.ps1`, `run.ps1`, `live.ps1` + `nav.ps1` (drive a running game), `merge_shader_cache.py`, `warm_shader_cache.ps1`, `package.ps1` |

## Build and run

Fresh checkout (the SDK, `port/generated`, build outputs and game files aren't in git):

```
git clone --recursive https://github.com/rexglue/rexglue-sdk && cd rexglue-sdk
git checkout c94f5ebdcb3c9d1a460ca48e04f9758448f8d518 && git submodule update --init --recursive
git apply ..\patches\rexglue-codegen-fixes.patch
..\tools\env.cmd cmake --preset win-amd64
..\tools\env.cmd cmake --build out/build/win-amd64 --config Release --target install
```

Then:

```
cd port
..\tools\env.cmd rexglue codegen monopoly_manifest.toml      # only after config or SDK changes
..\tools\env.cmd cmake --preset win-amd64-release             # once
..\tools\env.cmd cmake --build --preset win-amd64-release
cd ..
tools\env.cmd cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang-cl
tools\env.cmd ninja -C build                                  # monopoly_net, net_probe, netplay_selftest
tools\package.ps1
```

Run from source: `port\out\build\win-amd64-release\monopoly.exe --game_data_root="N:\ROMS\Xbox 360\Monopoly" --fullscreen=false [--net_host]`.

Unattended testing: `tools\live.ps1 start [-Extra ...]`, then `tools\nav.ps1 "A:4 DOWN:1 RIGHT:1 A:5" -Shot name` (button:seconds; LS_LEFT etc. for the stick), then `tools\live.ps1 stop` (closes gracefully, which flushes the shader storage).

## Fixes and findings

- **Codegen config** (`port/config/monopoly.toml`): two `this`-adjust thunks, one wide-char helper missing from `.pdata`, and about 90 vtable/callback targets that codegen had folded into neighbouring functions ("call to unregistered function" crashes). `tools/fix_missing_funcs.ps1` iterates until it converges.
- **ReXGlue codegen bugs fixed** (in `patches/`, with tests in `rexglue-sdk/tests/ppc/asm`):
  - `vpkuwus`/`vpkuhus` broke when vD aliased vA/vB, which corrupted VP6 motion compensation (green smear FMVs).
  - `sync` now emits a seq_cst fence.
  - The PPC instruction suite passes: 1458 cases plus the new in-place cases.
- **Submodule symlinks:** `core.symlinks=false` checks the `libmspack/cabextract/mspack` symlinks out as text. Replace them with copies of their targets after a fresh clone.
- **Menu gotcha:** on board/player screens, confirming takes DOWN (and RIGHT) to focus Next, then A. The highlight is subtle; the earlier "hangs" were this. Back is the on-screen button, not B.
- **Shader storage** is written through buffered stdio. A force-killed game loses the tail, so close the window normally.

## Decisions

- **ReXGlue** as the runtime (Xenia kernel/GPU/XMA/VFS), with the D3D12 backend.
- **Online = host streaming.** The game has no Live multiplayer, and lockstep needs determinism we don't have.
  - Transport is TCP port 27360. Video is H.264 at 30 fps / 4 Mbit/s through the MF software MFTs. Audio is a stereo fold of the 5.1 guest mix, sent as PCM.
  - Remote slot N is guest controller N (`NetplayAssignment`). Local pads fill user 0 first, then the free users.
  - A dropped player's slot stays plugged in as an idle pad, so the game doesn't pause, and a rejoin gets the lowest free slot back.
  - Players pick "Play With: Multiple" and the number of humans.
- **Shader precompile = ship the storage observed during play.** Xenia's storage needs the pipeline state that goes with each shader, so raw ucode extracted from the XEX can't be pretranslated. To extend coverage, play new content, close normally, then run `tools\merge_shader_cache.py port\assets\shader_cache <user cache>` and rebuild.

## Next steps

1. Widen shader coverage: other boards and minigames unlock with play. Industrialist/Tycoon modes and the Options and Credits screens aren't covered yet.
2. Netplay polish:
   - an in-game host/join overlay instead of CLI flags
   - AAC audio, since PCM is about 1.5 Mbit/s per player
   - UDP or a relay for players who can't port-forward
   - a persistent readback ring in `FrameCapture` (`CaptureGuestOutput` allocates per frame)
3. Remember window and fullscreen settings, and default the user data folder to %LOCALAPPDATA%\monopoly (the SDK default) instead of the dev folder.
4. Port to Vulkan for Linux and Steam Deck if wanted (ReXGlue supports it).
