# Explorer

A first-person flying exploration game in Unreal Engine 5.4: photorealistic, controller-first, with the floaty
freedom of flying in a dream, through an infinite procedurally generated Earth-like world. Quitting and relaunching
resumes where you left off.

## Status

Builds and runs (Lumen, ~75-100 fps). Photographic terrain/water materials, scanned rocks and foliage; biomes,
volcanoes, villages, cities, lighthouses, stone circles, floating islands; save/resume. Verified with
keyboard/mouse; gamepad is the primary intended input.

## Setup

See **[SETUP.md](SETUP.md)** for step-by-step instructions to go from a fresh clone to a running build, including
fetching the (not committed) free CC0 textures/models and optional Fab/Megascans tree packs.

## Controls

- **Gamepad (primary):** left stick steer (turn + bank, pitch) · right trigger speed (released = hover) ·
  right stick look · bumpers rise/sink · Start = pause menu
- **Keyboard/mouse (fallback):** WASD steer · Space cruise / Shift full speed · mouse look · E/Q rise/sink ·
  Esc = pause menu · Alt+Q quit

## Architecture

See **[CLAUDE.md](CLAUDE.md)** for the full architecture, technical direction, known decisions, and next steps.

- `Procedural/WorldGen` — pure functions of world XY: height, climate, biome, terrain layers, tree density
- `Procedural/TerrainStreamer` — chunk streaming, LOD, pooled instanced meshes for trees/rocks/landmarks
- `Flight/FlightPawn` — controller-first flight (bank, pitch, throttle, look), saves progress
- `Game/ExplorerGameMode` / `ExplorerPlayerController` — resume-from-save, pause menu

## Contributing

Solo project taking outside contributions. Open a PR or reach out directly.

---

*Built with Unreal Engine 5.4*
