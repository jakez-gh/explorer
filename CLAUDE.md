# Explorer — Development Context

## Project Overview
**Explorer** is a first-person flying exploration game in Unreal Engine 5.4: **photorealistic**, controller-first, with the floaty freedom of flying in a dream, through an infinite procedurally generated Earth-like world with a huge variety of things to discover the farther you fly. Quitting and relaunching resumes where you left off.

**Status:** Builds and runs (Lumen, ~75-100 fps). Photographic terrain/water materials from Starter Content, scanned rocks and foliage; biomes, volcanoes, villages, cities, lighthouses, stone circles, floating islands; save/resume. Verified with keyboard/mouse; gamepad untested (no controller on the dev machine).

## Game Pillars
1. **Flight** — Floaty, graceful first-person gliding (not walking); bank into turns
2. **Exploration** — The world changes with distance; places to discover if you fly far enough
3. **Immersion** — Minimal UI; photoreal lighting, materials and assets
4. **Scale** — Feeling small in a vast world

## Technical Direction
- **Engine:** Unreal Engine 5.4 (5.4.4), installed at `C:\Users\jake\UnrealEngine\UE_5.4` (not Program Files)
- **Toolchain:** VS 2022 Build Tools only (no IDE, MSVC 14.44) — build from the command line:
  `C:\Users\jake\UnrealEngine\UE_5.4\Engine\Build\BatchFiles\Build.bat ExplorerEditor Win64 Development -Project=<repo>\Explorer.uproject -WaitMutex`
  - Requires user env var `UE_SDKS_ROOT=C:\Users\jake\UnrealEngine\AutoSDK` (placeholder NETFXSDK so UBT resolves SwarmInterface; the real .NET Framework SDK isn't installed and needs admin)
  - `Explorer.Build.cs` defines `PLATFORM_HAS_ASAN_INCLUDE=0` and disables PCHs to work around UE 5.4 headers failing on MSVC 14.40+
  - Close the running game (and `CrashReportClient`) first, or linking fails on the locked DLL
- **Run:** `UnrealEditor.exe <repo>\Explorer.uproject -game -windowed -ResX=1600 -ResY=900` (map, game mode and input are set in `Config/`)
  - `-StartX= -StartY=` (world units) start anywhere; `-StartZ= -StartYaw= -StartPitch=` refine it
  - `-BiomeReport` logs the nearest location of each biome, village, city and floating island
  - `-FlightDebug` logs fps, altitude and velocity once a second
  - `-NewGame` ignores the save and starts fresh; otherwise the last position (saved every 5 s and on quit, `Saved/SaveGames/Explorer.sav`) is restored
- **Assets:** Engine Starter Content (not committed — run `Tools/setup_content.ps1` after cloning) supplies photographed textures, `SM_Rock` and `SM_Bush`. Materials are generated from it by `Tools/create_materials.py` (`UnrealEditor-Cmd.exe <repo>\Explorer.uproject -run=pythonscript -script=<repo>\Tools\create_materials.py -unattended -nullrhi`). Buildings/landmarks are still basic shapes with world-projected photo textures; truly photoreal trees and architecture need scanned assets (Megascans/Fab) — the next big step.
- **Fab packs (free, not committed, ~10 GB):** Quixel Megascans *European Beech* and *Norway Maple* (Content/EuropeanBeech, Content/NorwayMaple, Content/MSPresets). Add via Editor > Window > Fab > "Add to Project". Loaded at runtime; if missing, temperate forests fall back to generated trees. Megaplants (incl. all free conifers) need UE 5.7+; free Megascans rocks are UEFN-only (UE use is paid).
- **Rendering:** Shader Model 6 (DX12) for Nanite; first launch after shader/asset changes spends several minutes compiling shaders and building Nanite data (cached afterwards).
- **Rendering:** Lumen GI + reflections, virtual shadow maps (`Config/DefaultEngine.ini`).

## Architecture
- `Procedural/WorldGen` — pure functions of world XY: height, climate, biome, terrain material layers (sand/rock/snow/forest + dryness/wetness), tree density; continents ~40 km, climate zones ~20-35 km, ranges, dunes/mesas, volcanoes. Sea level is Z=0.
- `Procedural/TerrainStreamer` — disc of 320 m chunks: LOD rings with skirts, collision near, layer weights in vertex colour + UV1 for `M_Terrain`, ocean plane. Pooled HISMs: trees (trunk + scanned-foliage clusters), shrubs, scanned rocks, villages and stone circles near; cities, lighthouses, volcano glow, floating islands everywhere in view. Landmark `Find*` functions are shared with `-BiomeReport`.
- `Flight/FlightPawn` — controller-first: left stick turns (bank follows the stick) and pitches (auto-levels); right trigger sets speed directly (released = hover); right stick aims the view directly (clamped, springs back); bumpers rise/sink; look-ahead ground cushion; saves progress.
- `Game/ExplorerGameMode` — resumes from `UExplorerSaveGame` (or `-StartX`/`-NewGame`), spawns terrain, strips the template sky sphere/floor, sets up natural lighting/fog/post.
- `Game/ExplorerPlayerController` — Escape/Start toggles a Slate pause menu (Resume/Quit); Alt+Q quits.

## Controls
- **Gamepad (primary):** left stick steer (X turn + bank, Y pitch) · right trigger speed (0 = hover … full = very fast) · right stick look · bumpers rise/sink · Start menu
- **Keyboard/mouse (fallback):** WASD steer · Space cruise / Shift full speed · mouse look · E/Q rise/sink · Esc menu · Alt+Q quit

## Known Decisions
- **No combat, no NPCs** (for now) — focus on pure exploration
- **First-person only** — no third-person view
- **Continuous flight** — no landing or walking; the ground gently pushes you away
- **Photoreal** — real/scanned assets and physically based lighting; no stylised colour grading

## Next Steps
1. Test and tune with a gamepad (turn/pitch rates, throttle curve, look ranges)
2. Scanned trees, plants and buildings (Megascans via Fab) to replace foliage clusters and box buildings
3. More discoveries: rivers/waterfalls, canyons, glaciers, ruins, wildlife/birds
4. Time of day, weather, ambient sound

---

*Last updated: 2026-09-27*
