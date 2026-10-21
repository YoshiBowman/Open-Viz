"""What differs between the systems Openviz runs on, in one place. Standard library only.

Openviz is developed on a Mac. The Windows halves of these functions are written from the
documentation and could not be run here: `tools/windows_check.py` tries each of them on a Windows
computer and says which hold.
"""

import os
import re
import shutil
import socket
import subprocess
import sys

WINDOWS = sys.platform == "win32"
MAC = sys.platform == "darwin"


ENGINE_VERSION = "5.8"


def default_engine():
    """Where Unreal Engine 5.8 is installed unless told otherwise.

    On Windows the Epic Games Launcher may have put it anywhere: it says where in the registry and
    in its own list of installs, and the usual folder is the last guess."""
    if os.environ.get("UE_ROOT"):
        return os.environ["UE_ROOT"]
    if WINDOWS:
        for found in (_engine_from_registry(), _engine_from_launcher_list()):
            if found and os.path.isdir(found):
                return found
        return os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"), "Epic Games", "UE_" + ENGINE_VERSION)
    return "/Users/Shared/Epic Games/UE_5.8" if MAC else os.path.expanduser("~/UnrealEngine")


def _engine_from_registry():
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, "SOFTWARE\\EpicGames\\Unreal Engine\\" + ENGINE_VERSION) as key:
            return winreg.QueryValueEx(key, "InstalledDirectory")[0]
    except (ImportError, OSError):
        return ""


def _engine_from_launcher_list():
    path = os.path.join(os.environ.get("ProgramData", r"C:\ProgramData"), "Epic", "UnrealEngineLauncher", "LauncherInstalled.dat")
    try:
        with open(path, encoding="utf-8-sig") as file:
            return engine_in_launcher_list(file.read())
    except (OSError, UnicodeDecodeError):
        return ""


def engine_in_launcher_list(text, version=ENGINE_VERSION):
    """The folder of an engine version in the Epic Games Launcher's list of installs
    (LauncherInstalled.dat, JSON), or "" if it lists none."""
    import json
    try:
        listed = json.loads(text).get("InstallationList", [])
    except (ValueError, AttributeError):
        return ""
    for entry in listed if isinstance(listed, list) else []:
        if isinstance(entry, dict) and entry.get("AppName") == "UE_" + version and entry.get("InstallLocation"):
            return str(entry["InstallLocation"])
    return ""


def editor_binary(engine, platform=None):
    platform = platform or sys.platform
    if platform == "darwin":
        return os.path.join(engine, "Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor")
    if platform == "win32":
        return os.path.join(engine, "Engine", "Binaries", "Win64", "UnrealEditor.exe")
    return os.path.join(engine, "Engine/Binaries/Linux/UnrealEditor")


def build_command(engine, project, platform=None):
    """The command that builds the Unreal project, as a line to paste into a terminal."""
    platform = platform or sys.platform
    if platform == "win32":
        return f'"{engine}\\Engine\\Build\\BatchFiles\\Build.bat" LightingVizEditor Win64 Development -Project="{project}"'
    system = "Mac" if platform == "darwin" else "Linux"
    return f'"{engine}/Engine/Build/BatchFiles/{system}/Build.sh" LightingVizEditor {system} Development -Project="{project}"'


def build_arguments(engine, project, platform=None):
    """build_command as the arguments for starting it."""
    platform = platform or sys.platform
    if platform == "win32":
        return [os.path.join(engine, "Engine", "Build", "BatchFiles", "Build.bat"), "LightingVizEditor", "Win64", "Development", f"-Project={project}"]
    system = "Mac" if platform == "darwin" else "Linux"
    return [f"{engine}/Engine/Build/BatchFiles/{system}/Build.sh", "LightingVizEditor", system, "Development", f"-Project={project}"]


def visual_studio():
    """The folder of the newest Visual Studio with the C++ tools, as its own installer says, or ""."""
    where = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"), "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if not WINDOWS or not os.path.isfile(where):
        return ""
    try:
        done = subprocess.run([where, "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                               "-property", "installationPath"], capture_output=True, text=True, errors="replace", timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        return ""
    lines = done.stdout.strip().splitlines()
    return lines[0].strip() if done.returncode == 0 and lines else ""


def cmake_program(studio=None):
    """CMake: the one on the PATH, else on Windows the one Visual Studio brings along. "" if neither."""
    found = shutil.which("cmake")
    if found or not WINDOWS:
        return found or ""
    studio = visual_studio() if studio is None else studio
    bundled = os.path.join(studio, "Common7", "IDE", "CommonExtensions", "Microsoft", "CMake", "CMake", "bin", "cmake.exe") if studio else ""
    return bundled if bundled and os.path.isfile(bundled) else ""


def project_open(project):
    """Whether any Unreal on this computer has the project open, whoever started it: its built
    files are in use then, and building over them would go wrong."""
    try:
        if WINDOWS:
            script = "Get-CimInstance Win32_Process -Filter \"Name like 'UnrealEditor%'\" | ForEach-Object { $_.CommandLine }"
            listed = subprocess.run(["powershell", "-NoProfile", "-Command", script], capture_output=True, text=True, errors="replace", timeout=20).stdout
        else:
            listed = subprocess.run(["ps", "-ax", "-o", "command="], capture_output=True, text=True, errors="replace", timeout=20).stdout
    except (OSError, subprocess.TimeoutExpired):
        return False
    wanted = os.path.normcase(os.path.abspath(project))
    return any("UnrealEditor" in line and wanted in os.path.normcase(line) for line in listed.splitlines())


def built_tool(folder, name):
    """A program the core's build made: `folder/name`, or on Windows `name.exe` in the folder or
    in the Release or Debug folder Visual Studio puts it in. The first that is there; else the
    plain path, for saying what is missing."""
    plain = os.path.join(folder, name)
    for candidate in (plain, plain + ".exe", os.path.join(folder, "Release", name + ".exe"), os.path.join(folder, "RelWithDebInfo", name + ".exe"),
                      os.path.join(folder, "Debug", name + ".exe")):
        if os.path.isfile(candidate):
            return candidate
    return plain


def process_alive(pid):
    """Whether a process with this id is running.

    Not `os.kill(pid, 0)` everywhere: on Windows that call does not ask, it ends the process."""
    if not isinstance(pid, int) or pid <= 0:
        return False
    if WINDOWS:
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        handle = kernel.OpenProcess(0x1000, False, pid)    # PROCESS_QUERY_LIMITED_INFORMATION
        if not handle:
            return ctypes.get_last_error() == 5            # access denied: it is there, and somebody else's
        try:
            code = wintypes.DWORD()
            return bool(kernel.GetExitCodeProcess(handle, ctypes.byref(code))) and code.value == 259    # STILL_ACTIVE
        finally:
            kernel.CloseHandle(handle)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    except OSError:
        return False
    return True


def local_addresses():
    """This computer's IPv4 addresses, loopback first."""
    found = ["127.0.0.1"]
    if not WINDOWS:
        try:
            out = subprocess.run(["ifconfig"], capture_output=True, text=True, timeout=5).stdout
            for address in re.findall(r"\binet (?:addr:)?(\d+\.\d+\.\d+\.\d+)", out):
                if address not in found:
                    found.append(address)
        except (OSError, subprocess.TimeoutExpired):
            pass
    if len(found) == 1:    # Windows, or a system without ifconfig: what the computer's own name resolves to
        try:
            for entry in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
                address = entry[4][0]
                if address not in found:
                    found.append(address)
        except OSError:
            pass
    return found


def windows_command_line(arguments):
    """Arguments as the one line Windows starts a program with, written so that Unreal reads them.

    Python's own quoting puts an argument with a space in it wholly in quotes: "-LvShow=C:\\My
    Shows\\rig.mvr". Unreal looks for the name and reads the value after the "=" up to the next
    space unless the value itself opens with a quote, so it would take C:\\My. Here the quotes go
    round the value: -LvShow="C:\\My Shows\\rig.mvr"."""
    parts = []
    for argument in arguments:
        argument = str(argument)
        found = re.match(r"(-[A-Za-z][A-Za-z0-9_]*=)(.*)$", argument, re.S)
        if found and (" " in found.group(2) or "\t" in found.group(2)) and '"' not in found.group(2):
            value = found.group(2)
            # A backslash before the closing quote would swallow it.
            parts.append(f'{found.group(1)}"{value}{chr(92) if value.endswith(chr(92)) else ""}"')
        else:
            parts.append(subprocess.list2cmdline([argument]))
    return " ".join(parts)


def start(arguments, **options):
    """Starts a program on its own, not ended with this one's terminal. On Windows the arguments are joined as Unreal needs them."""
    if WINDOWS:
        return subprocess.Popen(windows_command_line(arguments), creationflags=getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0), **options)
    return subprocess.Popen(arguments, start_new_session=True, **options)


def region():
    """(the country this computer is set to, "" if it does not say; whether it measures in inches, None if it leaves that to the country)."""
    country, inches = region_of(os.environ.get("LC_ALL") or os.environ.get("LANG") or ""), None
    if MAC:
        def setting(key):
            try:
                done = subprocess.run(["defaults", "read", "-g", key], capture_output=True, text=True, timeout=10)
                return done.stdout.strip() if done.returncode == 0 else ""
            except (OSError, subprocess.TimeoutExpired):
                return ""
        country = region_of(setting("AppleLocale")) or country
        units = setting("AppleMeasurementUnits")    # only there when set apart from the region
        inches = True if units == "Inches" else False if units == "Centimeters" else None
    elif WINDOWS:
        try:
            import ctypes
            kernel = ctypes.WinDLL("kernel32")
            name = ctypes.create_unicode_buffer(85)
            if kernel.GetUserDefaultLocaleName(name, 85):
                country = region_of(name.value) or country
            measure = ctypes.create_unicode_buffer(4)
            if kernel.GetLocaleInfoEx(None, 0x000D, measure, 4):    # LOCALE_IMEASURE: "0" metric, "1" U.S.
                inches = measure.value == "1"
        except (OSError, AttributeError):
            pass
    return country, inches


def region_of(locale_name):
    """The country in a locale's name: "US" from "en_US", "en-US" or "en_US.UTF-8@rg=..."; "" if it names none."""
    found = re.match(r"[A-Za-z]{2,3}(?:[_-][A-Za-z]{4})?[_-]([A-Za-z]{2})(?![A-Za-z])", str(locale_name).strip())
    return found.group(1).upper() if found else ""


def choose_file_command(kind):
    """The command that has the user pick a picture or a film in the system's own file window and
    prints its path, or None where there is no such window to ask."""
    if MAC:
        kinds = '{"public.image"}' if kind == "picture" else '{"glb", "obj", "3ds"}' if kind == "model" else '{"public.audio"}' if kind == "sound" else '{"public.movie"}'
        prompt = "Choose a model for the show" if kind == "model" else "Choose the sound for the video" if kind == "sound" else f"Choose a {kind} for the screen"
        return ["osascript", "-e", f'POSIX path of (choose file with prompt "{prompt}" of type {kinds})']
    if WINDOWS:
        endings = ("*.png;*.jpg;*.jpeg;*.bmp;*.tga" if kind == "picture" else "*.glb;*.obj;*.3ds" if kind == "model" else
                   "*.wav;*.mp3;*.m4a;*.aif;*.aiff;*.flac" if kind == "sound" else "*.mp4;*.mov;*.m4v;*.avi;*.wmv")
        title = "Choose a model for the show" if kind == "model" else "Choose the sound for the video" if kind == "sound" else f"Choose a {kind} for the screen"
        script = ("Add-Type -AssemblyName System.Windows.Forms; $d = New-Object System.Windows.Forms.OpenFileDialog; "
                  f"$d.Title = '{title}'; $d.Filter = '{kind.capitalize()}s|{endings}|All files|*.*'; "
                  "if ($d.ShowDialog() -eq 'OK') { [Console]::Out.Write($d.FileName) } else { exit 3 }")
        return ["powershell", "-NoProfile", "-STA", "-Command", script]
    return None


def sound_converter(source, target, ffmpeg=None):
    """The command that makes a sound file (MP3, AAC, AIFF, FLAC, WAV...) a WAV of 16-bit samples at
    48 kHz, as a rendered video's sound is put in, or None where the system has nothing for it (then
    only WAV files can be used). macOS has afconvert; elsewhere FFmpeg, if it is installed."""
    if MAC and os.path.exists("/usr/bin/afconvert"):
        return ["/usr/bin/afconvert", "-f", "WAVE", "-d", "LEI16@48000", source, target]
    ffmpeg = ffmpeg or shutil.which("ffmpeg")
    if ffmpeg:
        return [ffmpeg, "-y", "-loglevel", "error", "-i", source, "-ac", "2", "-ar", "48000", "-c:a", "pcm_s16le", target]
    return None


def jpeg_converter(source, target):
    """The command that makes a JPEG of a PNG, or None where the system has nothing for it (then the
    PNG is kept). macOS has sips."""
    if MAC and os.path.exists("/usr/bin/sips"):
        return ["/usr/bin/sips", "-s", "format", "jpeg", "-s", "formatOptions", "92", source, "--out", target]
    return None


def video_converter(source, target, ffmpeg=None):
    """The command that makes an H.264 movie of a movie the visualizer recorded (JPEG pictures, which
    are large), or None where the system has nothing for it: then the movie is kept as it was made.
    macOS has avconvert, on AVFoundation. Windows has nothing of its own for it that a script can
    call, so there it is FFmpeg, if it is installed (winget install ffmpeg)."""
    if MAC and os.path.exists("/usr/bin/avconvert"):
        return ["/usr/bin/avconvert", "--source", source, "--preset", "PresetHighestQuality", "--output", target, "--replace"]
    ffmpeg = ffmpeg or shutil.which("ffmpeg")
    if ffmpeg:
        # Even sizes and 4:2:0, which every player takes; quality about that of avconvert's highest.
        return [ffmpeg, "-y", "-loglevel", "error", "-i", source, "-vf", "scale=trunc(iw/2)*2:trunc(ih/2)*2", "-c:v", "libx264",
                "-preset", "slow", "-crf", "16", "-pix_fmt", "yuv420p", "-movflags", "+faststart", target]
    return None


def open_path(path):
    """Opens a file or a folder in whatever this computer shows it with."""
    if WINDOWS:
        os.startfile(path)    # noqa: S606
    else:
        subprocess.Popen(["open" if MAC else "xdg-open", path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def app_window_command(url, profile):
    """The command that shows the page in a window of its own, without the browser's bar and tabs,
    or None where there is no such way: then the page opens in the browser.

    On Windows that is Microsoft Edge, which every Windows 10 and 11 has, as an app window. With a
    profile folder of its own the Edge it starts is a process of its own too, one that ends when
    the window is closed, so Openviz can end with it (as the Mac app does)."""
    if not WINDOWS:
        return None
    for root in (os.environ.get("ProgramFiles(x86)"), os.environ.get("ProgramFiles"), os.environ.get("LOCALAPPDATA")):
        edge = os.path.join(root, "Microsoft", "Edge", "Application", "msedge.exe") if root else ""
        if edge and os.path.isfile(edge):
            return [edge, f"--app={url}", f"--user-data-dir={profile}", "--no-first-run", "--no-default-browser-check",
                    "--disable-background-mode",
                    "--disable-features=Translate", "--window-size=1440,920"]
    return None


def file_manager():
    """What this system calls the program that shows folders."""
    return "Finder" if MAC else "File Explorer" if WINDOWS else "the file manager"


class WindowsCredentials:
    """One password in Windows' Credential Manager, as Keychain in launcher.py keeps one in the
    macOS Keychain: the GDTF Share login, when "Stay logged in" is on. It is kept for this Windows
    user on this computer, where Windows keeps the user's other saved passwords, never in a file of
    Openviz's. Through the system's own functions (advapi32), as UTF-8."""

    def __init__(self, service="Openviz.GDTF-Share", account="gdtf-share"):
        self.service = service
        self.account = account

    @staticmethod
    def available():
        return WINDOWS

    @staticmethod
    def _api():
        import ctypes
        from ctypes import wintypes

        class Credential(ctypes.Structure):
            _fields_ = [("Flags", wintypes.DWORD), ("Type", wintypes.DWORD), ("TargetName", wintypes.LPWSTR),
                        ("Comment", wintypes.LPWSTR), ("LastWritten", wintypes.FILETIME), ("CredentialBlobSize", wintypes.DWORD),
                        ("CredentialBlob", ctypes.POINTER(ctypes.c_ubyte)), ("Persist", wintypes.DWORD), ("AttributeCount", wintypes.DWORD),
                        ("Attributes", ctypes.c_void_p), ("TargetAlias", wintypes.LPWSTR), ("UserName", wintypes.LPWSTR)]

        api = ctypes.WinDLL("advapi32", use_last_error=True)
        api.CredWriteW.argtypes = [ctypes.POINTER(Credential), wintypes.DWORD]
        api.CredWriteW.restype = wintypes.BOOL
        api.CredReadW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.POINTER(ctypes.POINTER(Credential))]
        api.CredReadW.restype = wintypes.BOOL
        api.CredDeleteW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD]
        api.CredDeleteW.restype = wintypes.BOOL
        api.CredFree.argtypes = [ctypes.c_void_p]
        api.CredFree.restype = None
        return ctypes, api, Credential

    GENERIC = 1             # CRED_TYPE_GENERIC
    LOCAL_MACHINE = 2       # CRED_PERSIST_LOCAL_MACHINE: kept for this user on this computer, not roamed

    def save(self, password):
        try:
            ctypes, api, Credential = self._api()
            secret = password.encode("utf-8")
            blob = (ctypes.c_ubyte * max(1, len(secret))).from_buffer_copy(secret or b"\0")
            credential = Credential(Type=self.GENERIC, TargetName=self.service, Comment="Openviz: GDTF Share login",
                                    CredentialBlobSize=len(secret), CredentialBlob=blob, Persist=self.LOCAL_MACHINE, UserName=self.account)
            if not api.CredWriteW(ctypes.byref(credential), 0):
                return False
        except (OSError, AttributeError, ValueError):
            return False
        return self.load() == password

    def load(self):
        try:
            ctypes, api, Credential = self._api()
            found = ctypes.POINTER(Credential)()
            if not api.CredReadW(self.service, self.GENERIC, 0, ctypes.byref(found)):
                return None
            try:
                size = found.contents.CredentialBlobSize
                return ctypes.string_at(found.contents.CredentialBlob, size).decode("utf-8") if size else ""
            finally:
                api.CredFree(found)
        except (OSError, AttributeError, ValueError):
            return None

    def forget(self):
        try:
            _, api, _ = self._api()
            api.CredDeleteW(self.service, self.GENERIC, 0)
        except (OSError, AttributeError):
            pass


def hide_console_windows():
    """Windows only, and only for an Openviz started without a console of its own (from its
    shortcut): the programs it runs are mostly console programs (the core's inspector, PowerShell,
    the engine's build), and each would open a black window of its own every time. From here on
    they run without one. Their output is read as before; windows of their own, such as Unreal's
    or a file dialog, still show."""
    if not WINDOWS or getattr(subprocess.Popen, "_openviz_quiet", False):
        return
    quiet = getattr(subprocess, "CREATE_NO_WINDOW", 0x08000000)
    original = subprocess.Popen.__init__

    def init(self, *arguments, **options):
        options["creationflags"] = options.get("creationflags", 0) | quiet
        original(self, *arguments, **options)

    subprocess.Popen.__init__ = init
    subprocess.Popen._openviz_quiet = True
