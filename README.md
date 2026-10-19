# Openviz

**Download:** the Mac `.dmg` and the Windows `.exe` are on the [Releases page](https://github.com/YoshiBowman/Open-Viz/releases), under Assets of the newest release.

A show-lighting visualizer built on Unreal Engine 5.8. A lighting console sends DMX (Art-Net or sACN) and Openviz shows the rig as it would look: beams in haze with gobos, prisms, framing and shadows, LED walls and video screens, set pieces.

- Reads rigs as **MVR** (from grandMA3, Vectorworks, Capture and others) and fixture types as **GDTF**, with the fixtures' own 3D models, wheels, gobos and prisms. Finds and downloads fixtures from GDTF Share.
- Patch lists, fixture types swapped in for a console's placeholders, set pieces (decks, truss, drapes, model files), screens showing pictures, films, NDI streams or DMX pixels.
- Light plots and paperwork as PDF, MVR export, MVR-xchange with the console, OSC remote control, DMX recording and playback, video recording and rendering.

The same folder runs on a Mac and on Windows: the visualizer is built on the computer it runs on.

## Installing: the easy way

Download the installer for your computer from the newest release on [Releases](https://github.com/YoshiBowman/Open-Viz/releases). Openviz also needs Unreal Engine 5.8, installed from the [Epic Games Launcher](https://store.epicgames.com/download) with your Epic account.

- **Windows: `Openviz-<version>-Windows.exe`.** Run it. Windows may say it protected your PC from an unrecognised app: More info, Run anyway. Choose the folder (`C:\Openviz`), Install, and leave "Set Openviz up now" ticked: a window checks for Python, Visual Studio and Unreal Engine (and offers to install the first two and the Epic Games Launcher), builds Openviz and puts it in the Start menu. "Set up Openviz" in the Start menu does that again, for example once Unreal is installed. Openviz is removed in Settings, Apps, Installed apps; your shows are kept unless you say otherwise.
- **Mac: `Openviz-<version>-Mac.dmg`.** Open it and drag Openviz to Applications, then open Openviz. The first time, macOS may say it cannot check the app: System Settings, Privacy & Security, Open Anyway. Openviz needs Xcode (step 1 below), as Unreal does. It keeps its shows and its own files in `~/Openviz`; a newer Openviz from a newer .dmg updates them and keeps your shows.

The first time a show is watched, Openviz builds the visualizer: ten minutes to half an hour.

The steps below do the same by hand.

## On a Mac

1. Install [Xcode](https://developer.apple.com/download/all) (Apple ID needed) into `/Applications`, then finish its setup:

```bash
sudo xcodebuild -license
```

```bash
sudo xcodebuild -runFirstLaunch
```

```bash
xcodebuild -downloadComponent MetalToolchain
```

2. Install [Homebrew](https://brew.sh), then Git LFS and CMake:

```bash
brew install git-lfs cmake
```

```bash
git lfs install
```

3. Install the [Epic Games Launcher](https://store.epicgames.com/download) and, in it, Unreal Engine 5.8.
4. Get Openviz, build its core and put Openviz in Applications:

```bash
git clone https://github.com/YoshiBowman/Open-Viz.git ~/Openviz
```

```bash
cd ~/Openviz && cmake -S core -B core/build && cmake --build core/build -j
```

```bash
sh ~/Openviz/launcher/build_mac_app.sh
```

5. Open **Openviz** from Applications. The first time a show is watched, Openviz builds the visualizer (ten minutes to half an hour; the Engine page has "Build now" to do it beforehand).

## On Windows

1. Install, each with its own installer:
   - the [Epic Games Launcher](https://store.epicgames.com/download) and, in it, Unreal Engine 5.8;
   - [Visual Studio](https://visualstudio.microsoft.com) (Community is enough) with the workload **Game development with C++**, in the version Epic lists for Unreal 5.8 ([Setting up Visual Studio](https://dev.epicgames.com/documentation/en-us/unreal-engine/setting-up-visual-studio-development-environment-for-cplusplus-projects-in-unreal-engine));
   - [Git for Windows](https://git-scm.com) (it includes Git LFS);
   - [Python](https://www.python.org/downloads) 3.9 or newer, with "Add python.exe to PATH" ticked.
2. In a Command Prompt, in the folder that is to hold Openviz:

```bat
git clone https://github.com/YoshiBowman/Open-Viz.git Openviz
```

3. Double-click `launcher\setup_windows.bat` in the new folder. It fetches the Unreal material files, builds the core and the visualizer (ten minutes to half an hour the first time), puts **Openviz** in the Start menu and on the desktop, and ends with a system check that writes `windows-check.txt`. At the end it lists what is done and what is not; run it again after putting something right.
4. Open **Openviz** from the Start menu. It opens in a window of its own. The first time Openviz and Unreal use the network, Windows asks whether to allow them: allow both on private networks.

For videos to be made into MP4 files on Windows, install FFmpeg (`winget install ffmpeg`); without it they are kept as recorded, and VLC plays them.

Openviz has been used on a Mac; on Windows it has not yet been run on a real PC, so `windows-check.txt` and, if the visualizer does not build, `library\logs\build.log` are what to look at first.

## Updating

Run the installer again. Or, for a copy made with Git: in the Openviz folder, `git pull`, then open Openviz again; it rebuilds the visualizer by itself when its code has changed. On Windows, run `launcher\setup_windows.bat` again as well.

## Using it

Drop an MVR file on the Openviz window to add a show (grandMA3: Menu › Patch › Export, as MVR; Vectorworks: File › Export › Export MVR), then **Watch Live** to see it under the console's DMX. Open a show for its own page: edit the patch and fixture positions in the **Patch** tab, replace a console's placeholder fixture types in **Fixture Types**, build the stage in **Set Pieces**, set up **Screens**, and export the light plot and paperwork from **Paperwork**. **Open in Unreal Editor** moves fixtures with the mouse. In the visualizer, hold the right mouse button to look around, W A S D to fly, and the number keys for stored views. Openviz keeps shows, fixtures and settings in `library/` inside its folder.

## Licence

None has been chosen yet; until one is, all rights are reserved. The code under `core/third_party/` keeps its own licence (pugixml and miniz, both MIT). Unreal Engine is Epic Games' and comes under Epic's own licence.
