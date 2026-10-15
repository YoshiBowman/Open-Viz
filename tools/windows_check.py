"""Looks over a computer that Openviz is to run on, and writes down what it finds.

Openviz is developed on a Mac. Everything that had to be written for Windows without a Windows
computer at hand is tried here, one thing at a time, and what does not hold is said plainly. Run
it from the project's folder and send back the file it writes:

    python tools\\windows_check.py          (Windows)
    python3 tools/windows_check.py          (anywhere else: the same checks, for comparison)

It starts nothing but short-lived helpers of its own, sends a few packets to this computer itself
on ports nobody uses, and changes nothing. Standard library only.
"""

import os
import platform
import shutil
import socket
import subprocess
import sys
import time
import traceback
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "launcher"))

import oslayer  # noqa: E402

LINES = []
COUNTS = {"ok": 0, "NOT OK": 0, "note": 0}


def say(kind, text):
    """One finding: "ok", "NOT OK" (something Openviz needs does not hold) or "note" (worth knowing)."""
    COUNTS[kind] += 1
    line = f"{kind:7} {text}"
    LINES.append(line)
    print(line, flush=True)


def heading(text):
    LINES.append("")
    LINES.append(f"== {text}")
    print(f"\n== {text}", flush=True)


def attempt(what, check):
    """Runs one check; whatever goes wrong inside it is a finding, not the end of the run."""
    try:
        check()
    except Exception as error:    # noqa: BLE001 (the point is to go on)
        say("NOT OK", f"{what}: the check itself failed: {type(error).__name__}: {error}")
        LINES.extend("        " + line for line in traceback.format_exc().splitlines()[-4:])


def run(command, timeout=60):
    try:
        done = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=timeout)
        return done.returncode, (done.stdout + done.stderr).strip()
    except (OSError, subprocess.TimeoutExpired) as error:
        return None, str(error)


# ------------------------------------------------------------------------------------- checks

def the_system():
    say("note", f"{platform.system()} {platform.release()} ({platform.version()}), {platform.machine()}")
    say("ok" if sys.version_info >= (3, 9) else "NOT OK", f"Python {platform.python_version()} at {sys.executable} (3.9 or newer is needed)")
    say("note", f"the project's folder: {REPO}")
    if any(ord(ch) > 126 for ch in REPO):
        say("note", "the project's folder has letters outside plain English in its path: allowed for since 2026-10-15 (the core opens files by UTF-8 paths), but not yet tried in Unreal on Windows")
    elif " " in REPO:
        say("note", "the project's folder has a space in its path: allowed for, but the first thing to suspect if a show will not open")


def the_tools():
    found = shutil.which("git")
    say("ok" if found else "NOT OK", f"git: {found or 'not found'} (to fetch updates)")
    found = oslayer.cmake_program()
    say("ok" if found else "NOT OK", f"cmake: {found or 'not found, neither on the PATH nor with Visual Studio'} (to build the core)")
    code, out = run(["git", "lfs", "version"])
    say("ok" if code == 0 else "NOT OK", f"git lfs: {out.splitlines()[0] if code == 0 and out else 'not installed'} (the material files are kept with it)")
    material = os.path.join(REPO, "unreal", "Plugins", "LightingVisualizer", "Content", "Materials", "M_LvBeam.uasset")
    try:
        with open(material, "rb") as handle:
            start = handle.read(40)
        if start.startswith(b"version https://git-lfs"):
            say("NOT OK", "the material files are still Git LFS pointers: run 'git lfs pull' in the project's folder")
        else:
            say("ok", f"the material files are here ({os.path.getsize(material):,} bytes for the beam)")
    except OSError as error:
        say("NOT OK", f"the beam material cannot be read: {error}")
    if oslayer.WINDOWS:
        studio = oslayer.visual_studio()
        say("ok" if studio else "NOT OK", f"Visual Studio with the C++ tools: {studio}" if studio
            else "Visual Studio with the C++ tools is not installed (Unreal and the core are built with it; the 'Game development with C++' workload)")


def the_engine():
    engine = oslayer.default_engine()
    editor = oslayer.editor_binary(engine)
    say("ok" if os.path.exists(editor) else "NOT OK", f"Unreal Engine 5.8: {editor} {'is there' if os.path.exists(editor) else 'is not there (install it with the Epic Games Launcher, or set UE_ROOT)'}")
    ndi = os.path.join(engine, "Engine", "Plugins", "Media", "NDIMedia", "Binaries", "ThirdParty")
    libraries = [os.path.join(root, name) for root, _, names in os.walk(ndi) for name in names if name.lower().endswith((".dll", ".dylib", ".so"))] if os.path.isdir(ndi) else []
    say("ok" if libraries else "note", f"the engine's NDI library: {libraries[0] if libraries else 'not found: screens will not show NDI streams'}")
    print("  " + oslayer.build_command(engine, os.path.join(REPO, "unreal", "LightingViz.uproject")))
    LINES.append("        to build the Unreal project: " + oslayer.build_command(engine, os.path.join(REPO, "unreal", "LightingViz.uproject")))
    binaries = os.path.join(REPO, "unreal", "Plugins", "LightingVisualizer", "Binaries")
    built = os.path.isdir(binaries) and any("LvShow" in name for _, _, names in os.walk(binaries) for name in names)
    say("ok" if built else "NOT OK", "the Unreal project is built" if built else "the Unreal project is not built yet: run the line above")


def the_core():
    inspector = oslayer.built_tool(os.path.join(REPO, "core", "build"), "vizcore_inspect")
    if not os.path.isfile(inspector):
        say("NOT OK", "the core is not built: run launcher\\setup_windows.bat, or  cmake -S core -B core/build  and then  cmake --build core/build --config RelWithDebInfo")
        return
    code, out = run([inspector])
    say("ok" if code == 2 else "NOT OK", f"the core's inspector runs: {inspector}" if code == 2 else f"the core's inspector does not run as expected (code {code}): {out[:200]}")
    tests = oslayer.built_tool(os.path.join(REPO, "core", "build"), "vizcore_tests")
    if os.path.isfile(tests):
        code, out = run([tests], timeout=600)
        say("ok" if code == 0 else "NOT OK", f"the core's own tests: {out.splitlines()[-1] if out else 'no output'}")
    else:
        say("note", "the core's tests are not built")


def processes():
    child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(20)"])
    try:
        asked = oslayer.process_alive(child.pid)
        time.sleep(0.3)
        still = child.poll() is None
        say("ok" if asked and still else "NOT OK",
            "asking whether a process runs says yes and leaves it running" if asked and still
            else f"asking whether a process runs: answered {asked}, and the process is {'still there' if still else 'GONE: the question ended it'}")
    finally:
        child.kill()
        child.wait()
    gone = oslayer.process_alive(child.pid)
    say("ok" if not gone else "NOT OK", "an ended process is reported as ended" if not gone else "an ended process is still reported as running")
    # Arguments with spaces must arrive whole, written the way Unreal wants them.
    show = os.path.join("C:\\Users\\Jo Smith\\shows" if oslayer.WINDOWS else "/Users/Jo Smith/shows", "Summer tour.mvr")
    started = oslayer.start([sys.executable, "-c", "import sys; print(sys.argv[1:])", f"-LvShow={show}", "-game"], stdout=subprocess.PIPE, text=True)
    printed = started.communicate(timeout=30)[0].strip()
    say("ok" if printed == repr([f"-LvShow={show}", "-game"]) else "NOT OK", f"a path with spaces reaches a started program whole: {printed}")
    if oslayer.WINDOWS:
        say("note", "as one line for Unreal: " + oslayer.windows_command_line(["UnrealEditor.exe", f"-LvShow={show}", "-game"]))


def bind(address, port, broadcast=False):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, "SO_REUSEPORT"):
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    if broadcast:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.bind((address, port))
    sock.settimeout(0.4)
    return sock


def heard(sock, wanted):
    try:
        while True:
            data, _ = sock.recvfrom(2048)
            if data == wanted:
                return True
    except (socket.timeout, OSError):
        return False


def free_port():
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.bind(("", 0))
    port = probe.getsockname()[1]
    probe.close()
    return port


def sharing_a_port(addresses):
    """Openviz receives Art-Net on every address of a port and hands it to Unreal on 127.0.0.1 at
    the same port. That only works if the system gives what is sent to 127.0.0.1 to the socket
    bound there, and everything else to the one bound to every address."""
    for first in ("every address first", "127.0.0.1 first"):
        port = free_port()
        try:
            if first == "every address first":
                everywhere = bind("", port, broadcast=True)
                loopback = bind("127.0.0.1", port)
            else:
                loopback = bind("127.0.0.1", port)
                everywhere = bind("", port, broadcast=True)
        except OSError as error:
            say("NOT OK", f"two listeners on one port ({first}): the second was refused: {error}. Openviz could not pass Art-Net on to Unreal this way.")
            continue
        sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sender.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        try:
            findings = []
            for target in ["127.0.0.1"] + [address for address in addresses if address != "127.0.0.1"] + ["255.255.255.255"]:
                token = f"openviz-check {first} {target} {time.time()}".encode()
                try:
                    sender.sendto(token, (target, port))
                except OSError as error:
                    findings.append((target, f"could not be sent to: {error}"))
                    continue
                at_loopback, at_everywhere = heard(loopback, token), heard(everywhere, token)
                got = "both" if at_loopback and at_everywhere else "the 127.0.0.1 listener" if at_loopback else "the every-address listener" if at_everywhere else "nobody"
                findings.append((target, got))
            for target, got in findings:
                wanted = "the 127.0.0.1 listener" if target == "127.0.0.1" else "the every-address listener"
                if got == wanted:
                    say("ok", f"sharing a port ({first}): what is sent to {target} goes to {got}")
                elif target == "255.255.255.255" and got == "nobody":
                    say("note", f"sharing a port ({first}): a broadcast from this computer to itself was heard by nobody (often so; a console's broadcast is another matter)")
                else:
                    say("NOT OK", f"sharing a port ({first}): what is sent to {target} goes to {got}, not to {wanted}. Art-Net through Openviz needs another way on this system.")
        finally:
            for sock in (sender, loopback, everywhere):
                sock.close()


def multicast(addresses):
    """sACN arrives by multicast, on the network it was sent on: joined per address of this computer."""
    for address in [entry for entry in addresses if entry != "127.0.0.1"] or ["127.0.0.1"]:
        port = free_port()
        group = "239.255.0.201"    # sACN's group for universe 201
        try:
            listener = bind("", port)
            listener.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, socket.inet_aton(group) + socket.inet_aton(address))
            sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sender.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(address))
            sender.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
            token = f"openviz-check multicast {address} {time.time()}".encode()
            sender.sendto(token, (group, port))
            got = heard(listener, token)
            say("ok" if got else "NOT OK", f"multicast on {address}: {'joined, sent and heard' if got else 'sent but not heard (a firewall, or the network adapter is down)'}")
            sender.close()
            listener.close()
        except OSError as error:
            say("NOT OK", f"multicast on {address}: {error}")


def the_network():
    addresses = oslayer.local_addresses()
    say("ok" if len(addresses) > 1 else "note", f"this computer's addresses: {', '.join(addresses)}" + ("" if len(addresses) > 1 else " (only itself: no network found)"))
    attempt("sharing a port", lambda: sharing_a_port(addresses))
    attempt("multicast", lambda: multicast(addresses))
    import ndi_find
    streams = ndi_find.find(1.5, addresses)
    say("note", f"NDI streams on the network right now: {', '.join(streams) if streams else 'none'}")
    if oslayer.WINDOWS:
        say("note", "the first time Openviz and Unreal listen on the network, Windows asks whether to allow them: allow both on private networks")


def surroundings():
    country, inches = oslayer.region()
    say("note", f"region: {country or 'not said'}; measures in {'inches' if inches else 'metres' if inches is False else 'whatever that country uses'}")
    command = oslayer.choose_file_command("film")
    say("ok" if command and shutil.which(command[0]) else "note",
        f"the file window is opened with {command[0]}" if command and shutil.which(command[0]) else "no file window here: a film's path is typed in")
    window = oslayer.app_window_command("http://127.0.0.1:1/", os.path.join(REPO, "library", "logs", "window"))
    if oslayer.WINDOWS:
        say("ok" if window else "note", f"Openviz's own window: Edge at {window[0]}" if window else "Edge was not found: Openviz opens in the browser instead")
        try:
            import ctypes
            ctypes.WinDLL("advapi32").CredReadW    # noqa: B018 (looked up, not called: nothing is read or written here)
            say("ok", "Windows' Credential Manager is there for 'Stay logged in'")
        except (OSError, AttributeError) as error:
            say("NOT OK", f"Windows' Credential Manager cannot be reached ({error}): 'Stay logged in' will not keep the password")
    converter = oslayer.video_converter("in.mov", "out.mp4")
    say("ok" if converter else "note", f"videos are made into MP4s with {converter[0]}" if converter
        else "videos are kept as recorded (JPEG pictures): install FFmpeg (winget install ffmpeg) to have them made into MP4s")


def own_tests():
    """The launcher's tests that do not need a Mac: the PDF writer, the plot, NDI discovery, recordings, this module's helpers."""
    names = [name for name in ("test_pdf", "test_paperwork", "test_ndi_find", "test_oslayer", "test_dmx_record")
             if os.path.isfile(os.path.join(REPO, "launcher", name + ".py"))]
    if not names:
        say("note", "Openviz's own tests are not part of this copy")
        return
    loader = unittest.TestLoader()
    suite = unittest.TestSuite()
    for name in names:
        try:
            suite.addTests(loader.loadTestsFromName(name))
        except Exception as error:    # noqa: BLE001
            say("NOT OK", f"{name} could not be loaded: {error}")
    result = unittest.TestResult()
    suite.run(result)
    bad = result.failures + result.errors
    say("ok" if not bad else "NOT OK", f"Openviz's own tests that fit any system: {result.testsRun} run, {len(bad)} failed, {len(result.skipped)} skipped")
    for test, text in bad[:12]:
        LINES.append(f"        {test}: {text.strip().splitlines()[-1]}")
        print(f"        {test}: {text.strip().splitlines()[-1]}")


def main():
    started = time.strftime("%Y-%m-%d %H:%M:%S")
    for title, check in (("This computer", the_system), ("Tools", the_tools), ("Unreal Engine", the_engine), ("The core", the_core),
                         ("Starting and watching programs", processes), ("The network", the_network), ("Other things", surroundings),
                         ("Openviz's own tests", own_tests)):
        heading(title)
        attempt(title, check)
    summary = f"{COUNTS['ok']} ok, {COUNTS['NOT OK']} NOT OK, {COUNTS['note']} notes"
    out = os.path.join(REPO, "windows-check.txt")
    try:
        with open(out, "w", encoding="utf-8") as handle:
            handle.write(f"Openviz system check, {started}\n{summary}\n" + "\n".join(LINES) + "\n")
        print(f"\n{summary}\nWritten to {out}: send that file back.")
    except OSError as error:
        print(f"\n{summary}\nThe findings could not be written to a file ({error}): copy what is above instead.")
    return 1 if COUNTS["NOT OK"] else 0


if __name__ == "__main__":
    sys.exit(main())
