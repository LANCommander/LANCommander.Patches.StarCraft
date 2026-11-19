# StarCraft: Brood War Comprehensive Modern Patch

This project is a set of patches for StarCraft: Brood War 1.16.1 (Blizzard Entertainment, 1998) that makes the
game work well on modern Windows. The patches are applied on startup without making modifications to the original
executable or the game's data files, and the patch contains no Blizzard code or data. The following
fixes/improvements are included:

- **Modern renderer.** The 640x480 exclusive-fullscreen DirectDraw output is replaced by Direct3D 11: borderless
  fullscreen or a window, sharp scaling to any resolution, vsync, and real pixels on scaled displays.
- **Widescreen.** A wider map view during matches (768 or 896 wide, picked from the display's aspect ratio), with
  the HUD centred at the bottom. Menus keep their 640x480 layout.
- **HUD size.** A smaller HUD gives a taller screen with more map visible; set in Options → Controls → HUD Size.
- **Smooth frame pacing.** A 1 ms game clock gives evenly paced simulation frames (an even 42 ms on Fastest instead
  of 31/47 ms), and scrolling runs at the display's refresh rate. Game speed is unchanged.
- **Relocated saves.** Saved games, characters, replays, error logs and screenshots go to
  `%USERPROFILE%\Saved Games\Blizzard Entertainment\StarCraft`. Existing data is copied over on the first start.
- **Grid hotkeys.** An optional QWE / ASD / ZXC layout that follows the 3x3 command card, switched in
  Options → Controls.
- **LAN networking.** Larger UDP socket buffers for the LAN and Battle.net providers, so a burst of turn packets is
  not dropped by Windows. Works with unpatched players.
- **Lobby game rules.** The host of a multiplayer game picks a 24-unit selection limit, rally mining and zooming on
  the Create Game screen. See [Game rules](#game-rules).
- **Match statistics.** The host of a multiplayer game reports it to a [LANCommander](https://lancommander.app)
  server: players, races, eliminations, results, the score screen's totals and the replay. See
  [Match statistics](#match-statistics).

It only supports `StarCraft.exe` 1.16.1 (PE timestamp `0x496589CA`). Any other executable (StarEdit, other
versions) is left alone.

## Install

Download the package from the [releases](https://github.com/LANCommander/LANCommander.Patches.StarCraft/releases)
and extract it into the folder that holds `StarCraft.exe`.

| package | contents | for |
|---|---|---|
| `scpatch-<version>.zip` | `version.dll`, `scpatch.ini`, `scpatch\` (readme, licenses, `Config.example.yml`, `Set-StarCraftCDKey.ps1`) | any install (the game loads `version.dll` from its folder) |

`StarCraft.exe` imports `version.dll`, so Windows loads the patch from the game folder and the patch forwards the
real API to the system's copy. To uninstall, delete `version.dll`.

The log is `scpatch.log` in the save folder (`Saved Games\Blizzard Entertainment\StarCraft`).

**CD key.** `scpatch\Set-StarCraftCDKey.ps1` shows or changes the CD key and owner name stored in `StarDat.mpq`,
in place of the installer's key changer. It keeps the original as `StarDat.mpq.bak`.

```powershell
.\scpatch\Set-StarCraftCDKey.ps1 -StarCraftPath C:\Games\StarCraft -Show
.\scpatch\Set-StarCraftCDKey.ps1 -StarCraftPath C:\Games\StarCraft -CDKey 1234-56789-0123 -Owner "Player"
```

**LANCommander.** The [LANCommander Interposer](https://github.com/LANCommander/LANCommander.Interposer) is built
into `version.dll` (see [LANCommander Interposer](#lancommander-interposer)). If the game also gets LANCommander's
Interposer redistributable, set its load method to **DInput8** or **Injector**: the default **Proxy** method copies
the Interposer over `version.dll` and removes the patch. StarCraft does not load `dinput8.dll`, so with DInput8 the
redistributable only writes `.interposer\Config.yml`, which the built-in Interposer reads.

## Game rules

Anything that changes gameplay is a rule chosen by the host of a multiplayer game, in extra rows under Game Type on
the Create Game screen. A choice is saved like Game Speed and starts there the next time.

- **Selection Limit.** 12 (vanilla) or 24 units per selection and control group. The unit panel shows 12 units at
  a time; Tab (`[Selection] PageKey`) flips to units 13-24.
- **Rally Mining.** Workers trained at a building whose rally point is on a mineral field or on your own refinery
  start harvesting it, instead of walking there and standing idle.
- **Zooming.** Whether players may zoom the map (the zoom itself is still in development and off by default).

The rules travel as lobby messages that unpatched players ignore. The host's choice applies only if every player
has the patch; otherwise the game is vanilla for everyone, and an unpatched player never blocks the start. Joining
players follow the host. Single-player games use `scpatch.ini` (or the last choice). Replays carry the rules they
were recorded with and play back with them.

## Match statistics

When the [LANCommander Interposer's match statistics](https://github.com/LANCommander/LANCommander.Interposer) are
set up, the host of a multiplayer game reports it to the LANCommander server. Joining players report nothing, so
each game is reported once.

- **Start.** The map, free-for-all or team (players in more than one force), the agreed game rules, and each
  player's race and team. Humans are keyed `name:<name>`, computers `bot:<slot>`.
- **Events.** A player leaving, being defeated, winning or drawing, with the game time.
- **End.** Each player's score screen totals (`minerals_mined`, `gas_mined`, `resources_spent`,
  `units_produced`, `units_killed`, `units_lost`, `structures_constructed`, `structures_razed`, `structures_lost`,
  `unit_score`, `structure_score`, `resource_score`, `total_score`) and the placings: winners first; in a
  free-for-all the others by when they were knocked out. Players who left are `forfeited`. If the host quits a 1v1,
  the other player wins, as the game itself decides; if no winner is known, the match is marked abandoned.
- **Replay.** `LastReplay.rep` is attached. A copy is kept for the upload in the save folder's `Matches\` and
  deleted after 14 days.

Where the report goes is the `MatchStatistics` section of `.interposer\Config.yml` (see
`scpatch\Config.example.yml`): with `ServerUrl`, `Token` and `GameId` it is sent straight to that server; without
them, LANCommander's launcher delivers it when it started the game; otherwise it is only logged (`.interposer\Logs`,
with `Logging.Matches: true`). `[Matches]` in `scpatch.ini` turns it off or skips the replay.

## LANCommander Interposer

`version.dll` carries the [LANCommander Interposer](https://github.com/LANCommander/LANCommander.Interposer) and
starts it before any of the patch's own hooks. It reads `.interposer\Config.yml` in the game folder and exports the
Interposer's API (`Interposer*`), so Interposer plugins in `.interposer\Plugins\` find it.

The release is built with the Interposer's match statistics, virtual registry (the CD key and player name values
LANCommander sets), identity (user and computer name) and plugins. A feature left out is off for the whole game,
even if LANCommander configures it, because the patch loads first. `scpatch.log` lists the features at startup
(`interposer: ...`). Other features can be built in, see [Building](#building).

## Settings

The options are in `scpatch.ini` next to `StarCraft.exe`; every one is described in
[`dist/scpatch.ini`](dist/scpatch.ini). Only per-machine settings are there: anything that affects gameplay is a
[game rule](#game-rules). Set `Enabled=0` in a section to turn that fix off. Some choices made in the game's menus
(HUD size, hotkeys, the game rules) are saved in the registry like Game Speed and win over the file.

| section | options | what it does |
|---|---|---|
| `[SavePaths]` | `Enabled`, `MigrateExisting` | Saves, characters, replays, error logs and screenshots in the Saved Games folder; copy the existing ones the first time |
| `[Renderer]` | `Enabled`, `Windowed`, `WindowScale` | Direct3D 11 output; borderless fullscreen or a window, and the window's size as a multiple of 640x480 |
| | `Scaling`, `Filter`, `VSync` | `aspect`, `integer` or `stretch`; `sharp` or `nearest` pixels; vsync |
| `[Network]` | `Enabled`, `SocketBufferKB` | Larger UDP socket buffers (0 = Windows defaults, statistics only) |
| `[Widescreen]` | `Enabled`, `Width`, `HudSize` | Wider map view in matches (0 = from the display, 768, 896, 640 = off); HUD size in percent |
| `[FramePacing]` | `Enabled`, `HighResolutionTimer` | Evenly paced simulation frames with a 1 ms game clock |
| | `SmoothScrolling`, `MaxFps`, `ScrollGranularity` | Scrolling at the display's refresh rate; the redraw cap while scrolling (0 = refresh rate); the scroll pixel grid |
| `[Zoom]` | `Enabled`, `Level`, `Wheel`, `MinLevel`, `WheelSpeed` | In development, off by default: zoom the map in matches, with the mouse wheel |
| `[Selection]` | `Limit`, `PageKey` | Selection limit for single-player games (12 or 24); the key that flips the unit panel |
| `[Rally]` | `Mining`, `ShowFlag` | Rally mining for single-player games; mark the selected buildings' rally points |
| `[Matches]` | `Enabled`, `AttachReplay` | Report multiplayer games you host to LANCommander; attach the replay |
| `[Controls]` | `Hotkeys` | `Default` or `Grid` command card hotkeys |

## Building

The patch builds with CMake and MSVC. It requires:

- Visual Studio 2022 or later with the C++ x86 tools (v143) and a Windows SDK
- CMake 3.25 or later
- [vcpkg](https://vcpkg.io) in `VCPKG_ROOT`, for MinHook and yaml-cpp (`vcpkg.json`), which the Interposer uses
- the Interposer next to this repository (or in `SCPATCH_INTERPOSER_DIR`), with YY-Thunks fetched:

```
git clone https://github.com/LANCommander/LANCommander.Interposer.git
powershell -File LANCommander.Interposer\build\Get-YYThunks.ps1
```

The Interposer and its libraries are compiled into the patch and the CRT is linked statically, so the patch stays a
single DLL (licenses in the package's `scpatch\licenses\`).

```
git clone https://github.com/LANCommander/LANCommander.Patches.StarCraft.git
cd LANCommander.Patches.StarCraft
cmake -S . -B build -A Win32 -T v143
cmake --build build --config Release
```

Output: `build\Release\version.dll`. `-DSCPATCH_VERSION=v1.2.3` sets the version the patch writes to its log
(`dev` otherwise). A build folder configured without vcpkg needs `cmake --fresh` once.

The Interposer features built in are `-DSCPATCH_INTERPOSER_FEATURES="MATCHES;REGISTRY;IDENTITY;PLUGINS"` (the
default), any of `REGISTRY FILES FASTDL DIRECTINPUT AUDIO OSVERSION IDENTITY NETWORK RELAY RICH_PRESENCE MATCHES
VOICE PLUGINS`. `FILES`, `NETWORK` and `RELAY` hook the same file and socket APIs as the save relocation and the
network buffers and are left out until tested together; `VOICE` also needs opus, enet and miniaudio in
`vcpkg.json`.

GitHub Actions (`.github/workflows/build.yml`) builds every push and pull request, with the Interposer's `main` (or
the branch or tag in the repository variable `INTERPOSER_REF`). A semver tag (`v1.2.3`) also drafts a release with
the package.

## Repository layout

| path | contents |
|---|---|
| `src/` | The patch:<br>- `loader/` (the `version.dll` proxy that forwards to the system DLL)<br>- `core/` (exe check and deferred start-up, guarded code patches and import hooks, `scpatch.ini`, paths, log, crash log)<br>- `render/` (the emulated DirectDraw and the Direct3D 11 presenter)<br>- `features/`: `save_paths.cpp`, `renderer.cpp`, `widescreen.cpp` (with `widescreen_sites.inc`), `zoom.cpp`, `frame_pacing.cpp`, `network.cpp`, `selection.cpp` (with `selection_sites.inc`), `rally.cpp`, `controls.cpp`, `rules.cpp` (the lobby game rules and the replay trailer), `matches.cpp` (match statistics) |
| `dist/` | `scpatch.ini` and the Interposer's `Config.yml` example, shipped with the package |
| `Set-StarCraftCDKey.ps1` | The CD key changer for `StarDat.mpq`, shipped with the package |

Every code patch checks the game's original bytes before writing and refuses to apply on a mismatch.

The game files are not included. You need your own copy of the game.

## License

[MIT](LICENSE). StarCraft itself is not included; you need your own copy of the game.
