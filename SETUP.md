# Explorer — Setup Guide

This guide walks through getting Explorer running on your machine.

## System Requirements

- **OS**: Windows 10/11
- **Disk Space**: ~150GB (UE 5.4 + Visual Studio + project)
- **RAM**: 16GB minimum (32GB recommended)
- **GPU**: NVIDIA GTX 1060 or better (for decent performance)
- **Internet**: High-speed for downloading ~100GB of tools

## Step 1: Install Epic Games Launcher

1. Go to https://www.epicgames.com/store/download
2. Click **Download** for Epic Games Launcher
3. Run `EpicInstaller.exe`
4. Follow the installer prompts
5. Sign in with your Epic Games account (create one if needed)

**Time:** ~10 minutes

## Step 2: Install Unreal Engine 5.4

1. Open **Epic Games Launcher**
2. Click **Unreal Engine** in the left sidebar
3. Click **Library**
4. Scroll to find **Unreal Engine 5.4** (or click **+** to add it)
5. Click **Install**
6. Choose your installation location (default: `C:\Program Files\Epic Games\UE_5.4`)
7. Click **Install**
8. Wait for download and installation (~20-30 minutes depending on internet)

**Total Download:** ~100GB

## Step 3: Install Visual Studio 2022

Visual Studio is required to compile C++ code.

1. Go to https://visualstudio.microsoft.com/vs/community/
2. Click **Download Visual Studio Community**
3. Run the installer
4. Click **Continue** past the initial screen
5. On the **Workloads** tab, check:
   - ✓ **Desktop development with C++**
6. Click **Install**
7. Wait for installation (~30 minutes)

**Why?** Unreal uses Visual Studio to compile your game code.

## Step 4: Generate Project Files

1. Open **Windows File Explorer**
2. Navigate to your `Explorer` folder
3. Right-click `Explorer.uproject`
4. Select **Generate Visual Studio project files**
5. Wait a few seconds for `Explorer.sln` to be created

## Step 5: Build the Project

1. Open `Explorer.sln` in Visual Studio
2. At the top, make sure you're in **Development Editor** mode (dropdown near the Run button)
3. Press **Ctrl+Shift+B** or go to **Build → Build Solution**
4. Wait for compilation (5-15 minutes first time, faster after)
5. When done, you should see "Build succeeded" in the output

## Step 6: Open in Unreal Editor

### Option A: From Windows Explorer
1. Right-click `Explorer.uproject`
2. Select **Open with → Unreal Engine 5.4**

### Option B: From Unreal Launcher
1. Open **Epic Games Launcher**
2. Go to **Library**
3. Click **Create** next to Unreal Engine 5.4
4. Browse to your `Explorer` folder
5. Select `Explorer.uproject`
6. Click **Open**

**First load will take 5-10 minutes as it compiles shaders.**

## Step 7: Test Flight

1. In Unreal Editor, the default level should load
2. If not, create a new level: **File → New Level**
3. In the **Content Browser** (bottom panel), create a folder called `Maps` if it doesn't exist
4. **Place Actors** panel (right side) → Search for **SimpleTerrainActor**
5. Drag it into the level
6. In the **World Settings** panel (top right), set:
   - **Game Mode Override** → `ExplorerGameMode`
7. Plug in your controller
8. Press **Alt+P** or click the **Play** button
9. You should be able to fly around!

## Troubleshooting

### "Can't find Visual Studio"
- Make sure Visual Studio 2022 is installed with C++ workload
- Restart your computer after installation

### "Project failed to compile"
- Delete the `Intermediate`, `Binaries`, and `Saved` folders
- Right-click `Explorer.uproject` → **Generate Visual Studio project files**
- Rebuild in Visual Studio

### "Unreal Editor won't launch"
- Make sure UE 5.4 is fully installed (check Epic Games Launcher)
- Delete the `.vs` folder in the Explorer directory
- Regenerate Visual Studio files and rebuild

### "Flight feels weird or doesn't respond"
- Make sure your controller is plugged in and detected by Windows
- In Unreal Editor, go to **Edit → Project Settings → Input**
- Look for gamepad mappings

### "Can't place terrain"
- Make sure you're in a valid level (not the default)
- The **Place Actors** panel is on the right side of the editor
- Search for "SimpleTerrainActor" in the search box

## Next Steps

Once it's running:
- Explore the terrain with your controller
- Adjust flight parameters in the `FlightPawn` Blueprint
- Experiment with terrain settings in `SimpleTerrainActor`
- Check out the CLAUDE.md file for development context

## Getting Help

If you get stuck:
1. Check the output log: **Window → Developer Tools → Output Log**
2. Read error messages carefully — they often tell you exactly what's wrong
3. Try the troubleshooting section above
4. Check Unreal's official docs: https://docs.unrealengine.com

---

**You're almost there!** Once UE 5.4 and Visual Studio are installed, the rest is smooth.
