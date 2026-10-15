#!/usr/bin/env python3
"""A show packed into one file, for taking it to another computer or keeping it safe.

A show in Openviz is its rig file (the MVR, never changed) and what is kept beside it: the edits,
the stand-in types, the set pieces, the screens, the hidden layers, the stored views. Some of that
points outside the show: fixture types of Openviz's fixture library (stand-ins, added fixtures,
fixtures given a type of their own) and the pictures and films screens show. A package holds all
of it, so that on another computer the show opens as it was here.

The file is a ZIP archive ending in ".openviz":

  openviz-package.json     what is in it: the show's name, when it was made, the fixture types,
                           which media file was where on the computer it was made on
  show/<show>.mvr          the rig file as it came
  show/<show><ending>      each side file there is (".edits.xml", ".types.xml", ...)
  fixtures/<file>.gdtf     the fixture library's files the show uses
  models/<file>            the model files its set pieces are, with what an OBJ names beside it
  media/<n> <name>         the pictures and films its screens show

Only those places are read back out of a package, each by its own plain name: nothing in a
package can write anywhere else.
"""
from __future__ import annotations

import json
import os
import re
import time
import zipfile

FORMAT = "Openviz show package"
VERSION = 1
MANIFEST = "openviz-package.json"
EXTENSION = ".openviz"
SIDE_ENDINGS = (".edits.xml", ".types.xml", ".pieces.json", ".screens.json", ".hidden.json", ".views.txt", ".paper.json")


class PackageError(Exception):
    pass


def write(out_path, mvr_path, fixture_files, media_files, made_by="Openviz", model_files=()):
    """Packs a show. `fixture_files` are paths of library files, `media_files` paths of pictures
    and films. Returns what went in. Written beside `out_path` first, then put in its place."""
    show = os.path.basename(mvr_path)
    stem = show[:-4]
    media = {}
    with zipfile.ZipFile(out_path + ".part", "w") as archive:
        # What is compressed already (the MVR, GDTF, pictures, films) is stored as it is.
        archive.write(mvr_path, f"show/{show}", compress_type=zipfile.ZIP_STORED)
        sides = []
        for ending in SIDE_ENDINGS:
            side = mvr_path[:-4] + ending
            if os.path.isfile(side):
                archive.write(side, f"show/{stem}{ending}", compress_type=zipfile.ZIP_DEFLATED)
                sides.append(ending)
        fixtures = []
        for path in fixture_files:
            name = os.path.basename(path)
            if name not in fixtures:
                archive.write(path, f"fixtures/{name}", compress_type=zipfile.ZIP_STORED)
                fixtures.append(name)
        models = []
        for path in model_files:
            name = os.path.basename(path)
            if name not in models:
                archive.write(path, f"models/{name}", compress_type=zipfile.ZIP_DEFLATED)
                models.append(name)
        for number, path in enumerate(dict.fromkeys(media_files), start=1):
            stored = f"{number} {os.path.basename(path)}"
            archive.write(path, f"media/{stored}", compress_type=zipfile.ZIP_STORED)
            media[stored] = path
        manifest = {"format": FORMAT, "version": VERSION, "show": show, "made": time.strftime("%Y-%m-%d %H:%M"), "made_by": made_by,
                    "side_files": sides, "fixtures": fixtures, "media": media, "models": models}
        archive.writestr(MANIFEST, json.dumps(manifest, indent=1), compress_type=zipfile.ZIP_DEFLATED)
    os.replace(out_path + ".part", out_path)
    return manifest


def read(path):
    """What a package holds, checked: its manifest, with every name in it a plain one."""
    try:
        with zipfile.ZipFile(path) as archive:
            names = set(archive.namelist())
            manifest = json.loads(archive.read(MANIFEST).decode("utf-8"))
    except (zipfile.BadZipFile, OSError, KeyError, ValueError, UnicodeDecodeError):
        raise PackageError("it is damaged, or is not an Openviz package") from None
    if not isinstance(manifest, dict) or manifest.get("format") != FORMAT:
        raise PackageError("it is not an Openviz package")
    if not isinstance(manifest.get("version"), int) or manifest["version"] > VERSION:
        raise PackageError("it was made by a newer Openviz: update this one first")
    show = manifest.get("show")
    if not plain(show) or not show.lower().endswith(".mvr") or f"show/{show}" not in names:
        raise PackageError("it holds no show")
    sides = [ending for ending in manifest.get("side_files", []) if ending in SIDE_ENDINGS and f"show/{show[:-4]}{ending}" in names]
    fixtures = [name for name in manifest.get("fixtures", []) if plain(name) and name.lower().endswith(".gdtf") and f"fixtures/{name}" in names]
    media = {stored: original for stored, original in (manifest.get("media") or {}).items()
             if plain(stored) and isinstance(original, str) and f"media/{stored}" in names}
    models = [name for name in manifest.get("models", []) if plain(name) and f"models/{name}" in names]
    return dict(manifest, side_files=sides, fixtures=fixtures, media=media, models=models)


def plain(name):
    """A file name with no folder in it and nothing that could climb out of one."""
    return isinstance(name, str) and bool(name) and name not in (".", "..") and not re.search(r"[/\\\x00]", name) and name == name.strip()


def extract(path, entry, target):
    """Copies one entry of the package to `target`, by way of a neighbour."""
    with zipfile.ZipFile(path) as archive, archive.open(entry) as source, open(target + ".part", "wb") as out:
        while True:
            chunk = source.read(1 << 20)
            if not chunk:
                break
            out.write(chunk)
    os.replace(target + ".part", target)


def entry_bytes(path, entry):
    with zipfile.ZipFile(path) as archive:
        return archive.read(entry)
