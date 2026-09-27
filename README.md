# Explorer

A first-person flying exploration game built in Unreal Engine 5.4.

## Concept

**Explorer** is about the feeling of flight through wilderness—soaring over natural landscapes, alien terrain, and procedurally generated worlds. There's no combat, no objectives. Just discovery, navigation, and the beauty of motion through space.

### Core Experience
- **First-person perspective** — pilot's view, immersive flight
- **Natural + alien wilderness** — diverse procedural terrain to explore
- **Flying mechanics** — smooth, responsive movement through space
- **Infinite terrain** — procedural generation for endless exploration
- **Minimalist design** — focus on the experience, not the UI

## Project Structure

```
Explorer/
├── Source/
│   └── Explorer/          # C++ source code
├── Content/
│   ├── Maps/              # Level files
│   ├── Blueprints/        # Blueprint classes
│   ├── Materials/         # Shaders and materials
│   ├── Meshes/            # 3D models
│   └── Audio/             # Sound and music
├── Plugins/               # Custom and third-party plugins
├── Documentation/         # Design docs, notes
└── Explorer.uproject      # Unreal project file
```

## Development Setup

### Prerequisites
- **Unreal Engine 5.4** — [Install from Epic Games Launcher](https://www.epicgames.com/unrealengine/download)
- **Visual Studio 2022** (Windows) or **Xcode** (Mac) — for C++ development
- **Git LFS** — for large binary files (recommended)

### Quick Start
1. Clone this repo
2. Right-click `Explorer.uproject` → **Generate Visual Studio project files**
3. Open `Explorer.sln` in Visual Studio
4. Build the project
5. Open `Explorer.uproject` in Unreal Editor

## Design Notes

### Flight Mechanics
- Camera-relative movement (WASD for direction, Space/Ctrl for up/down)
- No landing/walking — pure flight exploration
- Responsive but weighty feel

### Terrain Generation
- Procedural height maps and biomes
- Streaming for infinite terrain
- Varied features: mountains, valleys, forests, alien structures

### Visual Direction
- Minimalist UI (compass, altitude, maybe minimal HUD)
- Atmospheric lighting and weather
- Focus on silhouettes and scale

## Building & Deployment

- **Packaging** — Use Unreal's Packaging tools (File → Package Project)
- **Platforms** — Windows (primary), consider macOS/Linux later
- **Distribution** — Steam, Epic Games Store, or itch.io

## Contributing

This is a solo project for now, but structured for clarity and modularity.

---

*Built with Unreal Engine 5.4*
