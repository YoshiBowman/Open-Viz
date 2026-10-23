"""Builds the core's inspector (vizcore_inspect), through which Openviz reads shows and fixtures.

With CMake where there is one, as core/CMakeLists.txt says; on a Mac without it, from the same
sources with Apple's own compiler, so that a Mac needs nothing but Apple's developer tools
(owner, 2026-10-22: a ton of dependencies had to be fetched by hand on another Mac). The list of
sources and their definitions is read out of CMakeLists.txt, so that the two ways cannot drift
apart. Standard library only.

    python3 launcher/core_build.py          builds core/build/vizcore_inspect
"""

from __future__ import annotations

import concurrent.futures
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import oslayer  # noqa: E402


class BuildFailed(RuntimeError):
    """The core did not build; the message says where and why, in a few lines."""


def recipe(core):
    """The library's sources and the definitions CMakeLists.txt compiles them with."""
    with open(os.path.join(core, "CMakeLists.txt"), encoding="utf-8") as handle:
        text = handle.read()
    block = re.search(r"add_library\(\s*vizcore\s+STATIC(.*?)\)", text, re.S)
    defines = re.search(r"set\(\s*VIZCORE_THIRD_PARTY_DEFINITIONS(.*?)\)", text, re.S)
    if not block or not defines:
        raise BuildFailed("core/CMakeLists.txt does not list the library's sources as expected.")
    sources = [name for name in block.group(1).split() if name.endswith((".c", ".cpp"))]
    return sources, defines.group(1).split()


def direct_commands(core, out, compiler="clang++", c_compiler="clang"):
    """What compiling without CMake runs: one command a source file, then the link. Optimised with
    debugging information, as CMake's default here; C++17, no exceptions, no RTTI."""
    sources, defines = recipe(core)
    objects = os.path.join(out, "direct")
    includes = ["-I" + os.path.join(core, part) for part in ("include", "src", "third_party/pugixml", "third_party/miniz")]
    flags = ["-O2", "-g", "-DNDEBUG"] + ["-D" + name for name in defines] + includes
    compile_steps = []
    for source in sources + ["tools/inspect.cpp"]:
        target = os.path.join(objects, source.replace("/", "_") + ".o")
        if source.endswith(".c"):
            command = [c_compiler, "-c", *flags, os.path.join(core, source), "-o", target]
        else:
            command = [compiler, "-c", "-std=c++17", "-fno-exceptions", "-fno-rtti", *flags, os.path.join(core, source), "-o", target]
        compile_steps.append(command)
    link = [compiler, *[step[-1] for step in compile_steps], "-o", os.path.join(out, "vizcore_inspect")]
    return compile_steps, link


def cmake_commands(core, out, cmake):
    return [[cmake, "-S", core, "-B", out, "-DVIZCORE_BUILD_TESTS=OFF"], [cmake, "--build", out, "-j", "--target", "vizcore_inspect"]]


def build(repo=REPO, log=None, cmake=None, run=None):
    """Builds the inspector into core/build and says where it is. `cmake` defaults to whatever
    CMake there is; with none on a Mac, Apple's compiler does it directly. `log` (a file) takes
    everything the tools say."""
    core = os.path.join(repo, "core")
    out = os.path.join(core, "build")
    os.makedirs(out, exist_ok=True)
    run = run or _run
    cmake = cmake if cmake is not None else oslayer.cmake_program()
    if cmake:
        for command in cmake_commands(core, out, cmake):
            _check(run(command, log), command)
    elif oslayer.MAC:
        compile_steps, link = direct_commands(core, out, *_apple_compilers(run, log))
        os.makedirs(os.path.dirname(compile_steps[0][-1]), exist_ok=True)
        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
            for command, code in zip(compile_steps, pool.map(lambda c: run(c, log), compile_steps)):
                _check(code, command)
        _check(run(link, log), link)
    else:
        raise BuildFailed("CMake is needed to build Openviz's core here (Visual Studio brings it).")
    built = oslayer.built_tool(out, "vizcore_inspect")
    if not os.path.isfile(built):
        raise BuildFailed("the build ended, but no vizcore_inspect came out of it.")
    return built


def _apple_compilers(run, log):
    """Apple's compilers, as xcrun finds them (in Xcode or in the Command Line Tools)."""
    found = []
    for name in ("clang++", "clang"):
        try:
            path = subprocess.run(["xcrun", "-f", name], capture_output=True, text=True, timeout=30).stdout.strip()
        except (OSError, subprocess.TimeoutExpired):
            path = ""
        if not path:
            raise BuildFailed("Apple's compiler was not found: install the Command Line Tools (System › Setup in Openviz).")
        found.append(path)
    return found


def _run(command, log):
    """Runs one step; `log`, a file opened for bytes, takes what it says."""
    try:
        done = subprocess.run(command, stdout=log or subprocess.DEVNULL, stderr=subprocess.STDOUT if log else subprocess.DEVNULL)
    except OSError as error:
        if log:
            log.write(f"{command[0]}: {error}\n".encode())
        return 127
    return done.returncode


def _check(code, command):
    if code != 0:
        raise BuildFailed(f"{os.path.basename(command[0])} stopped with code {code} ({' '.join(os.path.basename(part) for part in command[-3:])}).")


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    try:
        built = build(log=sys.stdout.buffer if "--verbose" in argv else None)
    except BuildFailed as error:
        print(f"The core did not build: {error}", file=sys.stderr)
        return 1
    print(built)
    return 0


if __name__ == "__main__":
    sys.exit(main())
