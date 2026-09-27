# Explorer — Development Context

## Project Overview
**Explorer** is a first-person flying exploration game in Unreal Engine 5.4. The core feeling is flight through wilderness—natural and alien terrain, procedurally generated, minimalist UI, pure discovery.

**Status:** Ready to build and test. Engine/tools installation needed.

## Game Pillars
1. **Flight** — Smooth, responsive first-person flying (not walking)
2. **Exploration** — Procedurally generated wilderness to discover
3. **Immersion** — Minimal UI, atmospheric, focus on the experience
4. **Scale** — Feeling small in a vast world

## Technical Direction
- **Engine:** Unreal Engine 5.4 (latest stable)
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

## Next Steps (when engine is running)
1. Test flight controls with controller
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
