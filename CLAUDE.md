# Explorer — Development Context

## Project Overview
**Explorer** is a first-person flying exploration game in Unreal Engine 5.4. The core feeling is flight through wilderness—natural and alien terrain, procedurally generated, minimalist UI, pure discovery.

**Status:** Engine installed; project builds and runs. Flight (keyboard/mouse) and terrain verified in-game; gamepad untested.

## Game Pillars
1. **Flight** — Smooth, responsive first-person flying (not walking)
2. **Exploration** — Procedurally generated wilderness to discover
3. **Immersion** — Minimal UI, atmospheric, focus on the experience
4. **Scale** — Feeling small in a vast world

## Technical Direction
- **Engine:** Unreal Engine 5.4 (5.4.4), installed at `C:\Users\jake\UnrealEngine\UE_5.4` (not Program Files)
- **Toolchain:** VS 2022 Build Tools only (no IDE, MSVC 14.44) — build from the command line:
  `C:\Users\jake\UnrealEngine\UE_5.4\Engine\Build\BatchFiles\Build.bat ExplorerEditor Win64 Development -Project=<repo>\Explorer.uproject -WaitMutex`
  - Requires user env var `UE_SDKS_ROOT=C:\Users\jake\UnrealEngine\AutoSDK` (placeholder NETFXSDK so UBT resolves SwarmInterface; the real .NET Framework SDK isn't installed and needs admin)
  - `Explorer.Build.cs` defines `PLATFORM_HAS_ASAN_INCLUDE=0` and disables PCHs to work around UE 5.4 headers failing on MSVC 14.40+
- **Run:** `UnrealEditor.exe <repo>\Explorer.uproject -game -windowed -ResX=1280 -ResY=720` (map, game mode and input are set in `Config/`)
- **Primary Language:** C++ (with Blueprint support for rapid iteration)
- **Terrain:** Procedural generation with streaming for infinite worlds
- **Target Platform:** Windows (primary), expandable to Mac/Linux

## Architecture Goals
- **Modular design:** Separate flight system, terrain generation, procedural biome system
- **Prototype-friendly:** Blueprint + C++ hybrid allows fast iteration on core mechanics
- **Performance-conscious:** Streaming and LOD systems for terrain, efficient physics

## Known Decisions
- **No combat, no NPCs** (for now) — focus on pure exploration
- **First-person only** — no third-person view
- **Continuous flight** — no landing or walking

## Current Status
✅ Unreal project file created (`Explorer.uproject`)
✅ Flight controller implemented (FlightPawn with gamepad support)
✅ Basic procedural terrain generation (SimpleTerrainActor with Perlin noise)
✅ Game mode and spawning system (ExplorerGameMode)
✅ Build configuration ready (ProceduralMeshComponent integrated)

## Next Steps
1. Test flight controls with controller (left stick move, right stick look, triggers up/down)
1. Terrain is a single ~1 km tile — its edge is visible within seconds; needs chunked streaming around the player
2. Polish flight feel (acceleration curves, speed ramping)
3. Add terrain variety (biomes, different noise patterns)
4. Implement camera smoothing
5. Add simple visual polish (materials, lighting)

## Reference Materials
- Unreal Flight Physics & Camera Systems
- Procedural terrain techniques (Perlin noise, heightmaps)
- World Partition system for infinite streaming

---

*Last updated: 2026-09-27*
