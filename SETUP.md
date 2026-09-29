# Explorer — Setup Guide

Getting Explorer running from a fresh clone.

## Prerequisites

- **Windows 10/11**, ~150 GB free disk, 16 GB+ RAM, a GPU that supports DX12/SM6 (for Lumen + Nanite)
- **Unreal Engine 5.4.4** — install via Epic Games Launcher (Engine tab → Library → Unreal Engine 5.4). Default install
  location is fine; if you install somewhere other than `C:\Users\<you>\UnrealEngine\UE_5.4`, adjust the paths below.
  Make sure **Starter Content** is checked in the installer options (or install the "Editor Symbols for debugging"
  variant that includes Samples) — `Tools/setup_content.ps1` copies it out of the engine install.
- **Visual Studio 2022 Build Tools** with the "Desktop development with C++" workload (full VS is fine too, but only
  the command-line build tools are required/tested)
- **Python 3** on PATH (used by `Tools/fetch_assets.py`, which runs outside the engine)
- A gamepad is recommended (primary input) but keyboard/mouse works as a fallback

## 1. Clone and generate project files

```
git clone https://github.com/jakez-gh/explorer.git
cd explorer
```

Right-click `Explorer.uproject` → **Generate Visual Studio project files** (or just proceed to step 3 — the
command-line build doesn't need the `.sln`).

## 2. Fetch and import free CC0 assets

These are not committed (large binaries), so pull them after cloning:

```powershell
# Engine Starter Content (photographed textures, SM_Rock, SM_Bush) — ~600 MB, copied from your engine install
Tools/setup_content.ps1

# Poly Haven + ambientCG textures/models (CC0) — downloads into SourceAssets/
python Tools/fetch_assets.py

# Import SourceAssets/ into the project as .uassets (Nanite enabled), headless
C:\Users\<you>\UnrealEngine\UE_5.4\Engine\Binaries\Win64\UnrealEditor-Cmd.exe <repo>\Explorer.uproject -run=pythonscript -script=<repo>\Tools\import_assets.py -unattended

# Regenerate terrain/prop materials from the imported textures, headless
C:\Users\<you>\UnrealEngine\UE_5.4\Engine\Binaries\Win64\UnrealEditor-Cmd.exe <repo>\Explorer.uproject -run=pythonscript -script=<repo>\Tools\create_materials.py -unattended -nullrhi
```

### Optional: photoreal trees (Fab/Megascans)

Temperate forests look for `Content/EuropeanBeech` and `Content/NorwayMaple` (Quixel Megascans, free) at runtime and
fall back to generated trees if they're missing. To add them:

1. Open the project in the Unreal Editor
2. **Window → Fab** → search "European Beech" and "Norway Maple" → **Add to Project** for each
3. They land in `Content/EuropeanBeech`, `Content/NorwayMaple`, `Content/MSPresets` — already git-ignored

This step is skippable; the game runs fine without it, just with simpler tree meshes.

## 3. Build

From a shell with VS 2022 Build Tools on PATH:

```
C:\Users\<you>\UnrealEngine\UE_5.4\Engine\Build\BatchFiles\Build.bat ExplorerEditor Win64 Development -Project=<repo>\Explorer.uproject -WaitMutex
```

(Or open `Explorer.sln` in Visual Studio and Build Solution, Development Editor config — slower to set up but works
the same.)

## 4. Run

```
C:\Users\<you>\UnrealEngine\UE_5.4\Engine\Binaries\Win64\UnrealEditor.exe <repo>\Explorer.uproject -game -windowed -ResX=1600 -ResY=900
```

The map, game mode, and input are all set in `Config/`, so this just works — no manual level setup, no placing
actors, no picking a game mode. First launch after any shader/asset change spends several minutes compiling shaders
and building Nanite data (cached after that).

Useful flags:
- `-NewGame` — ignore the save and start fresh (otherwise it resumes wherever you last quit)
- `-StartX=<u> -StartY=<u>` (world units) — start at a specific location; refine with `-StartZ=`, `-StartYaw=`, `-StartPitch=`
- `-BiomeReport` — logs the nearest biome/village/city/floating island and exits
- `-FlightDebug` — logs fps/altitude/velocity once a second

Or open `Explorer.uproject` in the Unreal Editor and press Play to iterate.

## Controls

- **Gamepad (primary):** left stick steer (turn + bank, pitch) · right trigger speed (released = hover) ·
  right stick look · bumpers rise/sink · Start = pause menu
- **Keyboard/mouse:** WASD steer · Space cruise / Shift full speed · mouse look · E/Q rise/sink · Esc = pause menu ·
  Alt+Q quit

## Troubleshooting

- **Linker error on a locked DLL** — close the running game and `CrashReportClient.exe` before rebuilding.
- **UBT fails resolving SwarmInterface/NETFXSDK** — set a user env var `UE_SDKS_ROOT` pointing at a folder containing
  a placeholder NETFXSDK dir (see `CLAUDE.md`); the real .NET Framework SDK isn't required, this just satisfies UBT's
  lookup.
- **Headers fail to compile on newer MSVC** — already worked around in `Source/Explorer/Explorer.Build.cs`
  (`PLATFORM_HAS_ASAN_INCLUDE=0`, PCHs disabled). If you hit something similar, that file is the place to look.
- **Missing Starter Content error from `setup_content.ps1`** — reinstall/modify the engine in Epic Games Launcher and
  make sure Starter Content/Samples is included.
- **Forests look plain / low-poly trees everywhere** — expected without the optional Fab packs (see above); not a bug.

See `CLAUDE.md` for the full architecture and development context.
