"""What a computer needs before Openviz can show a rig, looked at one thing at a time, and what can
be done about each (owner, 2026-10-22: "make sure the dependencies are laid out and either
downloaded automatically or it has a list in system that checks off once everything is done").

`items(system, kind, repo, engine, facts)` gives the list in the order things have to be done.
Each item is a dict:
  key        a short name
  name       what it is, in Title Case
  why        what Openviz needs it for, a plain sentence
  ok         whether it is there
  optional   whether Openviz works without it
  detail     what was found, or what to do next
  action     None, or {"label", "kind"}: "install" starts an installer here, "open" opens a page or
             an app, "admin" asks for an administrator's password in the system's own window,
             "build" builds; `act` does it
  after      the keys that have to be done before its action makes sense
`act(system, kind, key, repo, engine)` starts what an item's action does, without waiting for it.

Nothing here accepts a licence or terms for anyone: Xcode's own window asks for that, and on
Windows winget asks in a console window of its own. Every question to the computer goes through
a `System` (tests give a stand-in), so no test looks at or changes the real one.
Standard library only.
"""

from __future__ import annotations

import glob
import json
import os
import re
import shutil
import subprocess
import sys

XCODE_STORE = "macappstore://apps.apple.com/app/id497799835"
EPIC_DOWNLOAD = "https://store.epicgames.com/download"
VISUAL_STUDIO_DOWNLOAD = "https://visualstudio.microsoft.com/downloads/"
GIT_LFS = "https://git-lfs.com"
ENGINE_VERSION = "5.8"


class System:
    """The computer, as the checks see it: what programs say, and which files are there."""

    def run(self, command, timeout=30):
        """(exit code, what it printed); (None, why) if it could not be run."""
        try:
            done = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=timeout)
        except (OSError, subprocess.TimeoutExpired) as error:
            return None, str(error)
        return done.returncode, (done.stdout + done.stderr).strip()

    def exists(self, path):
        return os.path.exists(path)

    def glob(self, pattern):
        return sorted(glob.glob(pattern))

    def which(self, name):
        return shutil.which(name)

    def read(self, path):
        """A text file's contents, or "" if it cannot be read."""
        try:
            with open(path, encoding="utf-8", errors="replace") as handle:
                return handle.read()
        except OSError:
            return ""

    def start(self, command):
        """Starts something and does not wait for it."""
        return subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=sys.platform != "win32")


def kind_of(platform=None):
    platform = platform or sys.platform
    return "mac" if platform == "darwin" else "windows" if platform == "win32" else "other"


def _item(key, name, why, ok, detail="", action=None, after=(), optional=False):
    return {"key": key, "name": name, "why": why, "ok": bool(ok), "optional": optional, "detail": detail,
            "action": None if ok or not action else {"label": action[0], "kind": action[1]}, "after": list(after)}


# ----------------------------------------------------------------------------------------- Mac

def newest_xcode(system):
    """The Xcode app in /Applications with the highest version in its name (Xcode.app counts as the newest)."""
    found = [path for path in system.glob("/Applications/Xcode*.app") if system.exists(os.path.join(path, "Contents", "Developer"))]
    if "/Applications/Xcode.app" in found:
        return "/Applications/Xcode.app"
    return found[-1] if found else ""


def _mac(system, repo, engine, facts):
    out = []
    code, selected = system.run(["xcode-select", "-p"])
    tools = code == 0 and bool(selected)
    out.append(_item("tools", "Apple Command Line Tools", "Openviz runs on the Python they bring and builds its core with their compiler.",
                     tools, selected if tools else "Apple's installer asks before it downloads them (a few minutes).", ("Install", "install")))

    xcode = newest_xcode(system)
    version = ""
    if xcode:
        code, said = system.run([os.path.join(xcode, "Contents", "Developer", "usr", "bin", "xcodebuild"), "-version"])
        version = said.splitlines()[0] if code == 0 and said else ""
    accepted = xcode_range(system, engine)
    fits = not (xcode and version and accepted) or accepted[0] <= _numbers(version) <= accepted[1]
    detail = f"{version or 'Xcode'} in {xcode}" if xcode else "From the App Store, with your Apple ID; it is large, so give it a while."
    if not fits:
        detail = (f"This is {version}, and Unreal Engine {ENGINE_VERSION} accepts Xcode {_written(accepted[0])} to {_written(accepted[1])}: "
                  "older versions are on developer.apple.com/download/all.")
    out.append(_item("xcode", "Xcode", "Unreal Engine builds the visualizer and its shaders with it.", xcode and fits, detail, ("Get Xcode", "open")))

    in_xcode = tools and ".app/Contents/Developer" in selected
    out.append(_item("selected", "Xcode Selected", "Builds use whichever developer tools are selected, and the Command Line Tools alone cannot build Unreal projects.",
                     in_xcode, selected if in_xcode else f"Selected now: {selected or 'none'}. Selecting Xcode asks for an administrator's password.",
                     ("Select Xcode", "admin"), after=["xcode"]))

    ready = False
    if in_xcode:
        licence, _ = system.run(["xcodebuild", "-license", "check"])
        launched, _ = system.run(["xcodebuild", "-checkFirstLaunchStatus"])
        ready = licence == 0 and launched == 0
    out.append(_item("first_launch", "Xcode Set Up", "Xcode has to be opened once: it asks you to agree to its licence and installs what builds need.",
                     ready, "Done." if ready else "Open Xcode, agree to its licence and let it install its components, then close it.",
                     ("Open Xcode", "open"), after=["selected"]))

    metal = False
    if in_xcode:
        code, _ = system.run(["xcrun", "-sdk", "macosx", "-f", "metal"])
        metal = code == 0
    out.append(_item("metal", "Metal Toolchain", "The visualizer's shaders are compiled with it; newer versions of Xcode download it separately.",
                     metal, "Installed." if metal else "A download of a few hundred megabytes, which Xcode fetches from Apple.",
                     ("Download", "install"), after=["first_launch"]))
    return out


def _numbers(text):
    parts = [int(part) for part in re.findall(r"\d+", text or "")][:3]
    return tuple(parts + [0] * (3 - len(parts)))


def _written(numbers):
    return ".".join(str(part) for part in (numbers[:2] if numbers[2] == 0 else numbers))


def xcode_range(system, engine):
    """The Xcode versions the engine accepts, as it says in Engine/Config/Apple/Apple_SDK.json: (lowest, highest), or None."""
    text = system.read(os.path.join(engine or "", "Engine", "Config", "Apple", "Apple_SDK.json")) if engine else ""
    try:
        said = json.loads(text) if text else {}
    except ValueError:
        return None
    if not isinstance(said, dict) or not said.get("MinVersion") or not said.get("MaxVersion"):
        return None
    return _numbers(said["MinVersion"]), _numbers(said["MaxVersion"])


# ------------------------------------------------------------------------------------- Windows

def _windows(system, repo, engine, facts):
    out = []
    out.append(_item("python", "Python 3.9 or Newer", "Openviz itself runs on it.", sys.version_info >= (3, 9), f"Python {sys.version.split()[0]}"))
    studio = facts.get("visual_studio", "")
    winget = system.which("winget")
    out.append(_item("visual_studio", "Visual Studio 2022 with C++", "Unreal Engine builds the visualizer with it, and it builds Openviz's core.",
                     studio, studio or "With the workloads Game Development with C++ and Desktop Development with C++. "
                     + ("Installing opens a window where Microsoft's installer asks you to agree to its terms." if winget else "From Microsoft's site."),
                     ("Install", "install") if winget else ("Get Visual Studio", "open")))
    return out


# -------------------------------------------------------------------------------- both of them

def _common(system, kind, repo, engine, facts, before):
    out = []
    editor = os.path.join(engine or "", "Engine", "Binaries", *(("Mac", "UnrealEditor.app", "Contents", "MacOS", "UnrealEditor") if kind == "mac"
                                                                 else ("Win64", "UnrealEditor.exe")))
    has_engine = bool(engine) and system.exists(editor)
    epic = facts.get("epic_launcher", "")
    out.append(_item("engine", f"Unreal Engine {ENGINE_VERSION}", "The visualizer runs in it.", has_engine,
                     engine if has_engine else ("In the Epic Games Launcher: Unreal Engine, Library, the + beside Engine Versions, "
                                                f"{ENGINE_VERSION}, Install. It needs an Epic Games account." if epic else
                                                "It comes through the Epic Games Launcher, with an Epic Games account."),
                     ("Open Epic Games Launcher", "open") if epic else ("Get Epic Games Launcher", "open")))
    core = facts.get("core", "")
    out.append(_item("core", "Openviz Core", "Openviz reads shows and fixtures through it.", core,
                     core or "Built here in a few seconds.", ("Build", "build"), after=["tools"] if kind == "mac" else ["visual_studio"]))
    built, stale = facts.get("built", False), facts.get("stale", [False, ""])
    current = built and not stale[0]
    tools_first = ["first_launch", "metal"] if kind == "mac" else ["visual_studio"]
    out.append(_item("visualizer", "Visualizer", "Built once on this computer from Openviz's own sources.", current,
                     "Built and up to date." if current else (f"Its code has changed since it was built ({stale[1]})." if built
                                                             else "Not built yet: ten minutes to half an hour, once everything above is done."),
                     ("Build Now", "build"), after=tools_first + ["engine"]))
    if facts.get("git"):
        code, said = system.run(["git", "lfs", "version"])
        out.append(_item("git_lfs", "Git LFS", "A copy of Openviz fetched with Git needs it for Unreal's material files.", code == 0,
                         said.splitlines()[0] if code == 0 and said else "", ("Get Git LFS", "open")))
    if kind == "windows":
        ffmpeg = system.which("ffmpeg")
        out.append(_item("ffmpeg", "FFmpeg", "Videos are saved as MP4 files with it; without it they stay QuickTime movies.", ffmpeg,
                         ffmpeg or "", ("Install", "install") if system.which("winget") else None, optional=True))
    return before + out


def items(system, kind, repo, engine, facts):
    """Everything to check, in the order it has to be done. `facts` comes from the launcher:
    core (the inspector's path, or ""), built, stale ([bool, why]), git (a Git checkout),
    epic_launcher (its path, or ""), visual_studio (its folder, or "")."""
    if kind == "mac":
        before = _mac(system, repo, engine, facts)
    elif kind == "windows":
        before = _windows(system, repo, engine, facts)
    else:
        before = []
    return _common(system, kind, repo, engine, facts, before)


def summary(listed):
    """How far along: done, of how many that count, and the names still to do."""
    needed = [item for item in listed if not item["optional"]]
    left = [item["name"] for item in needed if not item["ok"]]
    return {"done": len(needed) - len(left), "total": len(needed), "left": left}


def command_for(kind, key, engine, facts, system):
    """What an item's action runs, or None for the ones the launcher does itself (builds)."""
    if kind == "mac":
        xcode = newest_xcode(system)
        if key == "tools":
            return ["xcode-select", "--install"]
        if key == "xcode":
            return ["open", XCODE_STORE]
        if key == "selected" and xcode:
            developer = os.path.join(xcode, "Contents", "Developer").replace("'", "")
            # The system's own window asks for an administrator's password; Openviz never sees it.
            return ["osascript", "-e", f"do shell script \"/usr/bin/xcode-select -s '{developer}'\" with administrator privileges"]
        if key == "first_launch" and xcode:
            return ["open", "-a", xcode]
        if key == "metal":
            return ["xcodebuild", "-downloadComponent", "MetalToolchain"]
        if key == "engine":
            return ["open", "-a", facts["epic_launcher"]] if facts.get("epic_launcher") else ["open", EPIC_DOWNLOAD]
        if key == "git_lfs":
            return ["open", GIT_LFS]
    if kind == "windows":
        # Installs run in a console window of their own, where winget asks you to agree to the terms.
        console = ["cmd", "/c", "start", "Openviz setup", "cmd", "/k"]
        if key == "visual_studio":
            if not system.which("winget"):
                return ["cmd", "/c", "start", "", VISUAL_STUDIO_DOWNLOAD]
            return console + ["winget", "install", "--id", "Microsoft.VisualStudio.2022.Community", "--override",
                              "--add Microsoft.VisualStudio.Workload.NativeGame --add Microsoft.VisualStudio.Workload.NativeDesktop --includeRecommended --passive"]
        if key == "engine":
            if facts.get("epic_launcher"):
                return ["cmd", "/c", "start", "", facts["epic_launcher"]]
            return console + ["winget", "install", "--id", "EpicGames.EpicGamesLauncher"] if system.which("winget") else ["cmd", "/c", "start", "", EPIC_DOWNLOAD]
        if key == "ffmpeg":
            return console + ["winget", "install", "--id", "Gyan.FFmpeg"]
        if key == "git_lfs":
            return ["cmd", "/c", "start", "", GIT_LFS]
    return None


def epic_launcher(kind, system):
    """Where the Epic Games Launcher is, or ""."""
    if kind == "mac":
        path = "/Applications/Epic Games Launcher.app"
        return path if system.exists(path) else ""
    if kind == "windows":
        for root in (os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"), os.environ.get("ProgramFiles", r"C:\Program Files")):
            path = os.path.join(root, "Epic Games", "Launcher", "Portal", "Binaries", "Win64", "EpicGamesLauncher.exe")
            if system.exists(path):
                return path
    return ""
