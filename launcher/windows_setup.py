"""Sets Openviz up on a Windows PC, one step after another, and says plainly what is done and what is not.

Started by double-clicking launcher\\setup_windows.bat. What it does:

  1. fetches the material files if they are still Git LFS pointers (git lfs pull);
  2. builds the core with Visual Studio (CMake, as Visual Studio brings it if none is installed) and runs its tests;
  3. builds the visualizer, the Unreal project, with the engine's own build script (the longest step);
  4. puts Openviz in the Start menu and on the desktop, with its icon: it opens in a window of its own;
  5. runs tools\\windows_check.py, which writes windows-check.txt for sending back.

What it needs installed first, and cannot install itself (each asks for your Epic or Microsoft
account, or an administrator's yes): Unreal Engine 5.8 from the Epic Games Launcher, Visual Studio
with the "Game development with C++" workload, Git with Git LFS, and Python. README.md lists
them.

Run it again whenever you like: each step only does what is not done. Standard library only.

    python launcher\\windows_setup.py [--skip-unreal] [--no-shortcuts] [--no-check] [--dry-run]
"""

import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import oslayer  # noqa: E402

PROJECT = os.path.join(REPO, "unreal", "LightingViz.uproject")
CORE = os.path.join(REPO, "core")
CORE_BUILD = os.path.join(CORE, "build")
CONFIG = "RelWithDebInfo"    # as the core builds by default elsewhere: optimised, with what a crash report needs
ICON = os.path.join(HERE, "windows", "openviz.ico")
MATERIAL = os.path.join(REPO, "unreal", "Plugins", "LightingVisualizer", "Content", "Materials", "M_LvBeam.uasset")


class Steps:
    """Says what each step came to, and keeps the list for the end."""

    def __init__(self, dry_run=False):
        self.dry_run = dry_run
        self.done = []       # (step, "ok" | "NOT DONE" | "skipped", what)

    def heading(self, text):
        print(f"\n== {text}", flush=True)

    def result(self, step, outcome, what):
        self.done.append((step, outcome, what))
        print(f"{outcome:8} {what}", flush=True)

    def run(self, command, cwd=REPO, log=None):
        """Runs a command for all to see (or into a log file), and gives its exit code."""
        print("  > " + subprocess.list2cmdline(command), flush=True)
        if self.dry_run:
            return 0
        try:
            if log:
                with open(log, "w", encoding="utf-8", errors="replace") as out:
                    return subprocess.call(command, cwd=cwd, stdout=out, stderr=subprocess.STDOUT)
            return subprocess.call(command, cwd=cwd)
        except OSError as error:
            print(f"  could not be started: {error}", flush=True)
            return None


def is_lfs_pointer(path):
    try:
        with open(path, "rb") as handle:
            return handle.read(40).startswith(b"version https://git-lfs")
    except OSError:
        return False


def materials(steps):
    steps.heading("1. The material files")
    if not os.path.exists(MATERIAL):
        steps.result("materials", "NOT DONE", f"{MATERIAL} is missing: is this the whole project folder?")
        return
    if not is_lfs_pointer(MATERIAL):
        steps.result("materials", "ok", "the material files are here")
        return
    if not os.path.isdir(os.path.join(REPO, ".git")):
        steps.result("materials", "NOT DONE", "the material files are Git LFS pointers and this folder is not a Git checkout: copy the project with Git (git clone) instead")
        return
    code = steps.run(["git", "lfs", "pull"])
    if code == 0 and (steps.dry_run or not is_lfs_pointer(MATERIAL)):
        steps.result("materials", "ok", "the material files were fetched (git lfs pull)")
    else:
        steps.result("materials", "NOT DONE", "git lfs pull did not fetch them: is Git LFS installed (git lfs install)?")


def core(steps):
    steps.heading("2. The core (GDTF and MVR reading)")
    studio = oslayer.visual_studio() if oslayer.WINDOWS else ""
    if oslayer.WINDOWS and not studio:
        steps.result("core", "NOT DONE", "Visual Studio with the C++ tools was not found: install it with the 'Game development with C++' workload")
        return False
    cmake = oslayer.cmake_program(studio)
    if not cmake:
        steps.result("core", "NOT DONE", "CMake was not found, neither on the PATH nor with Visual Studio")
        return False
    if steps.run([cmake, "-S", CORE, "-B", CORE_BUILD]) != 0:
        steps.result("core", "NOT DONE", "CMake could not set the build up: what it printed above says why")
        return False
    if steps.run([cmake, "--build", CORE_BUILD, "--config", CONFIG, "--parallel"]) != 0:
        steps.result("core", "NOT DONE", "the core did not build: what the compiler printed above says why (send it back)")
        return False
    inspector = oslayer.built_tool(CORE_BUILD, "vizcore_inspect")
    tests = oslayer.built_tool(CORE_BUILD, "vizcore_tests")
    if not os.path.isfile(tests) and not steps.dry_run:    # the published copy has no tests
        steps.result("core", "ok", f"built ({inspector})")
        return True
    code = steps.run([tests])
    if code == 0:
        steps.result("core", "ok", f"built, and its tests pass ({inspector})")
        return True
    steps.result("core", "NOT DONE", "built, but its tests fail: what they printed above says which (send it back)")
    return True


def unreal(steps, skip):
    steps.heading("3. The visualizer (the Unreal project)")
    if skip:
        steps.result("unreal", "skipped", "left as it is (--skip-unreal)")
        return
    engine = oslayer.default_engine()
    editor = oslayer.editor_binary(engine)
    if not os.path.isfile(editor) and not steps.dry_run:
        steps.result("unreal", "NOT DONE", f"Unreal Engine 5.8 was not found ({editor}): install it with the Epic Games Launcher, or set UE_ROOT to where it is")
        return
    if oslayer.project_open(PROJECT):
        steps.result("unreal", "NOT DONE", "Unreal has the project open: close it, then run this again")
        return
    log = os.path.join(REPO, "library", "logs", "build.log")
    os.makedirs(os.path.dirname(log), exist_ok=True)
    print(f"  This takes a while the first time (ten minutes to half an hour). What the build says goes to {log}.", flush=True)
    started = time.time()
    code = steps.run(oslayer.build_arguments(engine, PROJECT), log=log)
    minutes = (time.time() - started) / 60
    if code == 0:
        steps.result("unreal", "ok", f"built in {minutes:.0f} minutes")
    else:
        tail = ""
        try:
            with open(log, encoding="utf-8", errors="replace") as handle:
                lines = [line.rstrip() for line in handle if line.strip()]
            tail = "\n".join("    " + line for line in [line for line in lines if " error " in line.lower() or ": error" in line.lower()][:12] or lines[-12:])
        except OSError:
            pass
        steps.result("unreal", "NOT DONE", f"the build failed (code {code}); the whole of it is in {log}, which is what to send back" + (f":\n{tail}" if tail else ""))


def shortcut_script():
    """PowerShell that makes one shortcut from what it finds in its environment (no quoting to go wrong)."""
    return ("$s = (New-Object -ComObject WScript.Shell).CreateShortcut($env:OPENVIZ_LINK); "
            "$s.TargetPath = $env:OPENVIZ_TARGET; $s.Arguments = $env:OPENVIZ_ARGUMENTS; $s.WorkingDirectory = $env:OPENVIZ_FOLDER; "
            "$s.IconLocation = $env:OPENVIZ_ICON + ',0'; $s.Description = 'Openviz: show-lighting visualizer'; $s.Save()")


def windowless_python():
    """pythonw.exe beside the Python running this: Openviz without a console window."""
    beside = os.path.join(os.path.dirname(sys.executable), "pythonw.exe")
    return beside if os.path.isfile(beside) else sys.executable


def shortcut_places():
    """The Start menu's folder and the desktop, as this Windows user has them (the desktop may be in OneDrive)."""
    start_menu = os.path.join(os.environ.get("APPDATA", ""), "Microsoft", "Windows", "Start Menu", "Programs")
    desktop = ""
    try:
        done = subprocess.run(["powershell", "-NoProfile", "-Command", "[Environment]::GetFolderPath('Desktop')"],
                              capture_output=True, text=True, errors="replace", timeout=60)
        desktop = done.stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        pass
    return [(label, place) for label, place in (("in the Start menu", start_menu), ("on the desktop", desktop)) if place and os.path.isdir(place)]


def shortcuts(steps, skip):
    steps.heading("4. Openviz in the Start menu and on the desktop")
    if skip:
        steps.result("shortcuts", "skipped", "none made (--no-shortcuts)")
        return
    if not oslayer.WINDOWS and not steps.dry_run:
        steps.result("shortcuts", "skipped", "only on Windows")
        return
    places = shortcut_places() if oslayer.WINDOWS else [("in the Start menu", "Start menu"), ("on the desktop", "Desktop")]
    made = []
    for label, place in places:
        link = os.path.join(place, "Openviz.lnk")
        environment = dict(os.environ, OPENVIZ_LINK=link, OPENVIZ_TARGET=windowless_python(), OPENVIZ_FOLDER=REPO, OPENVIZ_ICON=ICON,
                           OPENVIZ_ARGUMENTS=subprocess.list2cmdline(["-X", "utf8", os.path.join(HERE, "launcher.py"), "--window"]))
        print(f"  {link}", flush=True)
        if steps.dry_run:
            made.append(label)
            continue
        try:
            done = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", shortcut_script()],
                                  env=environment, capture_output=True, text=True, errors="replace", timeout=60)
            if done.returncode == 0 and os.path.isfile(link):
                made.append(label)
            else:
                print("  " + (done.stderr.strip() or done.stdout.strip()), flush=True)
        except (OSError, subprocess.TimeoutExpired) as error:
            print(f"  {error}", flush=True)
    if made:
        steps.result("shortcuts", "ok", f"Openviz is {' and '.join(made)}: it opens in a window of its own")
    else:
        steps.result("shortcuts", "NOT DONE", "no shortcut could be made: start Openviz with launcher\\Openviz.bat instead")


def check(steps, skip):
    steps.heading("5. The system check")
    if skip:
        steps.result("check", "skipped", "not run (--no-check)")
        return
    code = steps.run([sys.executable, "-X", "utf8", os.path.join(REPO, "tools", "windows_check.py")])
    report = os.path.join(REPO, "windows-check.txt")
    steps.result("check", "ok" if code == 0 else "NOT DONE",
                 f"all well; the report is {report}" if code == 0 else f"some things are NOT OK (listed above); send back {report}")


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--skip-unreal", action="store_true", help="do not build the Unreal project (it is built already, or later)")
    p.add_argument("--no-shortcuts", action="store_true", help="do not put Openviz in the Start menu or on the desktop")
    p.add_argument("--no-check", action="store_true", help="do not run tools/windows_check.py at the end")
    p.add_argument("--dry-run", action="store_true", help="say what would be run, run nothing")
    args = p.parse_args(argv)
    if sys.version_info < (3, 9):
        print(f"Openviz needs Python 3.9 or newer; this is {sys.version.split()[0]}.")
        return 1
    steps = Steps(dry_run=args.dry_run)
    print(f"Setting Openviz up in {REPO}" + (" (dry run: nothing is run)" if args.dry_run else ""), flush=True)
    materials(steps)
    core(steps)
    unreal(steps, args.skip_unreal)
    shortcuts(steps, args.no_shortcuts)
    check(steps, args.no_check)
    print("\n== What came of it", flush=True)
    for step, outcome, what in steps.done:
        print(f"{outcome:8} {step}: {what.splitlines()[0]}", flush=True)
    missing = [step for step, outcome, _ in steps.done if outcome == "NOT DONE"]
    if not missing:
        print("\nOpenviz is ready: open it from the Start menu or the desktop.", flush=True)
    else:
        print(f"\nNot done yet: {', '.join(missing)}. Put right what is said above and run this again; it picks up where it stopped.", flush=True)
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
