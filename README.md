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
- **Epic Games Launcher** — [Download here](https://www.epicgames.com/store/download)
- **Unreal Engine 5.4** — Install via Epic Games Launcher
- **Visual Studio 2022** (Windows) — [Download Community Edition (free)](https://visualstudio.microsoft.com/vs/community/)
- **Git** — [Download here](https://git-scm.com/download/win)

### Installation Steps

#### 1. Install Epic Games Launcher
- Download from https://www.epicgames.com/store/download
- Run the installer and sign in with your Epic Games account

#### 2. Install Unreal Engine 5.4
- Open Epic Games Launcher
- Go to **Unreal Engine** tab → **Library**
- Click **Install** for Unreal Engine 5.4
- Choose your installation location (default is fine)
- Wait for install to complete (~100GB)

#### 3. Install Visual Studio 2022 (if needed)
- Download Community Edition: https://visualstudio.microsoft.com/vs/community/
- Run installer
- Select **Desktop development with C++** workload
- Complete installation

#### 4. Generate and Build Project
1. Navigate to the `Explorer` folder
2. Right-click `Explorer.uproject` → **Generate Visual Studio project files**
3. Open `Explorer.sln` in Visual Studio
4. Build → **Build Solution** (or press Ctrl+Shift+B)
5. Wait for compilation (~5-15 minutes first time)

#### 5. Open in Unreal Editor
- Double-click `Explorer.uproject`
- Unreal Editor will open and load the project
- First load may take a few minutes

### First Run
1. In the Unreal Editor, create a new level or open the default level
2. Drag a **SimpleTerrainActor** into the level (search in Place Actors panel)
3. Set the World Settings → Game Mode → **ExplorerGameMode**
4. Plug in your controller
5. Press Play (Alt+P) and fly around!

**Controls:**
- **Left Stick** — Move forward/back/strafe
- **Right Stick** — Look around
- **Triggers** — Ascend/Descend
- **ESC** — Return to editor

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
